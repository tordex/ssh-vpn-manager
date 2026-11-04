#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "ssh_tunnel.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "socks5.hpp"

namespace {

constexpr std::size_t kMaxBuffered = 1 << 20;  // 1 MiB per-direction cap (backpressure)

const std::string& success_reply() {
  static const std::string s("\x05\x00\x00\x01\x00\x00\x00\x00\x00\x00", 10);
  return s;
}
const std::string& fail_reply() {
  static const std::string s("\x05\x01\x00\x01\x00\x00\x00\x00\x00\x00", 10);
  return s;
}

struct Forwarder {
  enum class State { Handshake, Opening, Stream };

  int fd = -1;
  ssh_channel ch = nullptr;
  socks5::Stage hs_stage = socks5::Stage::Greeting;
  State state = State::Handshake;
  std::string in;         // buffered bytes for the SOCKS handshake
  std::string to_local;   // pending writes to the local socket
  std::string to_remote;  // pending writes to the SSH channel
  std::string target_host;
  std::uint16_t target_port = 0;
  std::uint64_t tx = 0;
  std::uint64_t rx = 0;
  bool dead = false;
};

void close_forwarder(Forwarder& f) {
  if (f.fd >= 0) {
    close(f.fd);
    f.fd = -1;
  }
  if (f.ch) {
    ssh_channel_free(f.ch);
    f.ch = nullptr;
  }
}

}  // namespace

TunnelWorker::TunnelWorker(TunnelTarget target, TunnelSettings settings,
                           std::function<void()> notify)
    : target_(std::move(target)), settings_(std::move(settings)),
      notify_(std::move(notify)) {}

TunnelWorker::~TunnelWorker() {
  stop();
  join();
  if (stop_pipe_[0] >= 0) close(stop_pipe_[0]);
  if (stop_pipe_[1] >= 0) close(stop_pipe_[1]);
}

void TunnelWorker::start() {
  if (pipe2(stop_pipe_, O_NONBLOCK | O_CLOEXEC) != 0) {
    set_status(TunnelStatus::Error, "failed to create control pipe");
    return;
  }
  thread_ = std::thread(&TunnelWorker::run, this);
}

void TunnelWorker::stop() {
  bool expected = false;
  if (!stop_requested_.compare_exchange_strong(expected, true)) {
    return;
  }
  if (stop_pipe_[1] >= 0) {
    const char c = 1;
    ssize_t r = ::write(stop_pipe_[1], &c, 1);
    (void)r;
  }
}

void TunnelWorker::join() {
  if (thread_.joinable()) {
    thread_.join();
  }
}

TunnelStats TunnelWorker::stats() const {
  std::lock_guard<std::mutex> lk(stats_mutex_);
  return stats_;
}

void TunnelWorker::set_status(TunnelStatus s, const std::string& error) {
  {
    std::lock_guard<std::mutex> lk(stats_mutex_);
    stats_.status = s;
    stats_.error = error;
    stats_.tx_bytes = tx_total_;
    stats_.rx_bytes = rx_total_;
    stats_.ping_ms = ping_ms_;
  }
  if (notify_) notify_();
}

void TunnelWorker::publish_stats(std::size_t active_connections) {
  const auto now = std::chrono::steady_clock::now();
  if (now - last_notify_ < std::chrono::milliseconds(500)) {
    return;
  }
  last_notify_ = now;
  {
    std::lock_guard<std::mutex> lk(stats_mutex_);
    stats_.tx_bytes = tx_total_;
    stats_.rx_bytes = rx_total_;
    stats_.ping_ms = ping_ms_;
    stats_.active_connections = static_cast<int>(active_connections);
  }
  if (notify_) notify_();
}

bool TunnelWorker::wait_interruptible(int timeout_ms) {
  pollfd p{stop_pipe_[0], POLLIN, 0};
  while (!stop_requested_.load()) {
    int rc = poll(&p, 1, timeout_ms);
    if (rc < 0) {
      if (errno == EINTR) continue;
      return true;
    }
    if (rc == 0) return false;  // timeout
    if (p.revents & POLLIN) return true;
  }
  return true;
}

bool TunnelWorker::connect_and_auth() {
  session_ = ssh_new();
  if (!session_) {
    set_status(TunnelStatus::Reconnecting, "failed to allocate SSH session");
    return false;
  }

  unsigned int port = static_cast<unsigned int>(target_.port);
  long timeout_s = settings_.connect_timeout_sec;
  long timeout_us = 0;
  ssh_options_set(session_, SSH_OPTIONS_HOST, target_.host.c_str());
  ssh_options_set(session_, SSH_OPTIONS_PORT, &port);
  ssh_options_set(session_, SSH_OPTIONS_USER, target_.login.c_str());
  ssh_options_set(session_, SSH_OPTIONS_TIMEOUT, &timeout_s);
  ssh_options_set(session_, SSH_OPTIONS_TIMEOUT_USEC, &timeout_us);

  if (ssh_connect(session_) != SSH_OK) {
    const std::string msg = "connect failed: " + std::string(ssh_get_error(session_));
    ssh_free(session_);
    session_ = nullptr;
    set_status(TunnelStatus::Reconnecting, msg);
    return false;
  }

  const int rc = ssh_userauth_password(session_, nullptr, target_.password.c_str());
  if (rc != SSH_AUTH_SUCCESS) {
    const std::string msg = "authentication failed: " + std::string(ssh_get_error(session_));
    ssh_disconnect(session_);
    ssh_free(session_);
    session_ = nullptr;
    set_status(TunnelStatus::Reconnecting, msg);
    return false;
  }

  ssh_set_blocking(session_, 0);
  return true;
}

void TunnelWorker::run_connected() {
  using clock = std::chrono::steady_clock;
  using namespace std::chrono;

  const int session_fd = ssh_get_fd(session_);
  std::vector<Forwarder> forwards;

  ssh_channel ping_ch = nullptr;
  bool ping_opening = false;
  auto ping_start = clock::now();
  const int ping_timeout_ms =
      std::clamp(settings_.ping_interval_sec * 1000, 1000, 5000);
  auto ping_next = clock::now() + seconds(std::max(1, settings_.ping_interval_sec));

  while (!stop_requested_.load()) {
    if (ssh_is_connected(session_) == 0) {
      break;
    }

    // Build the poll fd set.
    std::vector<pollfd> pfds;
    pfds.reserve(forwards.size() + 3);
    pfds.push_back(pollfd{stop_pipe_[0], POLLIN, 0});
    pfds.push_back(pollfd{listener_fd_, POLLIN, 0});

    bool sess_want_write = ping_opening;
    for (auto& f : forwards) {
      short ev = POLLIN;
      if (!f.to_local.empty()) ev |= POLLOUT;
      if (f.state == Forwarder::State::Opening || !f.to_remote.empty()) {
        sess_want_write = true;
      }
      pfds.push_back(pollfd{f.fd, ev, 0});
    }
    short sess_ev = POLLIN;
    if (sess_want_write) sess_ev |= POLLOUT;
    pfds.push_back(pollfd{session_fd, sess_ev, 0});
    const std::size_t sess_i = pfds.size() - 1;

    const auto now = clock::now();
    int timeout_ms = 1000;
    if (settings_.ping_interval_sec > 0) {
      const long long remain = duration_cast<milliseconds>(ping_next - now).count();
      timeout_ms = std::min(timeout_ms, (int)std::clamp<long long>(remain, 0, 1000));
    }

    const int rc = poll(pfds.data(), static_cast<nfds_t>(pfds.size()), timeout_ms);
    if (rc < 0) {
      if (errno == EINTR) continue;
      break;
    }

    if (pfds[0].revents & (POLLIN | POLLHUP | POLLERR)) break;         // stop
    if (pfds[sess_i].revents & (POLLERR | POLLHUP | POLLNVAL)) break;  // lost

    // Accept new local connections (only while connected).
    if (pfds[1].revents & POLLIN) {
      for (;;) {
        const int c = accept4(listener_fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (c < 0) {
          if (errno == EINTR) continue;
          break;  // EAGAIN or real error
        }
        Forwarder f;
        f.fd = c;
        forwards.push_back(std::move(f));
      }
    }

    for (auto& f : forwards) {
      // 1) Complete the direct-tcpip channel open (nonblocking).
      if (f.state == Forwarder::State::Opening && f.ch) {
        const int o = ssh_channel_open_forward(f.ch, f.target_host.c_str(),
                                               f.target_port, "127.0.0.1",
                                               settings_.local_port);
        if (o == SSH_OK) {
          f.to_local += success_reply();
          f.state = Forwarder::State::Stream;
        } else if (o == SSH_ERROR) {
          f.to_local += fail_reply();
          f.dead = true;
        }
        // SSH_AGAIN: retry on the next wake.
      }

      // 2) Drain channel -> local.
      if (f.state == Forwarder::State::Stream && !f.dead && f.ch) {
        for (int k = 0; k < 128 && f.to_local.size() < kMaxBuffered; ++k) {
          char buf[16384];
          const int n = ssh_channel_read_nonblocking(f.ch, buf, sizeof buf, 0);
          if (n > 0) {
            f.to_local.append(buf, static_cast<std::size_t>(n));
            f.rx += static_cast<std::uint64_t>(n);
          } else if (n == SSH_EOF || n == SSH_ERROR) {
            f.dead = true;
            break;
          } else {
            break;  // SSH_AGAIN / no data
          }
        }
      }

      // 3) Flush local -> channel.
      if (f.state == Forwarder::State::Stream && !f.dead && f.ch &&
          !f.to_remote.empty()) {
        std::size_t off = 0;
        for (int k = 0; k < 128 && off < f.to_remote.size(); ++k) {
          const int n = ssh_channel_write(
              f.ch, f.to_remote.data() + off,
              static_cast<std::uint32_t>(f.to_remote.size() - off));
          if (n > 0) {
            off += static_cast<std::size_t>(n);
          } else if (n == SSH_ERROR) {
            f.dead = true;
            break;
          } else {
            break;  // SSH_AGAIN
          }
        }
        if (off > 0) {
          f.tx += static_cast<std::uint64_t>(off);
          f.to_remote.erase(0, off);
        }
      }

      // 4) Read local socket -> handshake / channel.
      if (!f.dead) {
        char buf[16384];
        const ssize_t n = recv(f.fd, buf, sizeof buf, 0);
        if (n > 0) {
          if (f.state == Forwarder::State::Handshake) {
            f.in.append(buf, static_cast<std::size_t>(n));
            if (f.in.size() > 4096) {
              f.to_local += fail_reply();
              f.dead = true;
            } else {
              for (;;) {
                const socks5::Result r = socks5::process(f.hs_stage, f.in);
                if (r.kind == socks5::Result::Kind::NeedMore) break;
                if (r.kind == socks5::Result::Kind::Greeting) {
                  f.to_local += r.reply;
                  f.in.erase(0, r.consumed);
                  continue;
                }
                if (r.kind == socks5::Result::Kind::Fail) {
                  f.to_local += r.reply;
                  f.in.erase(0, r.consumed);
                  f.dead = true;
                  break;
                }
                // Kind::Connect
                f.in.erase(0, r.consumed);
                f.target_host = r.host;
                f.target_port = r.port;
                f.ch = ssh_channel_new(session_);
                f.state = Forwarder::State::Opening;
                break;
              }
            }
          } else if (f.state == Forwarder::State::Stream &&
                     f.to_remote.size() < kMaxBuffered) {
            f.to_remote.append(buf, static_cast<std::size_t>(n));
          }
        } else if (n == 0) {
          f.dead = true;
          f.to_local.clear();  // client went away; nothing left to flush
        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
          f.dead = true;
          f.to_local.clear();
        }
      }

      // 5) Flush local <- channel replies/data.
      if (!f.to_local.empty()) {
        const ssize_t n = send(f.fd, f.to_local.data(), f.to_local.size(), MSG_NOSIGNAL);
        if (n > 0) {
          f.to_local.erase(0, static_cast<std::size_t>(n));
        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
          f.dead = true;
          f.to_local.clear();
        }
      }
    }

    // Accumulate per-connection byte counts and drop finished forwarders.
    for (auto& f : forwards) {
      tx_total_ += f.tx;
      rx_total_ += f.rx;
      f.tx = 0;
      f.rx = 0;
    }
    forwards.erase(std::remove_if(forwards.begin(), forwards.end(),
                                  [](const Forwarder& f) {
                                    return f.dead && f.to_local.empty();
                                  }),
                   forwards.end());

    // Ping probe (measures tunnel RTT to ping_host via direct-tcpip).
    const auto now2 = clock::now();
    if (settings_.ping_interval_sec > 0) {
      if (!ping_opening) {
        if (now2 >= ping_next) {
          ping_next = now2 + seconds(settings_.ping_interval_sec);
          if (ping_ch) ssh_channel_free(ping_ch);
          ping_ch = ssh_channel_new(session_);
          if (ping_ch) {
            ping_opening = true;
            ping_start = now2;
          }
        }
      } else if (ping_ch) {
        const int o = ssh_channel_open_forward(ping_ch, settings_.ping_host.c_str(),
                                               settings_.ping_port, "127.0.0.1",
                                               settings_.local_port);
        if (o == SSH_OK) {
          ping_ms_ = duration<double, std::milli>(now2 - ping_start).count();
          ssh_channel_free(ping_ch);
          ping_ch = nullptr;
          ping_opening = false;
        } else if (o == SSH_ERROR) {
          ping_ms_ = -1.0;
          ssh_channel_free(ping_ch);
          ping_ch = nullptr;
          ping_opening = false;
        } else if (duration_cast<milliseconds>(now2 - ping_start).count() > ping_timeout_ms) {
          ping_ms_ = -1.0;
          ssh_channel_free(ping_ch);
          ping_ch = nullptr;
          ping_opening = false;
        }
      } else {
        ping_opening = false;
      }
    }

    publish_stats(forwards.size());
  }

  if (ping_ch) ssh_channel_free(ping_ch);
  for (auto& f : forwards) close_forwarder(f);
  forwards.clear();
}

void TunnelWorker::run() {
  set_status(TunnelStatus::Connecting);

  // Bind the loopback listener once, up front. A bind failure (port in use) is a
  // permanent error: report it and wait until the user stops the tunnel.
  int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    set_status(TunnelStatus::Error,
               "socket() failed: " + std::string(std::strerror(errno)));
    wait_interruptible(-1);
    set_status(TunnelStatus::Stopped);
    return;
  }
  const int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(static_cast<std::uint16_t>(settings_.local_port));
  if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0 ||
      listen(fd, 64) < 0) {
    const std::string msg = "cannot bind 127.0.0.1:" +
                            std::to_string(settings_.local_port) + " (" +
                            std::strerror(errno) + ")";
    close(fd);
    set_status(TunnelStatus::Error, msg);
    wait_interruptible(-1);
    set_status(TunnelStatus::Stopped);
    return;
  }
  listener_fd_ = fd;

  int backoff_ms = 1000;
  constexpr int kMaxBackoffMs = 30000;

  while (!stop_requested_.load()) {
    set_status(TunnelStatus::Connecting);
    if (connect_and_auth()) {
      set_status(TunnelStatus::Connected);
      backoff_ms = 1000;
      run_connected();
      if (session_) {
        ssh_disconnect(session_);
        ssh_free(session_);
        session_ = nullptr;
      }
      if (stop_requested_.load()) break;
      set_status(TunnelStatus::Reconnecting, "connection lost, reconnecting");
    }
    if (wait_interruptible(backoff_ms)) break;
    backoff_ms = std::min(backoff_ms * 2, kMaxBackoffMs);
  }

  if (listener_fd_ >= 0) {
    close(listener_fd_);
    listener_fd_ = -1;
  }
  if (session_) {
    ssh_disconnect(session_);
    ssh_free(session_);
    session_ = nullptr;
  }
  set_status(TunnelStatus::Stopped);
}

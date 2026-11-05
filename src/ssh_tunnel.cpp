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
#include <curl/curl.h>
#include <fcntl.h>
#include <libssh/server.h>
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

// curl write callback: discard the response body (we only care about timing).
std::size_t discard_body(char*, std::size_t size, std::size_t nmemb, void*) {
  return size * nmemb;
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
  ping_cv_.notify_all();
}

void TunnelWorker::join() {
  if (thread_.joinable()) {
    thread_.join();
  }
  if (ping_thread_.joinable()) {
    ping_thread_.join();
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
    stats_.reconnect_attempt = reconnect_attempt_;
    stats_.max_reconnect_attempts = max_reconnect_attempts_;
  }
  if (notify_) notify_();
}

void TunnelWorker::set_ping(double ms) {
  {
    std::lock_guard<std::mutex> lk(stats_mutex_);
    stats_.ping_ms = ms;
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

// Poll the SSH socket (and the stop pipe) for up to timeout_ms.
// Returns 1 when the SSH fd is ready, 0 on timeout, -1 when stop is requested.
int TunnelWorker::wait_for_ssh(int timeout_ms) {
  const int fd = ssh_get_fd(session_);
  pollfd pfds[2] = {
      {stop_pipe_[0], POLLIN, 0},
      {fd, POLLIN | POLLOUT, 0},  // either direction can wake us
  };
  for (;;) {
    const int rc = poll(pfds, 2, timeout_ms);
    if (rc < 0) {
      if (errno == EINTR) continue;
      return 0;  // treat unexpected errors as timeout; the deadline catches it
    }
    if (pfds[0].revents & (POLLIN | POLLHUP | POLLERR)) return -1;
    if (rc > 0) return 1;
    return 0;
  }
}

TunnelWorker::ConnectResult TunnelWorker::connect_and_auth() {
  session_ = ssh_new();
  if (!session_) {
    last_connect_error_ = "failed to allocate SSH session";
    return ConnectResult::Failed;
  }

  unsigned int port = static_cast<unsigned int>(target_.port);
  long timeout_s = settings_.connect_timeout_sec;
  long timeout_us = 0;
  ssh_options_set(session_, SSH_OPTIONS_HOST, target_.host.c_str());
  ssh_options_set(session_, SSH_OPTIONS_PORT, &port);
  ssh_options_set(session_, SSH_OPTIONS_USER, target_.login.c_str());
  ssh_options_set(session_, SSH_OPTIONS_TIMEOUT, &timeout_s);
  ssh_options_set(session_, SSH_OPTIONS_TIMEOUT_USEC, &timeout_us);

  // Nonblocking from the start so stop() can interrupt us and we never block
  // the worker thread (and therefore never block stop()'s join()).
  ssh_set_blocking(session_, 0);

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);

  auto timed_out = [&] {
    return std::chrono::steady_clock::now() >= deadline;
  };

  // --- connect ---
  for (;;) {
    if (stop_requested_.load()) {
      ssh_free(session_);
      session_ = nullptr;
      return ConnectResult::Stopped;
    }
    if (timed_out()) {
      last_connect_error_ = "connect timed out";
      ssh_free(session_);
      session_ = nullptr;
      return ConnectResult::Failed;
    }
    const int rc = ssh_connect(session_);
    if (rc == SSH_OK) break;
    if (rc != SSH_AGAIN) {
      last_connect_error_ =
          "connect failed: " + std::string(ssh_get_error(session_));
      ssh_free(session_);
      session_ = nullptr;
      return ConnectResult::Failed;
    }
    if (wait_for_ssh(250) < 0) {  // small slices to re-check stop/deadline
      ssh_free(session_);
      session_ = nullptr;
      return ConnectResult::Stopped;
    }
  }

  // --- authenticate ---
  for (;;) {
    if (stop_requested_.load()) {
      ssh_disconnect(session_);
      ssh_free(session_);
      session_ = nullptr;
      return ConnectResult::Stopped;
    }
    if (timed_out()) {
      last_connect_error_ = "authentication timed out";
      ssh_disconnect(session_);
      ssh_free(session_);
      session_ = nullptr;
      return ConnectResult::Failed;
    }
    const int rc = ssh_userauth_password(session_, nullptr, target_.password.c_str());
    if (rc == SSH_AUTH_SUCCESS) break;
    if (rc == SSH_AUTH_AGAIN) {
      if (wait_for_ssh(250) < 0) {
        ssh_disconnect(session_);
        ssh_free(session_);
        session_ = nullptr;
        return ConnectResult::Stopped;
      }
      continue;
    }
    last_connect_error_ =
        "authentication failed: " + std::string(ssh_get_error(session_));
    ssh_disconnect(session_);
    ssh_free(session_);
    session_ = nullptr;
    return ConnectResult::Failed;
  }

  last_connect_error_.clear();
  return ConnectResult::Connected;  // session already nonblocking
}

void TunnelWorker::run_connected() {
  // Drive the SSH socket via libssh's event loop instead of polling the raw
  // session fd ourselves. ssh_event reads incoming packets into the channel
  // buffers and flushes the out-buffer, so the session stays serviced even
  // when no forwarder happens to be in a read/write/open state.
  ssh_event event = ssh_event_new();
  if (!event || ssh_event_add_session(event, session_) != SSH_OK) {
    if (event) {
      ssh_event_free(event);
    }
    return;  // treat as a lost connection; run() will reconnect
  }

  std::vector<Forwarder> forwards;

  // ServerAlive keepalive bookkeeping (reset per connection).
  last_incoming_ = std::chrono::steady_clock::now();
  last_keepalive_ = std::chrono::steady_clock::now();

  while (!stop_requested_.load()) {
    // ServerAlive keepalive: when the connection has been idle (no inbound
    // data) for `server_alive_interval`, send a keepalive; disconnect after
    // `server_alive_count_max` consecutive silent intervals.
    if (settings_.server_alive_interval > 0) {
      const auto now = std::chrono::steady_clock::now();

      // Detect inbound data before ssh_event_dopoll drains the socket. Only
      // inbound data sets SSH_READ_PENDING, so our own keepalive flush (a
      // write) never counts as liveness.
      if (ssh_get_status(session_) & SSH_READ_PENDING) {
        last_incoming_ = now;
      }

      const auto interval =
          std::chrono::seconds(settings_.server_alive_interval);
      if (now - last_incoming_ >= interval) {
        // Probe once per interval. libssh's nonblocking global request is a
        // single-slot state machine, so the first call may only consume the
        // previous reply; call twice to guarantee one keepalive is sent.
        if (now - last_keepalive_ >= interval) {
          ssh_send_keepalive(session_);
          ssh_send_keepalive(session_);
          last_keepalive_ = now;
        }
        if (settings_.server_alive_count_max > 0 &&
            (now - last_incoming_) / interval >= settings_.server_alive_count_max) {
          break;  // treat as lost connection; run() will reconnect
        }
      }
    }

    if (ssh_is_connected(session_) == 0) {
      break;
    }

    // 1) Service the session (nonblocking). SSH_AGAIN means "nothing to do"
    //    and is normal; SSH_ERROR means the connection is dead.
    if (ssh_event_dopoll(event, 0) == SSH_ERROR) {
      break;
    }

    // 2) Poll our own fds only: stop pipe, listener, local sockets.
    //    The session fd is deliberately absent — ssh_event owns it now.
    std::vector<pollfd> pfds;
    pfds.reserve(forwards.size() + 2);
    pfds.push_back(pollfd{stop_pipe_[0], POLLIN, 0});
    pfds.push_back(pollfd{listener_fd_, POLLIN, 0});
    for (auto& f : forwards) {
      short ev = POLLIN;
      if (!f.to_local.empty()) ev |= POLLOUT;
      pfds.push_back(pollfd{f.fd, ev, 0});
    }

    // Short timeout so we loop back to ssh_event_dopoll frequently even when
    // only the server has data. Also caps stop() latency at ~100ms.
    const int rc = poll(pfds.data(), static_cast<nfds_t>(pfds.size()), 100);
    if (rc < 0) {
      if (errno == EINTR) continue;
      break;
    }

    if (pfds[0].revents & (POLLIN | POLLHUP | POLLERR)) break;  // stop

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
                // Preserve any payload the client pipelined right after the
                // CONNECT request; queue it for the channel (flushed once
                // open) instead of dropping it.
                if (!f.in.empty()) {
                  f.to_remote.append(f.in);
                  f.in.clear();
                }
                f.target_host = r.host;
                f.target_port = r.port;
                f.ch = ssh_channel_new(session_);
                if (!f.ch) {
                  f.to_local += fail_reply();
                  f.dead = true;
                  break;
                }
                f.state = Forwarder::State::Opening;
                // Initiate the open immediately (step 1 already ran for this
                // forwarder this iteration) so the request goes out without
                // waiting a full poll cycle.
                const int o = ssh_channel_open_forward(
                    f.ch, f.target_host.c_str(), f.target_port, "127.0.0.1",
                    settings_.local_port);
                if (o == SSH_OK) {
                  f.to_local += success_reply();
                  f.state = Forwarder::State::Stream;
                } else if (o == SSH_ERROR) {
                  f.to_local += fail_reply();
                  f.dead = true;
                }
                // SSH_AGAIN: request in flight; step 1 retries next time.
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

    publish_stats(forwards.size());
  }

  ssh_event_remove_session(event, session_);
  ssh_event_free(event);

  for (auto& f : forwards) close_forwarder(f);
  forwards.clear();
}

double TunnelWorker::do_probe() {
  CURL* curl = curl_easy_init();
  if (!curl) {
    return -1.0;
  }

  const std::string url = "http://" + settings_.ping_host + ":" +
                          std::to_string(settings_.ping_port) + "/";
  const std::string proxy =
      "socks5h://127.0.0.1:" + std::to_string(settings_.local_port);

  const long timeout_ms =
      std::clamp(settings_.ping_interval_sec * 1000, 1000, 5000);

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_PROXY, proxy.c_str());
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, timeout_ms);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_body);

  const CURLcode rc = curl_easy_perform(curl);
  double result = -1.0;
  if (rc == CURLE_OK) {
    double seconds = 0.0;
    curl_easy_getinfo(curl, CURLINFO_STARTTRANSFER_TIME, &seconds);
    result = seconds * 1000.0;  // curl reports seconds; ping_ms is milliseconds
  }
  curl_easy_cleanup(curl);
  return result;
}

void TunnelWorker::run_ping() {
  using clock = std::chrono::steady_clock;
  std::unique_lock<std::mutex> lk(ping_mtx_);

  while (!stop_requested_.load()) {
    // Wait until the tunnel is connected.
    ping_cv_.wait(
        lk, [this] { return stop_requested_.load() || connected_.load(); });
    if (stop_requested_.load()) break;

    // Connected: schedule the next probe and wait for it to come due.
    auto due = clock::now() + std::chrono::seconds(settings_.ping_interval_sec);
    while (!stop_requested_.load() && connected_.load()) {
      ping_cv_.wait_until(lk, due, [this] {
        return stop_requested_.load() || !connected_.load();
      });
      if (stop_requested_.load() || !connected_.load()) break;
      if (clock::now() < due) continue;  // spurious wake
      due = clock::now() + std::chrono::seconds(settings_.ping_interval_sec);

      lk.unlock();
      const double ms = do_probe();
      lk.lock();
      if (stop_requested_.load()) break;
      // The tunnel may have dropped while the probe was in flight; don't publish
      // a stale result (the worker resets ping to -1 on disconnect).
      if (connected_.load()) set_ping(ms);
    }
  }
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

  if (settings_.ping_interval_sec > 0) {
    ping_thread_ = std::thread(&TunnelWorker::run_ping, this);
  }

  int backoff_ms = 1000;
  constexpr int kMaxBackoffMs = 30000;
  max_reconnect_attempts_ = settings_.max_reconnect_attempts;

  while (!stop_requested_.load()) {
    set_status(TunnelStatus::Connecting);
    const ConnectResult cr = connect_and_auth();

    if (cr == ConnectResult::Connected) {
      reconnect_attempt_ = 0;
      connected_ = true;
      ping_cv_.notify_all();
      set_status(TunnelStatus::Connected);
      backoff_ms = 1000;
      run_connected();

      connected_ = false;
      ping_cv_.notify_all();
      set_ping(-1.0);

      if (session_) {
        ssh_disconnect(session_);
        ssh_free(session_);
        session_ = nullptr;
      }
      if (stop_requested_.load()) break;
      set_status(TunnelStatus::Reconnecting, "connection lost, reconnecting");
    } else if (cr == ConnectResult::Stopped) {
      break;  // no spurious reconnect counter / status
    } else {
      // A connect attempt failed (initial or reconnect). Count it, and give up
      // once the configured limit is reached.
      ++reconnect_attempt_;

      if (max_reconnect_attempts_ > 0 &&
          reconnect_attempt_ >= max_reconnect_attempts_) {
        set_status(TunnelStatus::Error,
                   "gave up after " + std::to_string(reconnect_attempt_) +
                       " reconnect attempts");
        wait_interruptible(-1);
        break;
      }

      set_status(TunnelStatus::Reconnecting, last_connect_error_);
    }

    if (stop_requested_.load()) break;
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

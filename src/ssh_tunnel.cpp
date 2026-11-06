#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "ssh_tunnel.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <curl/curl.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ssh_runner.hpp"

namespace {

// curl write callback: discard the response body (we only care about timing).
std::size_t discard_body(char*, std::size_t size, std::size_t nmemb, void*) {
  return size * nmemb;
}

// Read the cumulative byte counters for `pid` from /proc/<pid>/io. These count
// every byte the ssh process read/wrote across both its local (SOCKS) and
// remote (encrypted) sockets, so they include SSH framing and keepalive
// overhead and are only an approximation of tunneled traffic.
bool read_proc_io(pid_t pid, std::uint64_t* rchar, std::uint64_t* wchar) {
  *rchar = 0;
  *wchar = 0;
  if (pid <= 0) {
    return false;
  }
  std::ifstream f("/proc/" + std::to_string(pid) + "/io");
  if (!f) {
    return false;
  }
  std::string line;
  while (std::getline(f, line)) {
    const std::size_t colon = line.find(':');
    if (colon == std::string::npos) {
      continue;
    }
    const std::string key = line.substr(0, colon);
    const char* val = line.c_str() + colon + 1;
    if (key == "rchar") {
      *rchar = std::strtoull(val, nullptr, 10);
    } else if (key == "wchar") {
      *wchar = std::strtoull(val, nullptr, 10);
    }
  }
  return true;
}

// Count ESTABLISHED TCP connections to the local loopback port (i.e. open SOCKS
// connections through the tunnel). The ssh -D listener itself is LISTEN (0x0A),
// which this excludes.
std::size_t count_established(int port) {
  std::ifstream f("/proc/net/tcp");
  if (!f) {
    return 0;
  }
  std::string line;
  std::getline(f, line);  // header

  char port_hex[16];
  std::snprintf(port_hex, sizeof port_hex, "%04X",
                static_cast<unsigned>(port) & 0xFFFFu);
  const std::string want = port_hex;

  std::size_t n = 0;
  while (std::getline(f, line)) {
    std::istringstream ss(line);
    std::string sl, local, rem, st;
    ss >> sl >> local >> rem >> st;
    if (st != "01") {
      continue;
    }
    const std::size_t colon = local.rfind(':');
    if (colon != std::string::npos && local.substr(colon + 1) == want) {
      ++n;
    }
  }
  return n;
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

void TunnelWorker::publish_stats() {
  const auto now = std::chrono::steady_clock::now();
  if (now - last_notify_ < std::chrono::milliseconds(500)) {
    return;
  }
  last_notify_ = now;

  std::uint64_t rchar = 0, wchar = 0;
  read_proc_io(child_pid_, &rchar, &wchar);
  const std::size_t conns = count_established(settings_.local_port);

  {
    std::lock_guard<std::mutex> lk(stats_mutex_);
    stats_.rx_bytes = rchar;
    stats_.tx_bytes = wchar;
    stats_.active_connections = static_cast<int>(conns);
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

bool TunnelWorker::port_listening(int port) const {
  const int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    return false;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(static_cast<std::uint16_t>(port));
  const int rc = connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
  const bool ok =
      (rc == 0 || (rc < 0 && (errno == EINPROGRESS || errno == EISCONN)));
  close(fd);
  return ok;
}

void TunnelWorker::run() {
  set_status(TunnelStatus::Connecting);

  if (settings_.ping_interval_sec > 0) {
    ping_thread_ = std::thread(&TunnelWorker::run_ping, this);
  }

  int backoff_ms = 1000;
  constexpr int kMaxBackoffMs = 30000;
  max_reconnect_attempts_ = settings_.max_reconnect_attempts;

  while (!stop_requested_.load()) {
    set_status(TunnelStatus::Connecting);

    std::vector<std::string> args = {
        "ssh",
        "-N",
        "-D",
        "127.0.0.1:" + std::to_string(settings_.local_port),
        "-p",
        std::to_string(target_.port),
        "-o",
        "StrictHostKeyChecking=no",
        "-o",
        "UserKnownHostsFile=/dev/null",
        "-o",
        "ExitOnForwardFailure=yes",
        "-o",
        "ConnectTimeout=" + std::to_string(settings_.connect_timeout_sec),
        "-o",
        "NumberOfPasswordPrompts=1",
    };
    if (settings_.server_alive_interval > 0) {
      args.insert(
          args.end(),
          {"-o",
           "ServerAliveInterval=" + std::to_string(settings_.server_alive_interval),
           "-o",
           "ServerAliveCountMax=" +
               std::to_string(settings_.server_alive_count_max)});
    }
    args.push_back(target_.login + "@" + target_.host);

    child_stderr_.clear();
    child_pid_ = ssh_runner::spawn(args, target_.password, &child_stderr_fd_);
    const bool spawn_failed = (child_pid_ < 0);
    if (spawn_failed) {
      last_connect_error_ =
          "failed to start ssh: " + std::string(std::strerror(errno));
    }

    bool connected_this_run = false;

    if (!spawn_failed) {
      // Wait until the child exits or the local SOCKS port comes up (ssh binds
      // it only after authentication succeeds, so this doubles as our
      // "connected" signal).
      while (!stop_requested_.load()) {
        ssh_runner::read_stderr(child_stderr_fd_, child_stderr_);

        int status = 0;
        const pid_t r = waitpid(child_pid_, &status, WNOHANG);
        if (r == child_pid_) {
          child_pid_ = -1;
          break;  // child exited
        }
        if (r < 0 && errno != EINTR) {
          child_pid_ = -1;
          break;
        }

        if (!connected_this_run && port_listening(settings_.local_port)) {
          connected_this_run = true;
          connected_ = true;
          reconnect_attempt_ = 0;
          backoff_ms = 1000;
          ping_cv_.notify_all();
          set_status(TunnelStatus::Connected);
        }

        publish_stats();

        if (wait_interruptible(200)) break;  // stop requested
      }
    }

    // If stop was requested while the child was still running, kill it first so
    // it doesn't get SIGPIPE writing to a closed stderr pipe.
    if (stop_requested_.load() && child_pid_ > 0) {
      ssh_runner::terminate(child_pid_);
      child_pid_ = -1;
    }

    // Drain any remaining stderr and close the pipe.
    if (child_stderr_fd_ >= 0) {
      ssh_runner::read_stderr(child_stderr_fd_, child_stderr_);
      close(child_stderr_fd_);
      child_stderr_fd_ = -1;
    }

    if (stop_requested_.load()) {
      break;
    }

    // The child exited (or spawn failed). Reset connection state.
    connected_ = false;
    ping_cv_.notify_all();
    set_ping(-1.0);

    if (spawn_failed || !connected_this_run) {
      ++reconnect_attempt_;
      if (!spawn_failed) {
        last_connect_error_ = ssh_runner::error_from_stderr(child_stderr_);
        if (last_connect_error_.empty()) {
          last_connect_error_ = "ssh exited before connecting";
        }
      }
      if (max_reconnect_attempts_ > 0 &&
          reconnect_attempt_ >= max_reconnect_attempts_) {
        set_status(TunnelStatus::Error,
                   "gave up after " + std::to_string(reconnect_attempt_) +
                       " reconnect attempts");
        wait_interruptible(-1);
        break;
      }
      set_status(TunnelStatus::Reconnecting, last_connect_error_);
    } else {
      // Was connected, then the child exited: reconnect freely.
      set_status(TunnelStatus::Reconnecting, "connection lost, reconnecting");
    }

    if (stop_requested_.load()) break;
    if (wait_interruptible(backoff_ms)) break;
    backoff_ms = std::min(backoff_ms * 2, kMaxBackoffMs);
  }

  if (child_pid_ > 0) {
    ssh_runner::terminate(child_pid_);
    child_pid_ = -1;
  }
  if (child_stderr_fd_ >= 0) {
    close(child_stderr_fd_);
    child_stderr_fd_ = -1;
  }
  set_status(TunnelStatus::Stopped);
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

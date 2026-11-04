#include "server_checker.hpp"

#include <cerrno>
#include <chrono>
#include <string>

#include <libssh/libssh.h>
#include <poll.h>

namespace {

constexpr long kConnectTimeoutSec = 10;
constexpr int kPingTimeoutMs = 5000;

// Connects and authenticates over SSH, then measures the response time via a
// direct-tcpip round-trip to the server's ping target. `ok` reflects the
// connection + auth result; `ping_ms` is set only if the ping probe completes
// in time.
ServerCheckResult check_server(const std::string& host, int port,
                               const std::string& login,
                               const std::string& password,
                               const std::string& ping_host, int ping_port) {
  ServerCheckResult r;
  r.checked = true;

  ssh_session s = ssh_new();
  if (!s) {
    r.error = "failed to allocate SSH session";
    return r;
  }

  unsigned int ssh_port = static_cast<unsigned int>(port);
  long timeout_s = kConnectTimeoutSec;
  long timeout_us = 0;
  ssh_options_set(s, SSH_OPTIONS_HOST, host.c_str());
  ssh_options_set(s, SSH_OPTIONS_PORT, &ssh_port);
  ssh_options_set(s, SSH_OPTIONS_USER, login.c_str());
  ssh_options_set(s, SSH_OPTIONS_TIMEOUT, &timeout_s);
  ssh_options_set(s, SSH_OPTIONS_TIMEOUT_USEC, &timeout_us);

  if (ssh_connect(s) != SSH_OK) {
    r.error = "connect failed: " + std::string(ssh_get_error(s));
    ssh_free(s);
    return r;
  }

  if (ssh_userauth_password(s, nullptr, password.c_str()) != SSH_AUTH_SUCCESS) {
    r.error = "authentication failed: " + std::string(ssh_get_error(s));
    ssh_disconnect(s);
    ssh_free(s);
    return r;
  }

  r.ok = true;

  // Measure response time: open a direct-tcpip channel to the server's ping
  // target (originator port 0 = "any/random"), timing the nonblocking open.
  ssh_set_blocking(s, 0);
  ssh_channel ch = ssh_channel_new(s);
  if (ch) {
    const int fd = ssh_get_fd(s);
    const auto start = std::chrono::steady_clock::now();
    int o = SSH_AGAIN;
    while (o == SSH_AGAIN) {
      o = ssh_channel_open_forward(ch, ping_host.c_str(), ping_port,
                                   "127.0.0.1", 0);
      if (o != SSH_AGAIN) {
        break;
      }
      const auto elapsed =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - start)
              .count();
      if (elapsed >= kPingTimeoutMs) {
        break;
      }
      // The channel-open request write completes immediately (small packet,
      // empty send buffer), so only wait for the confirmation: POLLIN.
      pollfd pfd{fd, POLLIN, 0};
      const int pr = poll(&pfd, 1, 100);
      if (pr < 0) {
        if (errno != EINTR) {
          break;
        }
      } else if (pr > 0 && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
        break;
      }
    }
    if (o == SSH_OK) {
      r.ping_ms = std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - start)
                      .count();
    }
    ssh_channel_free(ch);
  }

  ssh_disconnect(s);
  ssh_free(s);
  return r;
}

}  // namespace

ServerChecker::ServerChecker(Model& model, SecretStore& secrets)
    : model_(model), secrets_(secrets) {
  dispatcher_ = std::make_unique<Glib::Dispatcher>();
  dispatcher_->connect([this] {
    std::vector<std::string> finished;
    {
      std::lock_guard<std::mutex> lk(mutex_);
      finished.swap(pending_);
    }
    for (const auto& id : finished) {
      signal_changed.emit(id);
    }
    if (!running_.load()) {
      signal_finished.emit();
    }
  });
}

ServerChecker::~ServerChecker() {
  if (thread_.joinable()) {
    thread_.join();
  }
}

void ServerChecker::check_all() {
  if (running_.load()) {
    return;
  }
  std::vector<Snapshot> servers;
  servers.reserve(model_.servers.size());
  for (const auto& s : model_.servers) {
    servers.push_back({s.id, s.host, s.port, s.login, s.ping_host, s.ping_port});
  }
  start(std::move(servers));
}

void ServerChecker::check_one(const std::string& id) {
  if (running_.load()) {
    return;
  }
  const Server* s = model_.find_server(id);
  if (!s) {
    return;
  }
  start({{s->id, s->host, s->port, s->login, s->ping_host, s->ping_port}});
}

void ServerChecker::start(std::vector<Snapshot> servers) {
  if (thread_.joinable()) {
    thread_.join();
  }
  running_.store(true);
  // Reset the affected results so rows show "Checking" while the check runs.
  for (const auto& s : servers) {
    {
      std::lock_guard<std::mutex> lk(mutex_);
      ServerCheckResult r;
      r.checking = true;
      results_[s.id] = r;
    }
    signal_changed.emit(s.id);
  }
  thread_ = std::thread([this, servers = std::move(servers)] { run(servers); });
}

void ServerChecker::run(std::vector<Snapshot> servers) {
  for (const auto& s : servers) {
    ServerCheckResult r;
    std::string perr;
    auto pw = secrets_.get_password(s.id, &perr);
    if (!pw) {
      r.checked = true;
      r.error = "no password: " + perr;
    } else {
      r = check_server(s.host, s.port, s.login, *pw, s.ping_host, s.ping_port);
    }
    {
      std::lock_guard<std::mutex> lk(mutex_);
      results_[s.id] = r;
      pending_.push_back(s.id);
    }
    dispatcher_->emit();
  }
  running_.store(false);
  dispatcher_->emit();
}

ServerCheckResult ServerChecker::result(const std::string& server_id) const {
  std::lock_guard<std::mutex> lk(mutex_);
  auto it = results_.find(server_id);
  if (it == results_.end()) {
    return ServerCheckResult{};
  }
  return it->second;
}

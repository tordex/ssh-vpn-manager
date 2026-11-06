#include "server_checker.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include "ssh_runner.hpp"

namespace {

constexpr long kConnectTimeoutSec = 10;
constexpr int kCheckTimeoutMs = 15000;

// Connects and authenticates over SSH via a one-shot `ssh ... true`, measuring
// the round-trip as the login latency. `ok` reflects the connect + auth result
// (exit status); `ping_ms` is the elapsed time of a successful login. The
// per-server ping target is no longer probed (ssh can't do a one-shot
// direct-tcpip), so ping_host/ping_port are unused here.
ServerCheckResult check_server(const std::string& host, int port,
                               const std::string& login,
                               const std::string& password,
                               const std::string& /*ping_host*/,
                               int /*ping_port*/) {
  ServerCheckResult r;
  r.checked = true;

  std::vector<std::string> args = {
      "ssh",
      "-o",
      "StrictHostKeyChecking=no",
      "-o",
      "UserKnownHostsFile=/dev/null",
      "-o",
      "ConnectTimeout=" + std::to_string(kConnectTimeoutSec),
      "-p",
      std::to_string(port),
      login + "@" + host,
      "true",
  };

  int stderr_fd = -1;
  const auto start = std::chrono::steady_clock::now();
  const pid_t pid = ssh_runner::spawn(args, password, &stderr_fd);
  if (pid < 0) {
    r.error = "failed to start ssh: " + std::string(std::strerror(errno));
    return r;
  }

  std::string err;
  int status = 0;
  for (;;) {
    ssh_runner::read_stderr(stderr_fd, err);
    const pid_t w = waitpid(pid, &status, WNOHANG);
    if (w == pid) {
      break;
    }
    if (w < 0 && errno != EINTR) {
      r.error = "waitpid failed: " + std::string(std::strerror(errno));
      break;
    }
    if (std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start)
            .count() >= kCheckTimeoutMs) {
      ssh_runner::terminate(pid);
      r.error = "check timed out";
      break;
    }
    usleep(50 * 1000);
  }
  ssh_runner::read_stderr(stderr_fd, err);
  close(stderr_fd);

  if (!r.error.empty()) {
    return r;
  }

  if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
    r.ok = true;
    r.ping_ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - start)
                    .count();
  } else {
    r.error = ssh_runner::error_from_stderr(err);
    if (r.error.empty()) {
      r.error = "ssh exited with status " + std::to_string(WEXITSTATUS(status));
    }
  }
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

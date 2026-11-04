#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <glibmm/dispatcher.h>
#include <sigc++/sigc++.h>

#include "model.hpp"
#include "secret_store.hpp"

struct ServerCheckResult {
  bool checked = false;
  bool checking = false;   // in progress
  bool ok = false;         // connect + auth succeeded
  std::string error;       // set when !ok
  double ping_ms = -1.0;   // < 0 = ping unavailable
};

// Runs one-off SSH reachability/latency checks for servers in a background
// thread and reports results on the main thread. All public methods are
// main-thread only (driven by the UI), mirroring TunnelController.
class ServerChecker {
public:
  ServerChecker(Model& model, SecretStore& secrets);
  ~ServerChecker();

  void check_all();                       // check every server
  void check_one(const std::string& id);  // check a single server
  bool busy() const { return running_.load(); }

  ServerCheckResult result(const std::string& server_id) const;

  // Emitted on the main thread, once per finished server (with its id). Also
  // emitted with a not-yet-checked result when a check starts, so rows can show
  // "—" immediately.
  sigc::signal<void(std::string)> signal_changed;
  // Emitted on the main thread when a check round finishes.
  sigc::signal<void()> signal_finished;

private:
  struct Snapshot {
    std::string id;
    std::string host;
    int port = 22;
    std::string login;
    std::string ping_host = "1.1.1.1";
    int ping_port = 80;
  };

  void start(std::vector<Snapshot> servers);
  void run(std::vector<Snapshot> servers);

  Model& model_;
  SecretStore& secrets_;

  std::thread thread_;
  std::atomic<bool> running_{false};
  std::unique_ptr<Glib::Dispatcher> dispatcher_;

  mutable std::mutex mutex_;
  std::map<std::string, ServerCheckResult> results_;
  std::vector<std::string> pending_;  // ids finished since the last drain
};

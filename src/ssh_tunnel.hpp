#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include <sys/types.h>

enum class TunnelStatus {
  Stopped,
  Connecting,
  Connected,
  Reconnecting,
  Error,
};

struct TunnelStats {
  TunnelStatus status = TunnelStatus::Stopped;
  std::string error;
  double ping_ms = -1.0;  // < 0 means "unknown / no successful ping yet"
  std::uint64_t tx_bytes = 0;  // bytes forwarded into the tunnel (approximate)
  std::uint64_t rx_bytes = 0;  // bytes forwarded out of the tunnel (approximate)
  int active_connections = 0;  // currently open SOCKS connections
  int reconnect_attempt = 0;   // consecutive failed (re)connect attempts so far
  int max_reconnect_attempts = 0;  // configured limit (0 = unlimited)
};

struct TunnelTarget {
  std::string host;
  int port = 22;
  std::string login;
  std::string password;  // decrypted at start; held in memory for reconnects
};

struct TunnelSettings {
  int local_port = 1080;
  std::string ping_host;
  int ping_port = 80;
  int ping_interval_sec = 10;
  int connect_timeout_sec = 10;
  int max_reconnect_attempts = 5;  // 0 = keep retrying forever
  int server_alive_interval = 0;   // seconds; 0 = disabled
  int server_alive_count_max = 3;  // silent intervals before disconnect
};

// One background thread per running tunnel. Supervises an `ssh -N -D` child
// process (which owns the SOCKS5 listener and the SSH forwarding) and restarts
// it when it exits. Stats are published through the `notify` callback (invoked
// from this thread) whenever the UI should re-read them.
class TunnelWorker {
public:
  TunnelWorker(TunnelTarget target, TunnelSettings settings,
               std::function<void()> notify);
  ~TunnelWorker();

  TunnelWorker(const TunnelWorker&) = delete;
  TunnelWorker& operator=(const TunnelWorker&) = delete;

  void start();
  void stop();  // async: wakes the worker, does not join
  void join();

  TunnelStats stats() const;

private:
  void run();
  void run_ping();
  double do_probe();
  void set_ping(double ms);
  void set_status(TunnelStatus s, const std::string& error = "");
  void publish_stats();
  bool wait_interruptible(int timeout_ms);  // true if stop was requested
  bool port_listening(int port) const;

  TunnelTarget target_;
  TunnelSettings settings_;
  std::function<void()> notify_;

  std::thread thread_;
  std::atomic<bool> stop_requested_{false};

  // Ping probe thread. It makes a real HTTP request through the tunnel's own
  // SOCKS5 listener (see do_probe), which is now served by the ssh child.
  std::thread ping_thread_;
  std::mutex ping_mtx_;
  std::condition_variable ping_cv_;
  std::atomic<bool> connected_{false};

  int stop_pipe_[2] = {-1, -1};

  // Current ssh child process — worker-thread only.
  pid_t child_pid_ = -1;
  int child_stderr_fd_ = -1;
  std::string child_stderr_;  // accumulated stderr for error reporting

  // Written only from the worker thread; guarded for reads from the UI thread.
  mutable std::mutex stats_mutex_;
  TunnelStats stats_;
  int reconnect_attempt_ = 0;
  int max_reconnect_attempts_ = 0;
  std::string last_connect_error_;  // worker-thread only (no mutex)
  std::chrono::steady_clock::time_point last_notify_{};
};

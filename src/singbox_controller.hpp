#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <glibmm/dispatcher.h>
#include <sigc++/sigc++.h>

#include "log.hpp"
#include "model.hpp"

// Owns a single sing-box instance that routes all system traffic through the
// SOCKS5 listener of the selected tunnel (a TUN interface). All public methods
// are main-thread only (driven by the UI), mirroring TunnelController. Only one
// route is active at a time: starting a new one stops the previous one.
class SingboxController
{
  public:
    SingboxController(Model& model, Log& log);
    ~SingboxController();

    // Generates the config, validates it with `sing-box check`, then runs
    // `sing-box run`. Returns an empty string on success or an error message.
    std::string start(const std::string& tunnel_id);
    void        stop();

    bool        is_running() const;
    std::string active_tunnel_id() const;

    sigc::signal<void()> signal_changed;

  private:
    std::string        generate_config(const Tunnel& t, std::string* err) const;
    static std::string config_path();
    void               watch_run();

    Model& model_;
    Log&   log_;

    std::unique_ptr<Glib::Dispatcher> dispatcher_;

    std::atomic<pid_t> pid_{-1};
    int                stderr_fd_ = -1;
    std::atomic<bool>  stop_requested_{false};
    std::thread        watch_thread_;

    std::string active_tunnel_id_;

    // Child-exit reporting from the watcher thread to the main thread.
    std::mutex  mutex_;
    bool        exited_      = false;
    bool        unexpected_  = false;
    int         exit_status_ = 0;
    std::string exit_stderr_;
};

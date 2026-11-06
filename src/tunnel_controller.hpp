#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <glibmm/dispatcher.h>
#include <sigc++/sigc++.h>

#include "log.hpp"
#include "model.hpp"
#include "secret_store.hpp"
#include "ssh_tunnel.hpp"

// Owns the worker thread for each running tunnel. All methods must be called
// from the GTK main thread (they are driven by the UI). `signal_changed` fires
// on the main thread whenever a tunnel's status/stats change.
class TunnelController
{
  public:
    TunnelController(Model& model, SecretStore& secrets, Log& log);

    // Starts the tunnel. Returns an empty string on success or an error message.
    std::string start(const std::string& tunnel_id);
    void        stop(const std::string& tunnel_id);
    void        stop_all();

    bool        is_running(const std::string& tunnel_id) const;
    TunnelStats stats(const std::string& tunnel_id) const;

    sigc::signal<void(std::string)> signal_changed;

  private:
    struct Entry
    {
        std::unique_ptr<TunnelWorker>     worker;
        std::unique_ptr<Glib::Dispatcher> dispatcher;
        std::string                       description; // human-readable, for log messages
        TunnelStatus                      last_status = TunnelStatus::Stopped;
        std::string                       last_error;
    };

    void log_status(const Entry& entry, const TunnelStats& st);

    Model&                       model_;
    SecretStore&                 secrets_;
    Log&                         log_;
    std::map<std::string, Entry> entries_;
};

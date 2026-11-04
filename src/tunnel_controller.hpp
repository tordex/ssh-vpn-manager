#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <glibmm/dispatcher.h>
#include <sigc++/sigc++.h>

#include "model.hpp"
#include "secret_store.hpp"
#include "ssh_tunnel.hpp"

// Owns the worker thread for each running tunnel. All methods must be called
// from the GTK main thread (they are driven by the UI). `signal_changed` fires
// on the main thread whenever a tunnel's status/stats change.
class TunnelController {
public:
  TunnelController(Model& model, SecretStore& secrets);

  // Starts the tunnel. Returns an empty string on success or an error message.
  std::string start(const std::string& tunnel_id);
  void stop(const std::string& tunnel_id);
  void stop_all();

  bool is_running(const std::string& tunnel_id) const;
  TunnelStats stats(const std::string& tunnel_id) const;

  sigc::signal<void(std::string)> signal_changed;

private:
  struct Entry {
    std::unique_ptr<TunnelWorker> worker;
    std::unique_ptr<Glib::Dispatcher> dispatcher;
  };

  Model& model_;
  SecretStore& secrets_;
  std::map<std::string, Entry> entries_;
};

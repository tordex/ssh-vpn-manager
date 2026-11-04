#include "tunnel_controller.hpp"

TunnelController::TunnelController(Model& model, SecretStore& secrets)
    : model_(model), secrets_(secrets) {}

std::string TunnelController::start(const std::string& tunnel_id) {
  stop(tunnel_id);

  const Tunnel* t = model_.find_tunnel(tunnel_id);
  if (!t) {
    return "tunnel not found";
  }
  const Server* s = model_.find_server(t->server_id);
  if (!s) {
    return "the server referenced by this tunnel no longer exists";
  }

  std::string perr;
  auto pw = secrets_.get_password(s->id, &perr);
  if (!pw) {
    return "cannot read password from keyring: " + perr;
  }

  TunnelTarget target;
  target.host = s->host;
  target.port = s->port;
  target.login = s->login;
  target.password = std::move(*pw);

  TunnelSettings set;
  set.local_port = t->local_port;
  set.ping_host = t->ping_host;
  set.ping_port = t->ping_port;
  set.ping_interval_sec = t->ping_interval_sec;
  set.connect_timeout_sec = t->connect_timeout_sec;

  auto entry = std::make_unique<Entry>();
  entry->dispatcher = std::make_unique<Glib::Dispatcher>();
  entry->worker = std::make_unique<TunnelWorker>(
      std::move(target), std::move(set),
      [disp = entry->dispatcher.get()] { disp->emit(); });

  entry->dispatcher->connect([this, tunnel_id] { signal_changed.emit(tunnel_id); });
  entry->worker->start();

  entries_[tunnel_id] = std::move(*entry);
  signal_changed.emit(tunnel_id);
  return "";
}

void TunnelController::stop(const std::string& tunnel_id) {
  auto it = entries_.find(tunnel_id);
  if (it == entries_.end()) {
    return;
  }
  it->second.worker->stop();
  it->second.worker->join();
  entries_.erase(it);
  signal_changed.emit(tunnel_id);
}

void TunnelController::stop_all() {
  std::vector<std::string> ids;
  ids.reserve(entries_.size());
  for (const auto& [id, entry] : entries_) {
    (void)entry;
    ids.push_back(id);
  }
  for (const auto& id : ids) {
    stop(id);
  }
}

bool TunnelController::is_running(const std::string& tunnel_id) const {
  auto it = entries_.find(tunnel_id);
  if (it == entries_.end()) {
    return false;
  }
  const TunnelStats st = it->second.worker->stats();
  return st.status != TunnelStatus::Error && st.status != TunnelStatus::Stopped;
}

TunnelStats TunnelController::stats(const std::string& tunnel_id) const {
  auto it = entries_.find(tunnel_id);
  if (it == entries_.end()) {
    return TunnelStats{};
  }
  return it->second.worker->stats();
}

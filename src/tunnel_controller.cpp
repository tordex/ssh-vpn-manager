#include "tunnel_controller.hpp"

namespace {

const char* status_word(TunnelStatus s) {
  switch (s) {
    case TunnelStatus::Stopped: return "stopped";
    case TunnelStatus::Connecting: return "connecting";
    case TunnelStatus::Connected: return "connected";
    case TunnelStatus::Reconnecting: return "reconnecting";
    case TunnelStatus::Error: return "error";
  }
  return "unknown";
}

}  // namespace

TunnelController::TunnelController(Model& model, SecretStore& secrets, Log& log)
    : model_(model), secrets_(secrets), log_(log) {}

std::string TunnelController::start(const std::string& tunnel_id) {
  stop(tunnel_id);

  const Tunnel* t = model_.find_tunnel(tunnel_id);
  if (!t) {
    return "tunnel not found";
  }

  const std::string port_text =
      "Tunnel 127.0.0.1:" + std::to_string(t->local_port);

  const Server* s = model_.find_server(t->server_id);
  if (!s) {
    log_.append(port_text + ": failed to start — server no longer exists");
    return "the server referenced by this tunnel no longer exists";
  }

  const std::string desc =
      port_text + (s->name.empty() ? "" : " (" + s->name + ")");

  std::string perr;
  auto pw = secrets_.get_password(s->id, &perr);
  if (!pw) {
    log_.append(desc + ": failed to start — " + perr);
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
  set.max_reconnect_attempts = t->max_reconnect_attempts;

  auto entry = std::make_unique<Entry>();
  entry->dispatcher = std::make_unique<Glib::Dispatcher>();
  entry->worker = std::make_unique<TunnelWorker>(
      std::move(target), std::move(set),
      [disp = entry->dispatcher.get()] { disp->emit(); });
  entry->description = desc;

  entry->dispatcher->connect([this, tunnel_id] {
    signal_changed.emit(tunnel_id);
    auto it = entries_.find(tunnel_id);
    if (it == entries_.end()) {
      return;
    }
    const TunnelStats st = it->second.worker->stats();
    if (st.status != it->second.last_status ||
        st.error != it->second.last_error) {
      log_status(it->second, st);
      it->second.last_status = st.status;
      it->second.last_error = st.error;
    }
  });
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
  log_.append(it->second.description + ": stopped");
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

void TunnelController::log_status(const Entry& entry, const TunnelStats& st) {
  std::string msg = entry.description + ": " + status_word(st.status);
  if (st.status == TunnelStatus::Reconnecting && st.max_reconnect_attempts > 0 &&
      st.reconnect_attempt > 0) {
    msg += " (attempt " + std::to_string(st.reconnect_attempt) + "/" +
           std::to_string(st.max_reconnect_attempts) + ")";
  }
  if (!st.error.empty()) {
    msg += " — " + st.error;
  }
  log_.append(msg);
}

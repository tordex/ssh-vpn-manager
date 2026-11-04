#include "window.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "app.hpp"

namespace {

std::string format_bytes(std::uint64_t b) {
  static const char* units[] = {"B", "KB", "MB", "GB", "TB"};
  double v = static_cast<double>(b);
  int i = 0;
  while (v >= 1024.0 && i < 4) {
    v /= 1024.0;
    ++i;
  }
  char buf[32];
  std::snprintf(buf, sizeof buf, i == 0 ? "%.0f %s" : "%.1f %s", v, units[i]);
  return buf;
}

std::string ping_text(double ms) {
  if (ms < 0.0) {
    return "—";
  }
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.0f ms", ms);
  return buf;
}

const char* status_name(TunnelStatus s) {
  switch (s) {
    case TunnelStatus::Stopped: return "Stopped";
    case TunnelStatus::Connecting: return "Connecting";
    case TunnelStatus::Connected: return "Connected";
    case TunnelStatus::Reconnecting: return "Reconnecting";
    case TunnelStatus::Error: return "Error";
  }
  return "Unknown";
}

const char* status_color(TunnelStatus s) {
  switch (s) {
    case TunnelStatus::Stopped: return "#9a9996";
    case TunnelStatus::Connecting: return "#3584e4";
    case TunnelStatus::Connected: return "#26a269";
    case TunnelStatus::Reconnecting: return "#e66100";
    case TunnelStatus::Error: return "#c01c28";
  }
  return "#9a9996";
}

std::string status_markup(const TunnelStats& st) {
  std::string name = status_name(st.status);
  if (st.status == TunnelStatus::Reconnecting && st.max_reconnect_attempts > 0 &&
      st.reconnect_attempt > 0) {
    name += " (" + std::to_string(st.reconnect_attempt) + "/" +
            std::to_string(st.max_reconnect_attempts) + ")";
  }
  std::string m = "<span color='";
  m += status_color(st.status);
  m += "'>";
  m += name;
  m += "</span>";
  return m;
}

Gtk::Label* make_cell(const std::string& text, int width, float xalign, bool ellipsize) {
  auto* l = Gtk::make_managed<Gtk::Label>(text);
  l->set_size_request(width, -1);
  l->set_xalign(xalign);
  if (ellipsize) {
    l->set_ellipsize(Pango::EllipsizeMode::END);
  }
  return l;
}

void grid_row(Gtk::Grid& grid, int row, const char* label, Gtk::Widget& w) {
  auto* l = Gtk::make_managed<Gtk::Label>(label);
  l->set_halign(Gtk::Align::END);
  l->set_margin_end(8);
  grid.attach(*l, 0, row, 1, 1);
  w.set_hexpand(true);
  grid.attach(w, 1, row, 1, 1);
}

Gtk::Label* make_error_label() {
  auto* l = Gtk::make_managed<Gtk::Label>();
  l->set_markup("<span color='#c01c28'></span>");
  l->set_visible(false);
  l->set_wrap(true);
  l->set_xalign(0.0f);
  return l;
}

void set_error(Gtk::Label* l, const std::string& msg) {
  if (msg.empty()) {
    l->set_visible(false);
    return;
  }
  l->set_markup("<span color='#c01c28'>" + msg + "</span>");
  l->set_visible(true);
}

// --- Server add/edit dialog --------------------------------------------------

class ServerDialog : public Gtk::Window {
public:
  ServerDialog(Gtk::Window& parent, const Server* existing)
      : id_(existing ? existing->id : "") {
    set_modal(true);
    set_transient_for(parent);
    set_title(existing ? "Edit Server" : "Add Server");
    set_default_size(420, -1);

    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
    box->set_margin(16);
    set_child(*box);

    auto* grid = Gtk::make_managed<Gtk::Grid>();
    grid->set_row_spacing(8);
    grid->set_column_spacing(8);

    name_ = Gtk::make_managed<Gtk::Entry>();
    host_ = Gtk::make_managed<Gtk::Entry>();
    login_ = Gtk::make_managed<Gtk::Entry>();
    password_ = Gtk::make_managed<Gtk::Entry>();
    password_->set_visibility(false);
    password_->set_placeholder_text(existing ? "(unchanged)" : "required");
    password_->set_input_purpose(Gtk::InputPurpose::PASSWORD);

    port_ = Gtk::make_managed<Gtk::SpinButton>();
    port_->set_range(1, 65535);
    port_->set_increments(1, 10);
    port_->set_value(existing ? existing->port : 22);

    auto* show_pw = Gtk::make_managed<Gtk::CheckButton>("Show password");
    show_pw->signal_toggled().connect([this, show_pw] {
      password_->set_visibility(show_pw->get_active());
    });

    if (existing) {
      name_->set_text(existing->name);
      host_->set_text(existing->host);
      login_->set_text(existing->login);
    }

    grid_row(*grid, 0, "Name", *name_);
    grid_row(*grid, 1, "Host / IP", *host_);
    grid_row(*grid, 2, "Port", *port_);
    grid_row(*grid, 3, "Login", *login_);
    grid_row(*grid, 4, "Password", *password_);
    grid->attach(*show_pw, 1, 5, 1, 1);
    box->append(*grid);

    error_ = make_error_label();
    box->append(*error_);

    auto* btns = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    btns->set_halign(Gtk::Align::END);
    auto* cancel = Gtk::make_managed<Gtk::Button>("Cancel");
    auto* save = Gtk::make_managed<Gtk::Button>("Save");
    save->add_css_class("suggested-action");
    cancel->signal_clicked().connect([this] { close(); });
    save->signal_clicked().connect([this] { on_save(); });
    btns->append(*cancel);
    btns->append(*save);
    box->append(*btns);
  }

  sigc::signal<void(Server, std::string, bool)> signal_saved;

private:
  void on_save() {
    const std::string name = name_->get_text();
    const std::string host = host_->get_text();
    const std::string login = login_->get_text();
    const std::string pw = password_->get_text();

    if (name.empty()) return set_error(error_, "Name is required.");
    if (host.empty()) return set_error(error_, "Host / IP is required.");
    if (login.empty()) return set_error(error_, "Login is required.");
    if (id_.empty() && pw.empty()) return set_error(error_, "Password is required.");

    Server s;
    s.id = id_.empty() ? Model::new_id() : id_;
    s.name = name;
    s.host = host;
    s.port = port_->get_value_as_int();
    s.login = login;

    signal_saved.emit(std::move(s), pw, !pw.empty());
    close();
  }

  Gtk::Entry* name_;
  Gtk::Entry* host_;
  Gtk::Entry* login_;
  Gtk::Entry* password_;
  Gtk::SpinButton* port_;
  Gtk::Label* error_;
  std::string id_;
};

// --- Tunnel add/edit dialog --------------------------------------------------

class TunnelDialog : public Gtk::Window {
public:
  TunnelDialog(Gtk::Window& parent, const Model& model, const Tunnel* existing)
      : id_(existing ? existing->id : "") {
    set_modal(true);
    set_transient_for(parent);
    set_title(existing ? "Edit Tunnel" : "Add Tunnel");
    set_default_size(420, -1);

    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
    box->set_margin(16);
    set_child(*box);

    auto* grid = Gtk::make_managed<Gtk::Grid>();
    grid->set_row_spacing(8);
    grid->set_column_spacing(8);

    local_port_ = Gtk::make_managed<Gtk::SpinButton>();
    local_port_->set_range(1, 65535);
    local_port_->set_increments(1, 10);
    local_port_->set_value(existing ? existing->local_port : 1080);

    std::vector<Glib::ustring> names;
    for (const auto& s : model.servers) {
      server_ids_.push_back(s.id);
      names.emplace_back(s.name);
    }
    server_drop_ = Gtk::make_managed<Gtk::DropDown>(names);

    timeout_ = Gtk::make_managed<Gtk::SpinButton>();
    timeout_->set_range(1, 300);
    timeout_->set_increments(1, 5);
    timeout_->set_value(existing ? existing->connect_timeout_sec : 10);

    reconnect_ = Gtk::make_managed<Gtk::SpinButton>();
    reconnect_->set_range(0, 999);
    reconnect_->set_increments(1, 1);
    reconnect_->set_value(existing ? existing->max_reconnect_attempts : 5);
    reconnect_->set_tooltip_text("Maximum consecutive reconnect attempts before giving up (0 = keep trying forever).");

    ping_host_ = Gtk::make_managed<Gtk::Entry>();
    ping_host_->set_text(existing ? existing->ping_host : "1.1.1.1");

    ping_port_ = Gtk::make_managed<Gtk::SpinButton>();
    ping_port_->set_range(1, 65535);
    ping_port_->set_increments(1, 10);
    ping_port_->set_value(existing ? existing->ping_port : 80);

    ping_interval_ = Gtk::make_managed<Gtk::SpinButton>();
    ping_interval_->set_range(1, 3600);
    ping_interval_->set_increments(1, 10);
    ping_interval_->set_value(existing ? existing->ping_interval_sec : 10);

    autostart_ = Gtk::make_managed<Gtk::CheckButton>("Run on startup");
    autostart_->set_active(existing ? existing->run_on_startup : false);

    if (existing) {
      const auto it = std::find(server_ids_.begin(), server_ids_.end(), existing->server_id);
      if (it != server_ids_.end()) {
        server_drop_->set_selected(static_cast<guint>(it - server_ids_.begin()));
      }
    }

    grid_row(*grid, 0, "Local port", *local_port_);
    grid_row(*grid, 1, "SSH server", *server_drop_);
    grid_row(*grid, 2, "Connect timeout (s)", *timeout_);
    grid_row(*grid, 3, "Max reconnect attempts", *reconnect_);
    grid_row(*grid, 4, "Ping host", *ping_host_);
    grid_row(*grid, 5, "Ping port", *ping_port_);
    grid_row(*grid, 6, "Ping interval (s)", *ping_interval_);
    grid->attach(*autostart_, 1, 7, 1, 1);
    box->append(*grid);

    error_ = make_error_label();
    box->append(*error_);

    auto* btns = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    btns->set_halign(Gtk::Align::END);
    auto* cancel = Gtk::make_managed<Gtk::Button>("Cancel");
    auto* save = Gtk::make_managed<Gtk::Button>("Save");
    save->add_css_class("suggested-action");
    cancel->signal_clicked().connect([this] { close(); });
    save->signal_clicked().connect([this, &model] { on_save(model); });
    btns->append(*cancel);
    btns->append(*save);
    box->append(*btns);
  }

  sigc::signal<void(Tunnel)> signal_saved;

private:
  void on_save(const Model& model) {
    if (server_drop_->get_selected() == GTK_INVALID_LIST_POSITION) {
      return set_error(error_, "Add an SSH server first.");
    }
    if (ping_host_->get_text().empty()) {
      return set_error(error_, "Ping host is required.");
    }

    const int port = local_port_->get_value_as_int();
    for (const auto& t : model.tunnels) {
      if (t.id != id_ && t.local_port == port) {
        return set_error(error_, "Local port " + std::to_string(port) + " is already in use.");
      }
    }

    Tunnel t;
    t.id = id_.empty() ? Model::new_id() : id_;
    t.local_port = port;
    t.server_id = server_ids_.at(server_drop_->get_selected());
    t.ping_host = ping_host_->get_text();
    t.ping_port = ping_port_->get_value_as_int();
    t.ping_interval_sec = ping_interval_->get_value_as_int();
    t.connect_timeout_sec = timeout_->get_value_as_int();
    t.max_reconnect_attempts = reconnect_->get_value_as_int();
    t.run_on_startup = autostart_->get_active();

    signal_saved.emit(std::move(t));
    close();
  }

  Gtk::SpinButton* local_port_;
  Gtk::DropDown* server_drop_;
  Gtk::SpinButton* timeout_;
  Gtk::SpinButton* reconnect_;
  Gtk::Entry* ping_host_;
  Gtk::SpinButton* ping_port_;
  Gtk::SpinButton* ping_interval_;
  Gtk::CheckButton* autostart_;
  Gtk::Label* error_;
  std::string id_;
  std::vector<std::string> server_ids_;
};

}  // namespace

Window::Window(Model& model, SecretStore& secrets, TunnelController& controller,
               Log& log)
    : model_(model), secrets_(secrets), controller_(controller), log_(log) {
  build_ui();
  rebuild_tunnels();
  rebuild_servers();
  refresh_log();

  controller_.signal_changed.connect(
      [this](const std::string& id) { refresh_tunnel_row(id); });
  log_.signal_changed.connect([this] { refresh_log(); });
}

Window::~Window() = default;

void Window::build_ui() {
  set_default_size(1080, 480);
  set_title("SSH VPN Manager");

  auto* hb = Gtk::make_managed<Gtk::HeaderBar>();
  hb->set_title_widget(*Gtk::make_managed<Gtk::Label>("SSH VPN Manager"));

  auto* add_tunnel = Gtk::make_managed<Gtk::Button>("Add Tunnel");
  add_tunnel->signal_clicked().connect([this] { on_add_tunnel(); });
  hb->pack_start(*add_tunnel);

  auto* add_server = Gtk::make_managed<Gtk::Button>("Add Server");
  add_server->signal_clicked().connect([this] { on_add_server(); });
  hb->pack_end(*add_server);

  auto* quit = Gtk::make_managed<Gtk::Button>("Quit");
  quit->signal_clicked().connect([this] {
    if (auto app = std::dynamic_pointer_cast<App>(get_application())) {
      app->quit_requested();
    }
  });
  hb->pack_end(*quit);

  set_titlebar(*hb);

  // Closing the window hides it to the tray instead of destroying it; Quit
  // (button or tray menu) actually exits.
  set_hide_on_close(true);

  auto* notebook = Gtk::make_managed<Gtk::Notebook>();

  // --- Tunnels tab ---
  auto* tunnels_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);

  auto* thead = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  thead->set_margin_start(6);
  thead->set_margin_end(6);
  thead->set_margin_top(8);
  thead->set_margin_bottom(4);
  thead->append(*make_cell("Port", 130, 0.0f, false));
  thead->append(*make_cell("Server", 150, 0.0f, false));
  thead->append(*make_cell("Status", 130, 0.0f, false));
  thead->append(*make_cell("Error", 220, 0.0f, true));
  thead->append(*make_cell("Ping", 70, 1.0f, false));
  thead->append(*make_cell("Tx", 70, 1.0f, false));
  thead->append(*make_cell("Rx", 70, 1.0f, false));
  tunnels_box->append(*thead);

  tunnel_list_ = Gtk::make_managed<Gtk::ListBox>();
  tunnel_list_->set_selection_mode(Gtk::SelectionMode::NONE);
  auto* tscroll = Gtk::make_managed<Gtk::ScrolledWindow>();
  tscroll->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
  tscroll->set_child(*tunnel_list_);
  tscroll->set_vexpand(true);
  tunnels_box->append(*tscroll);

  auto* ttab = Gtk::make_managed<Gtk::Label>("Tunnels");
  notebook->append_page(*tunnels_box, *ttab);

  // --- Servers tab ---
  auto* servers_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);

  auto* shead = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  shead->set_margin_start(6);
  shead->set_margin_end(6);
  shead->set_margin_top(8);
  shead->set_margin_bottom(4);
  shead->append(*make_cell("Name", 180, 0.0f, false));
  shead->append(*make_cell("Address", 220, 0.0f, false));
  shead->append(*make_cell("Login", 120, 0.0f, false));
  servers_box->append(*shead);

  server_list_ = Gtk::make_managed<Gtk::ListBox>();
  server_list_->set_selection_mode(Gtk::SelectionMode::NONE);
  auto* sscroll = Gtk::make_managed<Gtk::ScrolledWindow>();
  sscroll->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
  sscroll->set_child(*server_list_);
  sscroll->set_vexpand(true);
  servers_box->append(*sscroll);

  auto* stab = Gtk::make_managed<Gtk::Label>("Servers");
  notebook->append_page(*servers_box, *stab);

  // --- Log tab ---
  auto* log_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);

  auto* toolbar = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  toolbar->set_margin_start(6);
  toolbar->set_margin_end(6);
  toolbar->set_margin_top(8);
  toolbar->set_margin_bottom(4);
  auto* clear_btn = Gtk::make_managed<Gtk::Button>("Clear");
  clear_btn->set_halign(Gtk::Align::END);
  clear_btn->set_hexpand(true);
  clear_btn->signal_clicked().connect([this] { log_.clear(); });
  toolbar->append(*clear_btn);
  log_box->append(*toolbar);

  log_buffer_ = Gtk::TextBuffer::create();
  log_view_ = Gtk::make_managed<Gtk::TextView>(log_buffer_);
  log_view_->set_editable(false);
  log_view_->set_cursor_visible(false);
  log_view_->set_monospace(true);
  log_view_->set_wrap_mode(Gtk::WrapMode::WORD_CHAR);
  auto* log_scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
  log_scroll->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
  log_scroll->set_child(*log_view_);
  log_scroll->set_vexpand(true);
  log_box->append(*log_scroll);

  auto* ltab = Gtk::make_managed<Gtk::Label>("Log");
  notebook->append_page(*log_box, *ltab);

  set_child(*notebook);
}

void Window::rebuild_tunnels() {
  tunnel_rows_.clear();
  tunnel_list_->remove_all();

  for (const auto& t : model_.tunnels) {
    const Server* s = model_.find_server(t.server_id);

    auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    row->set_margin_start(6);
    row->set_margin_end(6);
    row->set_margin_top(4);
    row->set_margin_bottom(4);

    RowWidgets rw;
    rw.port = make_cell("", 130, 0.0f, false);
    rw.server = make_cell(s ? s->name : "<missing>", 150, 0.0f, true);
    rw.status = Gtk::make_managed<Gtk::Label>();
    rw.status->set_size_request(130, -1);
    rw.status->set_xalign(0.0f);
    rw.status->set_ellipsize(Pango::EllipsizeMode::END);
    rw.error = make_cell("", 220, 0.0f, true);
    rw.ping = make_cell("", 70, 1.0f, false);
    rw.tx = make_cell("", 70, 1.0f, false);
    rw.rx = make_cell("", 70, 1.0f, false);

    row->append(*rw.port);
    row->append(*rw.server);
    row->append(*rw.status);
    row->append(*rw.error);
    row->append(*rw.ping);
    row->append(*rw.tx);
    row->append(*rw.rx);

    rw.toggle = Gtk::make_managed<Gtk::Button>();
    rw.toggle->set_icon_name("media-playback-start-symbolic");
    rw.toggle->set_tooltip_text("Run");

    auto* edit = Gtk::make_managed<Gtk::Button>();
    edit->set_icon_name("document-edit-symbolic");
    edit->set_tooltip_text("Edit");

    auto* del = Gtk::make_managed<Gtk::Button>();
    del->set_icon_name("edit-delete-symbolic");
    del->set_tooltip_text("Delete");
    del->add_css_class("destructive-action");

    row->append(*rw.toggle);
    row->append(*edit);
    row->append(*del);

    const std::string id = t.id;
    rw.toggle->signal_clicked().connect([this, id] { on_toggle_tunnel(id); });
    edit->signal_clicked().connect([this, id] { on_edit_tunnel(id); });
    del->signal_clicked().connect([this, id] { on_delete_tunnel(id); });

    tunnel_list_->append(*row);
    tunnel_rows_[id] = rw;
  }

  for (const auto& [id, rw] : tunnel_rows_) {
    (void)rw;
    refresh_tunnel_row(id);
  }
}

void Window::rebuild_servers() {
  server_list_->remove_all();

  for (const auto& s : model_.servers) {
    auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    row->set_margin_start(6);
    row->set_margin_end(6);
    row->set_margin_top(4);
    row->set_margin_bottom(4);

    auto* name = make_cell(s.name, 180, 0.0f, true);
    auto* addr = make_cell(s.host + ":" + std::to_string(s.port), 220, 0.0f, true);
    auto* login = make_cell(s.login, 120, 0.0f, true);

    auto* edit = Gtk::make_managed<Gtk::Button>();
    edit->set_icon_name("document-edit-symbolic");
    edit->set_tooltip_text("Edit");

    auto* del = Gtk::make_managed<Gtk::Button>();
    del->set_icon_name("edit-delete-symbolic");
    del->set_tooltip_text("Delete");
    del->add_css_class("destructive-action");

    row->append(*name);
    row->append(*addr);
    row->append(*login);
    row->append(*edit);
    row->append(*del);

    const std::string id = s.id;
    edit->signal_clicked().connect([this, id] { on_edit_server(id); });
    del->signal_clicked().connect([this, id] { on_delete_server(id); });

    server_list_->append(*row);
  }
}

void Window::refresh_tunnel_row(const std::string& id) {
  auto it = tunnel_rows_.find(id);
  if (it == tunnel_rows_.end()) return;

  const Tunnel* t = model_.find_tunnel(id);
  if (!t) return;
  const Server* s = model_.find_server(t->server_id);
  const TunnelStats st = controller_.stats(id);

  std::string port = "127.0.0.1:" + std::to_string(t->local_port);
  if (t->run_on_startup) port += " ⚡";

  it->second.port->set_text(port);
  it->second.server->set_text(s ? s->name : "<missing>");
  it->second.status->set_markup(status_markup(st));
  it->second.status->set_tooltip_text(st.error);
  it->second.error->set_text(st.error);
  it->second.error->set_tooltip_text(st.error);
  it->second.ping->set_text(ping_text(st.ping_ms));
  it->second.tx->set_text(format_bytes(st.tx_bytes));
  it->second.rx->set_text(format_bytes(st.rx_bytes));
  const bool running = controller_.is_running(id);
  it->second.toggle->set_icon_name(running ? "media-playback-stop-symbolic"
                                           : "media-playback-start-symbolic");
  it->second.toggle->set_tooltip_text(running ? "Stop" : "Run");
}

void Window::refresh_log() {
  if (!log_buffer_ || !log_view_) return;

  std::string text;
  const auto& entries = log_.entries();
  text.reserve(entries.size() * 64);
  for (const auto& line : entries) {
    text += line;
    text += '\n';
  }
  log_buffer_->set_text(text);

  auto end = log_buffer_->end();
  log_buffer_->place_cursor(end);
  log_view_->scroll_to(end);
}

void Window::on_add_server() {
  auto* dlg = new ServerDialog(*this, nullptr);
  dlg->signal_saved.connect([this](Server s, std::string pw, bool changed) {
    std::string err;
    if (!secrets_.set_password(s.id, pw, &err)) {
      show_error("Could not store password: " + err);
      return;
    }
    // Replace or append.
    if (Server* existing = model_.find_server(s.id)) {
      *existing = s;
    } else {
      model_.servers.push_back(std::move(s));
    }
    (void)changed;
    model_.save();
    rebuild_servers();
    rebuild_tunnels();
  });
  dlg->signal_hide().connect([dlg] { delete dlg; });
  dlg->present();
}

void Window::on_edit_server(const std::string& id) {
  const Server* s = model_.find_server(id);
  if (!s) return;
  auto* dlg = new ServerDialog(*this, s);
  dlg->signal_saved.connect([this](Server s, std::string pw, bool changed) {
    if (changed) {
      std::string err;
      if (!secrets_.set_password(s.id, pw, &err)) {
        show_error("Could not store password: " + err);
        return;
      }
    }
    if (Server* existing = model_.find_server(s.id)) {
      *existing = s;
    }
    model_.save();
    rebuild_servers();
    rebuild_tunnels();
  });
  dlg->signal_hide().connect([dlg] { delete dlg; });
  dlg->present();
}

void Window::on_delete_server(const std::string& id) {
  model_.remove_server(id);
  std::string err;
  secrets_.delete_password(id, &err);  // best-effort
  model_.save();
  rebuild_servers();
  rebuild_tunnels();
}

void Window::on_add_tunnel() {
  if (model_.servers.empty()) {
    show_error("Add an SSH server first.");
    return;
  }
  auto* dlg = new TunnelDialog(*this, model_, nullptr);
  dlg->signal_saved.connect([this](Tunnel t) {
    model_.tunnels.push_back(std::move(t));
    model_.save();
    rebuild_tunnels();
  });
  dlg->signal_hide().connect([dlg] { delete dlg; });
  dlg->present();
}

void Window::on_edit_tunnel(const std::string& id) {
  const Tunnel* t = model_.find_tunnel(id);
  if (!t) return;
  auto* dlg = new TunnelDialog(*this, model_, t);
  dlg->signal_saved.connect([this](Tunnel t) {
    const std::string id = t.id;
    if (Tunnel* existing = model_.find_tunnel(id)) {
      *existing = t;
    }
    model_.save();
    rebuild_tunnels();
  });
  dlg->signal_hide().connect([dlg] { delete dlg; });
  dlg->present();
}

void Window::on_delete_tunnel(const std::string& id) {
  controller_.stop(id);
  model_.remove_tunnel(id);
  model_.save();
  rebuild_tunnels();
}

void Window::on_toggle_tunnel(const std::string& id) {
  if (controller_.is_running(id)) {
    controller_.stop(id);
  } else {
    const std::string err = controller_.start(id);
    if (!err.empty()) {
      show_error(err);
    }
  }
  refresh_tunnel_row(id);
}

void Window::show_error(const Glib::ustring& message) {
  auto dlg = Gtk::AlertDialog::create(message);
  dlg->set_buttons({"OK"});
  dlg->show(*this);
}

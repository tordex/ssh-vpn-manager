#pragma once

#include <gtkmm.h>

#include <map>
#include <string>

#include "model.hpp"
#include "secret_store.hpp"
#include "tunnel_controller.hpp"

// Main application window: two tabs (Tunnels, Servers) plus dialogs for editing.
class Window : public Gtk::ApplicationWindow {
public:
  Window(Model& model, SecretStore& secrets, TunnelController& controller);
  ~Window() override;

  // Called by App before quitting so the close-request handler lets the window go.
  void allow_close();

private:
  void build_ui();

  void rebuild_tunnels();
  void rebuild_servers();
  void refresh_tunnel_row(const std::string& id);

  void on_add_server();
  void on_add_tunnel();
  void on_toggle_tunnel(const std::string& id);
  void on_edit_tunnel(const std::string& id);
  void on_delete_tunnel(const std::string& id);
  void on_edit_server(const std::string& id);
  void on_delete_server(const std::string& id);

  void show_error(const Glib::ustring& message);

  Model& model_;
  SecretStore& secrets_;
  TunnelController& controller_;

  bool quitting_ = false;

  Gtk::ListBox* tunnel_list_ = nullptr;
  Gtk::ListBox* server_list_ = nullptr;

  // Per-tunnel row labels (for in-place stats updates), keyed by tunnel id.
  struct RowWidgets {
    Gtk::Label* port = nullptr;
    Gtk::Label* server = nullptr;
    Gtk::Label* status = nullptr;
    Gtk::Label* ping = nullptr;
    Gtk::Label* tx = nullptr;
    Gtk::Label* rx = nullptr;
    Gtk::Button* toggle = nullptr;
  };
  std::map<std::string, RowWidgets> tunnel_rows_;
};

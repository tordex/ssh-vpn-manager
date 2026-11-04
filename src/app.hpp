#pragma once

#include <gtkmm.h>

#include <memory>

#include "model.hpp"
#include "secret_store.hpp"
#include "tray.hpp"
#include "tunnel_controller.hpp"

class Window;

class App : public Gtk::Application {
public:
  static Glib::RefPtr<App> create();

  // Request a clean shutdown (stops tunnels, flushes config, then quits).
  void quit_requested();

protected:
  App();

  void on_startup() override;
  void on_activate() override;
  void on_shutdown() override;

private:
  void create_window();
  void show_window();
  void start_autostart_tunnels();

  Model model_;
  SecretStore secrets_;
  std::unique_ptr<TunnelController> controller_;
  std::unique_ptr<TrayIcon> tray_;
  Window* window_ = nullptr;  // owned by Gtk::Application
  bool autostart_done_ = false;
};

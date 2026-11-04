#include "app.hpp"

#include "window.hpp"

App::App() : Gtk::Application("org.sshvpnmanager.SshVpnManager") {}

Glib::RefPtr<App> App::create() {
  return Glib::make_refptr_for_instance<App>(new App());
}

void App::on_startup() {
  Gtk::Application::on_startup();

  std::string err;
  if (!model_.load(&err)) {
    g_warning("failed to load config: %s", err.c_str());
  }

  controller_ = std::make_unique<TunnelController>(model_, secrets_);

  tray_ = std::make_unique<TrayIcon>(
      [this] { show_window(); },
      [this] { quit_requested(); });
}

void App::on_activate() {
  create_window();
  window_->present();
  if (!autostart_done_) {
    autostart_done_ = true;
    start_autostart_tunnels();
  }
}

void App::on_shutdown() {
  if (controller_) {
    controller_->stop_all();
  }
  std::string err;
  model_.save(&err);
  Gtk::Application::on_shutdown();
}

void App::create_window() {
  if (window_) {
    return;
  }
  window_ = new Window(model_, secrets_, *controller_);
  add_window(*window_);
}

void App::show_window() {
  create_window();
  window_->present();
}

void App::quit_requested() {
  if (window_) {
    window_->allow_close();
  }
  quit();
}

void App::start_autostart_tunnels() {
  for (const auto& t : model_.tunnels) {
    if (t.run_on_startup) {
      const std::string err = controller_->start(t.id);
      if (!err.empty()) {
        g_warning("autostart tunnel failed: %s", err.c_str());
      }
    }
  }
}

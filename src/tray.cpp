#include "tray.hpp"

#ifdef HAVE_APPINDICATOR

#include <ayatana-appindicator.h>

#include <utility>

struct TrayIcon::Impl {
  AppIndicator* indicator = nullptr;
  GSimpleActionGroup* actions = nullptr;
  GMenu* menu = nullptr;
  std::function<void()> on_show;
  std::function<void()> on_quit;
};

namespace {

void on_activate_event(AppIndicator* indicator, gint x, gint y, gpointer data) {
  (void)indicator;
  (void)x;
  (void)y;
  auto* impl = static_cast<TrayIcon::Impl*>(data);
  if (impl->on_show) {
    impl->on_show();
  }
}

void on_show_action(GSimpleAction* action, GVariant* parameter, gpointer data) {
  (void)action;
  (void)parameter;
  auto* impl = static_cast<TrayIcon::Impl*>(data);
  if (impl->on_show) {
    impl->on_show();
  }
}

void on_quit_action(GSimpleAction* action, GVariant* parameter, gpointer data) {
  (void)action;
  (void)parameter;
  auto* impl = static_cast<TrayIcon::Impl*>(data);
  if (impl->on_quit) {
    impl->on_quit();
  }
}

}  // namespace

TrayIcon::TrayIcon(std::function<void()> on_show, std::function<void()> on_quit) {
  impl_ = new Impl;
  impl_->on_show = std::move(on_show);
  impl_->on_quit = std::move(on_quit);

  impl_->indicator = app_indicator_new("org.sshvpnmanager.SshVpnManager",
                                       "org.sshvpnmanager.SshVpnManager",
                                       APP_INDICATOR_CATEGORY_APPLICATION_STATUS);
  app_indicator_set_status(impl_->indicator, APP_INDICATOR_STATUS_ACTIVE);
  app_indicator_set_title(impl_->indicator, "SSH VPN Manager");

  g_signal_connect(impl_->indicator, "activate-event", G_CALLBACK(on_activate_event), impl_);

  impl_->actions = g_simple_action_group_new();
  GSimpleAction* a_show = g_simple_action_new("show", nullptr);
  g_signal_connect(a_show, "activate", G_CALLBACK(on_show_action), impl_);
  g_action_map_add_action(G_ACTION_MAP(impl_->actions), G_ACTION(a_show));
  g_object_unref(a_show);

  GSimpleAction* a_quit = g_simple_action_new("quit", nullptr);
  g_signal_connect(a_quit, "activate", G_CALLBACK(on_quit_action), impl_);
  g_action_map_add_action(G_ACTION_MAP(impl_->actions), G_ACTION(a_quit));
  g_object_unref(a_quit);

  impl_->menu = g_menu_new();
  GMenuItem* item = g_menu_item_new("Show", "indicator.show");
  g_menu_append_item(impl_->menu, item);
  g_object_unref(item);
  item = g_menu_item_new("Quit", "indicator.quit");
  g_menu_append_item(impl_->menu, item);
  g_object_unref(item);

  app_indicator_set_menu(impl_->indicator, impl_->menu);
  app_indicator_set_actions(impl_->indicator, impl_->actions);
}

TrayIcon::~TrayIcon() {
  if (!impl_) {
    return;
  }
  if (impl_->menu) {
    g_object_unref(impl_->menu);
  }
  if (impl_->actions) {
    g_object_unref(impl_->actions);
  }
  // The indicator itself lives for the whole process lifetime.
  delete impl_;
  impl_ = nullptr;
}

#else  // !HAVE_APPINDICATOR

struct TrayIcon::Impl {};

TrayIcon::TrayIcon(std::function<void()>, std::function<void()>) { impl_ = new Impl; }
TrayIcon::~TrayIcon() {
  delete impl_;
  impl_ = nullptr;
}

#endif

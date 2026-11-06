#pragma once

#include <functional>

// System tray (StatusNotifierItem) icon. When built without
// ayatana-appindicator-glib this is a no-op and the app falls back to
// "close hides the window, re-launch shows it".
class TrayIcon
{
  public:
    TrayIcon(std::function<void()> on_show, std::function<void()> on_quit);
    ~TrayIcon();

    TrayIcon(const TrayIcon&)            = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

    // Opaque implementation type (defined in tray.cpp). Public so the GLib C
    // callbacks in tray.cpp can reference it; it is incomplete here.
    struct Impl;

  private:
    Impl* impl_ = nullptr;
};

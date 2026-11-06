#include "app.hpp"

#include "window.hpp"

App::App() :
    Gtk::Application("org.sshvpnmanager.SshVpnManager")
{
    add_main_option_entry(OptionType::BOOL, "hidden", '\0', "Run without showing the main window (tray icon only).");
}

Glib::RefPtr<App> App::create()
{
    return Glib::make_refptr_for_instance<App>(new App());
}

void App::on_startup()
{
    Gtk::Application::on_startup();

    // Keep the window connected to the application while it is hidden, so the app
    // keeps running in the tray. Otherwise Gtk::Application quits when its last
    // window is hidden/removed. (Closing hides instead of destroying, see
    // Window::build_ui.)
    Gtk::Window::set_remove_on_hide(false);

    std::string err;
    if(!model_.load(&err))
    {
        g_warning("failed to load config: %s", err.c_str());
    }

    controller_ = std::make_unique<TunnelController>(model_, secrets_, log_);
    singbox_    = std::make_unique<SingboxController>(model_, log_);

    tray_ = std::make_unique<TrayIcon>([this] { show_window(); }, [this] { quit_requested(); });
}

void App::on_activate()
{
    if(!autostart_done_)
    {
        autostart_done_ = true;
        start_autostart_tunnels();
        if(hidden_)
        {
            // Hidden mode: no main window, keep running with only the tray icon.
            // Gtk::Application quits when it has no windows, so hold it alive.
            hold();
            return;
        }
    }
    create_window();
    window_->present();
}

int App::on_handle_local_options(const Glib::RefPtr<Glib::VariantDict>& options)
{
    if(options->contains("hidden"))
    {
        hidden_ = true;
    }
    return -1; // continue with normal activation
}

void App::on_shutdown()
{
    if(singbox_)
    {
        singbox_->stop();
    }
    if(controller_)
    {
        controller_->stop_all();
    }
    std::string err;
    model_.save(&err);
    Gtk::Application::on_shutdown();
}

void App::create_window()
{
    if(window_)
    {
        return;
    }
    window_ = new Window(model_, secrets_, *controller_, *singbox_, log_);
    add_window(*window_);
}

void App::show_window()
{
    create_window();
    window_->present();
}

void App::quit_requested()
{
    quit();
}

void App::start_autostart_tunnels()
{
    for(const auto& t : model_.tunnels)
    {
        if(t.run_on_startup)
        {
            const std::string err = controller_->start(t.id);
            if(!err.empty())
            {
                g_warning("autostart tunnel failed: %s", err.c_str());
            }
        }
    }
}

#include "window.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <pango/pango.h>

#include "app.hpp"

namespace
{

    std::string format_bytes(std::uint64_t b)
    {
        static const char* units[] = {"B", "KB", "MB", "GB", "TB"};
        double             v       = static_cast<double>(b);
        int                i       = 0;
        while(v >= 1024.0 && i < 4)
        {
            v /= 1024.0;
            ++i;
        }
        char buf[32];
        std::snprintf(buf, sizeof buf, i == 0 ? "%.0f %s" : "%.1f %s", v, units[i]);
        return buf;
    }

    std::string ping_text(double ms)
    {
        if(ms < 0.0)
        {
            return "—";
        }
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.0f ms", ms);
        return buf;
    }

    const char* status_name(TunnelStatus s)
    {
        switch(s)
        {
        case TunnelStatus::Stopped:
            return "Stopped";
        case TunnelStatus::Connecting:
            return "Connecting";
        case TunnelStatus::Connected:
            return "Connected";
        case TunnelStatus::Reconnecting:
            return "Reconnecting";
        case TunnelStatus::Error:
            return "Error";
        }
        return "Unknown";
    }

    const char* status_color(TunnelStatus s)
    {
        switch(s)
        {
        case TunnelStatus::Stopped:
            return "#9a9996";
        case TunnelStatus::Connecting:
            return "#3584e4";
        case TunnelStatus::Connected:
            return "#26a269";
        case TunnelStatus::Reconnecting:
            return "#e66100";
        case TunnelStatus::Error:
            return "#c01c28";
        }
        return "#9a9996";
    }

    std::string status_markup(const TunnelStats& st)
    {
        std::string name = status_name(st.status);
        if(st.status == TunnelStatus::Reconnecting && st.max_reconnect_attempts > 0 && st.reconnect_attempt > 0)
        {
            name += " (" + std::to_string(st.reconnect_attempt) + "/" + std::to_string(st.max_reconnect_attempts) + ")";
        }
        std::string m  = "<span color='";
        m             += status_color(st.status);
        m             += "'>";
        m             += name;
        m             += "</span>";
        return m;
    }

    // Cap an ellipsizing label's natural width to `width` pixels. Long text then
    // ellipsizes within its column instead of widening it (which would push the
    // following columns out of alignment). The character count uses the same font
    // metric GTK itself uses for set_max_width_chars() (the larger of the font's
    // approximate char width and digit width), so it stays exact under any
    // font size or DPI scaling.
    void constrain_ellipsized(Gtk::Label& label, int width)
    {
        label.set_ellipsize(Pango::EllipsizeMode::END);

        PangoContext*     ctx         = label.get_pango_context()->gobj();
        PangoFontMetrics* metrics     = pango_context_get_metrics(ctx, nullptr, nullptr);
        const int         char_pixels = std::max(pango_font_metrics_get_approximate_char_width(metrics),
                                                 pango_font_metrics_get_approximate_digit_width(metrics));
        pango_font_metrics_unref(metrics);

        const int px_per_char = char_pixels / PANGO_SCALE;
        label.set_max_width_chars(px_per_char > 0 ? width / px_per_char : width / 8);
    }

    Gtk::Label* make_cell(const std::string& text, int width, float xalign, bool ellipsize)
    {
        auto* l = Gtk::make_managed<Gtk::Label>(text);
        l->set_size_request(width, -1);
        l->set_xalign(xalign);
        if(ellipsize)
        {
            constrain_ellipsized(*l, width);
        }
        return l;
    }

    Gtk::Button* make_icon_button(const std::string& label, const char* icon)
    {
        auto* btn = Gtk::make_managed<Gtk::Button>();
        auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
        auto* img = Gtk::make_managed<Gtk::Image>();
        img->set_from_icon_name(icon);
        box->append(*img);
        box->append(*Gtk::make_managed<Gtk::Label>(label));
        btn->set_child(*box);
        return btn;
    }

    std::vector<const Tunnel*> sorted_tunnels(const Model& model)
    {
        std::vector<const Tunnel*> v;
        v.reserve(model.tunnels.size());
        for(const auto& t : model.tunnels)
        {
            v.push_back(&t);
        }
        std::stable_sort(v.begin(), v.end(),
                         [](const Tunnel* a, const Tunnel* b) { return a->local_port < b->local_port; });
        return v;
    }

    std::vector<const Server*> sorted_servers(const Model& model)
    {
        std::vector<const Server*> v;
        v.reserve(model.servers.size());
        for(const auto& s : model.servers)
        {
            v.push_back(&s);
        }
        std::stable_sort(v.begin(), v.end(), [](const Server* a, const Server* b) { return a->name < b->name; });
        return v;
    }

    void grid_row(Gtk::Grid& grid, int row, const char* label, Gtk::Widget& w)
    {
        auto* l = Gtk::make_managed<Gtk::Label>(label);
        l->set_halign(Gtk::Align::END);
        l->set_margin_end(8);
        grid.attach(*l, 0, row, 1, 1);
        w.set_hexpand(true);
        grid.attach(w, 1, row, 1, 1);
    }

    Gtk::Label* make_error_label()
    {
        auto* l = Gtk::make_managed<Gtk::Label>();
        l->set_markup("<span color='#c01c28'></span>");
        l->set_visible(false);
        l->set_wrap(true);
        l->set_xalign(0.0f);
        return l;
    }

    void set_error(Gtk::Label* l, const std::string& msg)
    {
        if(msg.empty())
        {
            l->set_visible(false);
            return;
        }
        l->set_markup("<span color='#c01c28'>" + msg + "</span>");
        l->set_visible(true);
    }

    // --- Server add/edit dialog --------------------------------------------------

    class ServerDialog : public Gtk::Window
    {
      public:
        ServerDialog(Gtk::Window& parent, const Server* existing) :
            id_(existing ? existing->id : "")
        {
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

            name_     = Gtk::make_managed<Gtk::Entry>();
            host_     = Gtk::make_managed<Gtk::Entry>();
            login_    = Gtk::make_managed<Gtk::Entry>();
            password_ = Gtk::make_managed<Gtk::Entry>();
            password_->set_visibility(false);
            password_->set_placeholder_text(existing ? "(unchanged)" : "required");
            password_->set_input_purpose(Gtk::InputPurpose::PASSWORD);

            port_ = Gtk::make_managed<Gtk::SpinButton>();
            port_->set_range(1, 65535);
            port_->set_increments(1, 10);
            port_->set_value(existing ? existing->port : 22);

            ping_host_ = Gtk::make_managed<Gtk::Entry>();
            ping_host_->set_text(existing ? existing->ping_host : "1.1.1.1");

            ping_port_ = Gtk::make_managed<Gtk::SpinButton>();
            ping_port_->set_range(1, 65535);
            ping_port_->set_increments(1, 10);
            ping_port_->set_value(existing ? existing->ping_port : 80);

            auto* show_pw = Gtk::make_managed<Gtk::CheckButton>("Show password");
            show_pw->signal_toggled().connect([this, show_pw] { password_->set_visibility(show_pw->get_active()); });

            if(existing)
            {
                name_->set_text(existing->name);
                host_->set_text(existing->host);
                login_->set_text(existing->login);
            }

            grid_row(*grid, 0, "Name", *name_);
            grid_row(*grid, 1, "Host / IP", *host_);
            grid_row(*grid, 2, "Port", *port_);
            grid_row(*grid, 3, "Login", *login_);
            grid_row(*grid, 4, "Password", *password_);
            grid_row(*grid, 5, "Ping host", *ping_host_);
            grid_row(*grid, 6, "Ping port", *ping_port_);
            grid->attach(*show_pw, 1, 7, 1, 1);
            box->append(*grid);

            error_ = make_error_label();
            box->append(*error_);

            auto* btns = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
            btns->set_halign(Gtk::Align::END);
            auto* cancel = Gtk::make_managed<Gtk::Button>("Cancel");
            auto* save   = Gtk::make_managed<Gtk::Button>("Save");
            save->add_css_class("suggested-action");
            cancel->signal_clicked().connect([this] { close(); });
            save->signal_clicked().connect([this] { on_save(); });
            btns->append(*cancel);
            btns->append(*save);
            box->append(*btns);
        }

        sigc::signal<void(Server, std::string, bool)> signal_saved;

      private:
        void on_save()
        {
            const std::string name  = name_->get_text();
            const std::string host  = host_->get_text();
            const std::string login = login_->get_text();
            const std::string pw    = password_->get_text();

            if(name.empty())
            {
                return set_error(error_, "Name is required.");
            }
            if(host.empty())
            {
                return set_error(error_, "Host / IP is required.");
            }
            if(login.empty())
            {
                return set_error(error_, "Login is required.");
            }
            if(id_.empty() && pw.empty())
            {
                return set_error(error_, "Password is required.");
            }

            Server s;
            s.id        = id_.empty() ? Model::new_id() : id_;
            s.name      = name;
            s.host      = host;
            s.port      = port_->get_value_as_int();
            s.login     = login;
            s.ping_host = ping_host_->get_text();
            s.ping_port = ping_port_->get_value_as_int();

            signal_saved.emit(std::move(s), pw, !pw.empty());
            close();
        }

        Gtk::Entry*      name_;
        Gtk::Entry*      host_;
        Gtk::Entry*      login_;
        Gtk::Entry*      password_;
        Gtk::SpinButton* port_;
        Gtk::Entry*      ping_host_;
        Gtk::SpinButton* ping_port_;
        Gtk::Label*      error_;
        std::string      id_;
    };

    // --- Tunnel add/edit dialog --------------------------------------------------

    class TunnelDialog : public Gtk::Window
    {
      public:
        TunnelDialog(Gtk::Window& parent, const Model& model, const Tunnel* existing) :
            id_(existing ? existing->id : "")
        {
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
            for(const Server* s : sorted_servers(model))
            {
                server_ids_.push_back(s->id);
                names.emplace_back(s->name);
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
            reconnect_->set_tooltip_text(
                "Maximum consecutive reconnect attempts before giving up (0 = keep trying forever).");

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

            server_alive_interval_ = Gtk::make_managed<Gtk::SpinButton>();
            server_alive_interval_->set_range(0, 86400);
            server_alive_interval_->set_increments(1, 10);
            server_alive_interval_->set_value(existing ? existing->server_alive_interval : 0);
            server_alive_interval_->set_tooltip_text("Send a keepalive after this many idle seconds (0 = disabled).");

            server_alive_count_max_ = Gtk::make_managed<Gtk::SpinButton>();
            server_alive_count_max_->set_range(1, 999);
            server_alive_count_max_->set_increments(1, 1);
            server_alive_count_max_->set_value(existing ? existing->server_alive_count_max : 3);
            server_alive_count_max_->set_tooltip_text("Disconnect after this many unanswered keepalives.");

            autostart_ = Gtk::make_managed<Gtk::CheckButton>("Run on startup");
            autostart_->set_active(existing ? existing->run_on_startup : false);

            if(existing)
            {
                const auto it = std::find(server_ids_.begin(), server_ids_.end(), existing->server_id);
                if(it != server_ids_.end())
                {
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
            grid_row(*grid, 7, "Server alive interval (s)", *server_alive_interval_);
            grid_row(*grid, 8, "Server alive count max", *server_alive_count_max_);
            grid->attach(*autostart_, 1, 9, 1, 1);
            box->append(*grid);

            error_ = make_error_label();
            box->append(*error_);

            auto* btns = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
            btns->set_halign(Gtk::Align::END);
            auto* cancel = Gtk::make_managed<Gtk::Button>("Cancel");
            auto* save   = Gtk::make_managed<Gtk::Button>("Save");
            save->add_css_class("suggested-action");
            cancel->signal_clicked().connect([this] { close(); });
            save->signal_clicked().connect([this] { on_save(); });
            btns->append(*cancel);
            btns->append(*save);
            box->append(*btns);
        }

        sigc::signal<void(Tunnel)> signal_saved;

      private:
        void on_save()
        {
            if(server_drop_->get_selected() == GTK_INVALID_LIST_POSITION)
            {
                return set_error(error_, "Add an SSH server first.");
            }
            if(ping_host_->get_text().empty())
            {
                return set_error(error_, "Ping host is required.");
            }

            const int port = local_port_->get_value_as_int();

            Tunnel t;
            t.id                     = id_.empty() ? Model::new_id() : id_;
            t.local_port             = port;
            t.server_id              = server_ids_.at(server_drop_->get_selected());
            t.ping_host              = ping_host_->get_text();
            t.ping_port              = ping_port_->get_value_as_int();
            t.ping_interval_sec      = ping_interval_->get_value_as_int();
            t.connect_timeout_sec    = timeout_->get_value_as_int();
            t.max_reconnect_attempts = reconnect_->get_value_as_int();
            t.server_alive_interval  = server_alive_interval_->get_value_as_int();
            t.server_alive_count_max = server_alive_count_max_->get_value_as_int();
            t.run_on_startup         = autostart_->get_active();

            signal_saved.emit(std::move(t));
            close();
        }

        Gtk::SpinButton*         local_port_;
        Gtk::DropDown*           server_drop_;
        Gtk::SpinButton*         timeout_;
        Gtk::SpinButton*         reconnect_;
        Gtk::Entry*              ping_host_;
        Gtk::SpinButton*         ping_port_;
        Gtk::SpinButton*         ping_interval_;
        Gtk::SpinButton*         server_alive_interval_;
        Gtk::SpinButton*         server_alive_count_max_;
        Gtk::CheckButton*        autostart_;
        Gtk::Label*              error_;
        std::string              id_;
        std::vector<std::string> server_ids_;
    };

    // --- Settings dialog --------------------------------------------------------

    class SettingsDialog : public Gtk::Window
    {
      public:
        SettingsDialog(Gtk::Window& parent, const Model& model)
        {
            set_modal(true);
            set_transient_for(parent);
            set_title("Settings");
            set_default_size(560, -1);

            auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
            box->set_margin(16);
            set_child(*box);

            on_cmd_  = make_command_editor(*box, "Command to run after routing starts");
            off_cmd_ = make_command_editor(*box, "Command to run after routing stops");

            auto* hint = Gtk::make_managed<Gtk::Label>();
            hint->set_markup("<span size='small' color='#9a9996'>Leave a command empty to skip it.</span>");
            hint->set_xalign(0.0f);
            box->append(*hint);

            auto* btns = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
            btns->set_halign(Gtk::Align::END);
            auto* cancel = Gtk::make_managed<Gtk::Button>("Cancel");
            auto* save   = Gtk::make_managed<Gtk::Button>("Save");
            save->add_css_class("suggested-action");
            cancel->signal_clicked().connect([this] { close(); });
            save->signal_clicked().connect([this] { on_save(); });
            btns->append(*cancel);
            btns->append(*save);
            box->append(*btns);

            on_cmd_->get_buffer()->set_text(model.route_on_command);
            off_cmd_->get_buffer()->set_text(model.route_off_command);
        }

        sigc::signal<void(std::string, std::string)> signal_saved;

      private:
        Gtk::TextView* make_command_editor(Gtk::Box& box, const char* label)
        {
            auto* l = Gtk::make_managed<Gtk::Label>(label);
            l->set_halign(Gtk::Align::START);
            l->set_xalign(0.0f);
            box.append(*l);

            auto* tv = Gtk::make_managed<Gtk::TextView>();
            tv->set_monospace(true);
            tv->set_wrap_mode(Gtk::WrapMode::WORD_CHAR);
            tv->set_left_margin(8);
            tv->set_top_margin(6);
            tv->set_bottom_margin(6);

            auto* sw = Gtk::make_managed<Gtk::ScrolledWindow>();
            sw->set_policy(Gtk::PolicyType::AUTOMATIC, Gtk::PolicyType::AUTOMATIC);
            sw->set_min_content_height(80);
            sw->set_child(*tv);
            box.append(*sw);

            return tv;
        }

        void on_save()
        {
            signal_saved.emit(on_cmd_->get_buffer()->get_text(), off_cmd_->get_buffer()->get_text());
            close();
        }

        Gtk::TextView* on_cmd_;
        Gtk::TextView* off_cmd_;
    };

} // namespace

Window::Window(Model& model, SecretStore& secrets, TunnelController& controller, SingboxController& singbox, Log& log) :
    model_(model),
    secrets_(secrets),
    controller_(controller),
    singbox_(singbox),
    log_(log),
    server_checker_(model, secrets)
{
    build_ui();
    rebuild_tunnels();
    rebuild_servers();
    refresh_log();

    controller_.signal_changed.connect([this](const std::string& id) { refresh_tunnel_row(id); });
    singbox_.signal_changed.connect([this] {
        for(const auto& [id, rw] : tunnel_rows_)
        {
            (void) rw;
            refresh_tunnel_row(id);
        }
    });
    log_.signal_changed.connect([this] { refresh_log(); });
    server_checker_.signal_changed.connect([this](const std::string& id) {
        refresh_server_row(id);
        log_server_result(id);
    });
    server_checker_.signal_finished.connect([this] {
        if(check_btn_)
        {
            check_btn_->set_sensitive(true);
        }
    });
}

Window::~Window() = default;

void Window::build_ui()
{
    set_default_size(1080, 480);
    set_title("SSH VPN Manager");

    auto* hb = Gtk::make_managed<Gtk::HeaderBar>();
    hb->set_title_widget(*Gtk::make_managed<Gtk::Label>("SSH VPN Manager"));

    auto* add_tunnel = make_icon_button("Add Tunnel", "list-add-symbolic");
    add_tunnel->signal_clicked().connect([this] { on_add_tunnel(); });
    hb->pack_start(*add_tunnel);

    auto* add_server = make_icon_button("Add Server", "list-add-symbolic");
    add_server->signal_clicked().connect([this] { on_add_server(); });
    hb->pack_start(*add_server);

    check_btn_ = make_icon_button("Check servers", "view-refresh-symbolic");
    check_btn_->signal_clicked().connect([this] { on_check_servers(); });
    hb->pack_start(*check_btn_);

    auto* settings = make_icon_button("Settings", "preferences-system-symbolic");
    settings->signal_clicked().connect([this] { on_settings(); });
    hb->pack_end(*settings);

    auto* quit = Gtk::make_managed<Gtk::Button>("Quit");
    quit->signal_clicked().connect([this] {
        if(auto app = std::dynamic_pointer_cast<App>(get_application()))
        {
            app->quit_requested();
        }
    });
    hb->pack_end(*quit);

    set_titlebar(*hb);

    // Closing the window hides it to the tray instead of destroying it; Quit
    // (button or tray menu) actually exits.
    set_hide_on_close(true);

    auto* notebook = Gtk::make_managed<Gtk::Notebook>();
    notebook_      = notebook;

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
    shead->append(*make_cell("Status", 140, 0.0f, true));
    shead->append(*make_cell("Ping", 70, 1.0f, false));
    servers_box->append(*shead);

    server_list_ = Gtk::make_managed<Gtk::ListBox>();
    server_list_->set_selection_mode(Gtk::SelectionMode::NONE);
    auto* sscroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    sscroll->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    sscroll->set_child(*server_list_);
    sscroll->set_vexpand(true);
    servers_box->append(*sscroll);

    auto* stab    = Gtk::make_managed<Gtk::Label>("Servers");
    servers_page_ = notebook->append_page(*servers_box, *stab);

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
    log_view_   = Gtk::make_managed<Gtk::TextView>(log_buffer_);
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

void Window::rebuild_tunnels()
{
    tunnel_rows_.clear();
    tunnel_list_->remove_all();

    for(const Tunnel* tp : sorted_tunnels(model_))
    {
        const Tunnel& t = *tp;
        const Server* s = model_.find_server(t.server_id);

        auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        row->set_margin_start(6);
        row->set_margin_end(6);
        row->set_margin_top(4);
        row->set_margin_bottom(4);

        RowWidgets rw;
        rw.port   = make_cell("", 130, 0.0f, false);
        rw.server = make_cell(s ? s->name : "<missing>", 150, 0.0f, true);
        rw.status = Gtk::make_managed<Gtk::Label>();
        rw.status->set_size_request(130, -1);
        rw.status->set_xalign(0.0f);
        constrain_ellipsized(*rw.status, 130);
        rw.error = make_cell("", 220, 0.0f, true);
        rw.ping  = make_cell("", 70, 1.0f, false);
        rw.tx    = make_cell("", 70, 1.0f, false);
        rw.rx    = make_cell("", 70, 1.0f, false);

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

        rw.route = Gtk::make_managed<Gtk::Button>();
        rw.route->set_icon_name("network-vpn-symbolic");
        rw.route->set_tooltip_text("Route all traffic through this tunnel");

        auto* edit = Gtk::make_managed<Gtk::Button>();
        edit->set_icon_name("document-edit-symbolic");
        edit->set_tooltip_text("Edit");

        auto* del = Gtk::make_managed<Gtk::Button>();
        del->set_icon_name("edit-delete-symbolic");
        del->set_tooltip_text("Delete");
        del->add_css_class("destructive-action");

        row->append(*rw.toggle);
        row->append(*rw.route);
        row->append(*edit);
        row->append(*del);

        const std::string id = t.id;
        rw.toggle->signal_clicked().connect([this, id] { on_toggle_tunnel(id); });
        rw.route->signal_clicked().connect([this, id] { on_route_tunnel(id); });
        edit->signal_clicked().connect([this, id] { on_edit_tunnel(id); });
        del->signal_clicked().connect([this, id] { on_delete_tunnel(id); });

        tunnel_list_->append(*row);
        tunnel_rows_[id] = rw;
    }

    for(const auto& [id, rw] : tunnel_rows_)
    {
        (void) rw;
        refresh_tunnel_row(id);
    }
}

void Window::rebuild_servers()
{
    server_list_->remove_all();
    server_rows_.clear();

    for(const Server* sp : sorted_servers(model_))
    {
        const Server& s   = *sp;
        auto*         row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        row->set_margin_start(6);
        row->set_margin_end(6);
        row->set_margin_top(4);
        row->set_margin_bottom(4);

        auto* name  = make_cell(s.name, 180, 0.0f, true);
        auto* addr  = make_cell(s.host + ":" + std::to_string(s.port), 220, 0.0f, true);
        auto* login = make_cell(s.login, 120, 0.0f, true);

        ServerRowWidgets sw;
        sw.status = make_cell("", 140, 0.0f, true);
        sw.ping   = make_cell("", 70, 1.0f, false);

        auto* check = Gtk::make_managed<Gtk::Button>();
        check->set_icon_name("view-refresh-symbolic");
        check->set_tooltip_text("Check");

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
        row->append(*sw.status);
        row->append(*sw.ping);
        row->append(*check);
        row->append(*edit);
        row->append(*del);

        const std::string id = s.id;
        check->signal_clicked().connect([this, id] { on_check_server(id); });
        edit->signal_clicked().connect([this, id] { on_edit_server(id); });
        del->signal_clicked().connect([this, id] { on_delete_server(id); });

        server_list_->append(*row);
        server_rows_[id] = sw;
    }

    for(const auto& [id, rw] : server_rows_)
    {
        (void) rw;
        refresh_server_row(id);
    }
}

void Window::refresh_tunnel_row(const std::string& id)
{
    auto it = tunnel_rows_.find(id);
    if(it == tunnel_rows_.end())
    {
        return;
    }

    const Tunnel* t = model_.find_tunnel(id);
    if(!t)
    {
        return;
    }
    const Server*     s  = model_.find_server(t->server_id);
    const TunnelStats st = controller_.stats(id);

    std::string port = "127.0.0.1:" + std::to_string(t->local_port);
    if(t->run_on_startup)
    {
        port += " ⚡";
    }

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
    it->second.toggle->set_icon_name(running ? "media-playback-stop-symbolic" : "media-playback-start-symbolic");
    it->second.toggle->set_tooltip_text(running ? "Stop" : "Run");

    const bool routing = singbox_.active_tunnel_id() == id;
    if(routing)
    {
        it->second.route->add_css_class("suggested-action");
        it->second.route->set_tooltip_text("Stop routing all traffic");
    } else
    {
        it->second.route->remove_css_class("suggested-action");
        it->second.route->set_tooltip_text("Route all traffic through this tunnel");
    }
}

void Window::refresh_server_row(const std::string& id)
{
    auto it = server_rows_.find(id);
    if(it == server_rows_.end())
    {
        return;
    }

    const ServerCheckResult r = server_checker_.result(id);
    if(r.checking)
    {
        it->second.status->set_markup("<span color='#3584e4'>Checking</span>");
        it->second.status->set_tooltip_text("");
        it->second.ping->set_text("—");
        return;
    }
    if(!r.checked)
    {
        it->second.status->set_text("—");
        it->second.status->set_tooltip_text("");
        it->second.ping->set_text("—");
        return;
    }
    if(r.ok)
    {
        it->second.status->set_markup("<span color='#26a269'>OK</span>");
        it->second.status->set_tooltip_text("");
        it->second.ping->set_text(ping_text(r.ping_ms));
    } else
    {
        it->second.status->set_markup("<span color='#c01c28'>Error</span>");
        it->second.status->set_tooltip_text(r.error);
        it->second.ping->set_text("—");
    }
}

void Window::refresh_log()
{
    if(!log_buffer_ || !log_view_)
    {
        return;
    }

    std::string text;
    const auto& entries = log_.entries();
    text.reserve(entries.size() * 64);
    for(const auto& line : entries)
    {
        text += line;
        text += '\n';
    }
    log_buffer_->set_text(text);

    auto end = log_buffer_->end();
    log_buffer_->place_cursor(end);
    log_view_->scroll_to(end);
}

void Window::on_settings()
{
    auto* dlg = new SettingsDialog(*this, model_);
    dlg->signal_saved.connect([this](std::string on_cmd, std::string off_cmd) {
        model_.route_on_command  = std::move(on_cmd);
        model_.route_off_command = std::move(off_cmd);
        model_.save();
    });
    dlg->signal_hide().connect([dlg] { delete dlg; });
    dlg->present();
}

void Window::on_add_server()
{
    auto* dlg = new ServerDialog(*this, nullptr);
    dlg->signal_saved.connect([this](Server s, std::string pw, bool changed) {
        std::string err;
        if(!secrets_.set_password(s.id, pw, &err))
        {
            show_error("Could not store password: " + err);
            return;
        }
        // Replace or append.
        if(Server* existing = model_.find_server(s.id))
        {
            *existing = s;
        } else
        {
            model_.servers.push_back(std::move(s));
        }
        (void) changed;
        model_.save();
        rebuild_servers();
        rebuild_tunnels();
    });
    dlg->signal_hide().connect([dlg] { delete dlg; });
    dlg->present();
}

void Window::on_edit_server(const std::string& id)
{
    const Server* s = model_.find_server(id);
    if(!s)
    {
        return;
    }
    auto* dlg = new ServerDialog(*this, s);
    dlg->signal_saved.connect([this](Server s, std::string pw, bool changed) {
        if(changed)
        {
            std::string err;
            if(!secrets_.set_password(s.id, pw, &err))
            {
                show_error("Could not store password: " + err);
                return;
            }
        }
        if(Server* existing = model_.find_server(s.id))
        {
            *existing = s;
        }
        model_.save();
        rebuild_servers();
        rebuild_tunnels();
    });
    dlg->signal_hide().connect([dlg] { delete dlg; });
    dlg->present();
}

void Window::on_delete_server(const std::string& id)
{
    model_.remove_server(id);
    std::string err;
    secrets_.delete_password(id, &err); // best-effort
    model_.save();
    rebuild_servers();
    rebuild_tunnels();
}

void Window::on_check_servers()
{
    if(notebook_)
    {
        notebook_->set_current_page(servers_page_);
    }
    if(server_checker_.busy())
    {
        return;
    }
    if(check_btn_)
    {
        check_btn_->set_sensitive(false);
    }
    server_checker_.check_all();
}

void Window::on_check_server(const std::string& id)
{
    if(server_checker_.busy())
    {
        return;
    }
    server_checker_.check_one(id);
}

void Window::log_server_result(const std::string& id)
{
    const ServerCheckResult r = server_checker_.result(id);
    if(!r.checked)
    {
        return;
    }

    const Server*     s    = model_.find_server(id);
    const std::string name = s ? s->name : id;
    if(r.ok)
    {
        std::string msg = "Server " + name + ": OK";
        if(r.ping_ms >= 0.0)
        {
            msg += " (" + ping_text(r.ping_ms) + ")";
        }
        log_.append(msg);
    } else
    {
        log_.append("Server " + name + ": " + r.error);
    }
}

void Window::on_add_tunnel()
{
    if(model_.servers.empty())
    {
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

void Window::on_edit_tunnel(const std::string& id)
{
    const Tunnel* t = model_.find_tunnel(id);
    if(!t)
    {
        return;
    }
    auto* dlg = new TunnelDialog(*this, model_, t);
    dlg->signal_saved.connect([this](Tunnel t) {
        const std::string id = t.id;
        if(Tunnel* existing = model_.find_tunnel(id))
        {
            *existing = t;
        }
        model_.save();
        rebuild_tunnels();
    });
    dlg->signal_hide().connect([dlg] { delete dlg; });
    dlg->present();
}

void Window::on_delete_tunnel(const std::string& id)
{
    controller_.stop(id);
    if(singbox_.active_tunnel_id() == id)
    {
        singbox_.stop();
    }
    model_.remove_tunnel(id);
    model_.save();
    rebuild_tunnels();
}

void Window::on_toggle_tunnel(const std::string& id)
{
    if(controller_.is_running(id))
    {
        controller_.stop(id);
    } else
    {
        const std::string err = controller_.start(id);
        if(!err.empty())
        {
            show_error(err);
        }
    }
    refresh_tunnel_row(id);
}

void Window::on_route_tunnel(const std::string& id)
{
    if(singbox_.active_tunnel_id() == id)
    {
        singbox_.stop();
        return;
    }

    // Routing needs the tunnel's SOCKS port listening; auto-start it if needed.
    if(!controller_.is_running(id))
    {
        const std::string err = controller_.start(id);
        if(!err.empty())
        {
            show_error(err);
            return;
        }
    }

    const std::string err = singbox_.start(id);
    if(!err.empty())
    {
        show_error(err);
    }
}

void Window::show_error(const Glib::ustring& message)
{
    auto dlg = Gtk::AlertDialog::create(message);
    dlg->set_buttons({"OK"});
    dlg->show(*this);
}

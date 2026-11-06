#include "singbox_controller.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include <arpa/inet.h>
#include <glib.h>
#include <glibmm/main.h>
#include <netdb.h>
#include <sys/wait.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include "singbox_runner.hpp"
#include "ssh_runner.hpp"

using nlohmann::json;

namespace
{
    // Delay between sing-box starting and the route_on_command hook, giving the
    // TUN interface time to come up before the hook runs.
    constexpr int kActivateHookDelayMs = 1000;

    bool is_ip_literal(const std::string& host)
    {
        in_addr  a4;
        in6_addr a6;
        return inet_pton(AF_INET, host.c_str(), &a4) == 1 || inet_pton(AF_INET6, host.c_str(), &a6) == 1;
    }

    // Resolves `host` to its IPv4/IPv6 addresses as /32 and /128 CIDRs (used both
    // for the direct-outbound rules and the TUN route_exclude_address list).
    std::vector<std::string> resolve_host(const std::string& host)
    {
        std::vector<std::string> cidrs;
        addrinfo                 hints{};
        hints.ai_family   = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res     = nullptr;
        if(getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0)
        {
            return cidrs;
        }
        for(const addrinfo* p = res; p; p = p->ai_next)
        {
            char buf[INET6_ADDRSTRLEN];
            if(p->ai_family == AF_INET)
            {
                const auto* a = reinterpret_cast<const sockaddr_in*>(p->ai_addr);
                inet_ntop(AF_INET, &a->sin_addr, buf, sizeof buf);
                cidrs.emplace_back(std::string(buf) + "/32");
            } else if(p->ai_family == AF_INET6)
            {
                const auto* a = reinterpret_cast<const sockaddr_in6*>(p->ai_addr);
                inet_ntop(AF_INET6, &a->sin6_addr, buf, sizeof buf);
                cidrs.emplace_back(std::string(buf) + "/128");
            }
        }
        freeaddrinfo(res);
        return cidrs;
    }

    std::string last_nonempty_line(const std::string& text)
    {
        std::size_t start = 0;
        std::string last;
        while(start <= text.size())
        {
            const std::size_t nl   = text.find('\n', start);
            const std::size_t end  = (nl == std::string::npos) ? text.size() : nl;
            std::string       line = text.substr(start, end - start);
            while(!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r'))
            {
                line.pop_back();
            }
            if(!line.empty())
            {
                last = line;
            }
            if(nl == std::string::npos)
            {
                break;
            }
            start = nl + 1;
        }
        return last;
    }

    // Runs `sing-box check -c <path>`; true on success, else *err is filled.
    bool run_check(const std::string& path, std::string* err)
    {
        int         stderr_fd = -1;
        const pid_t pid       = singbox_runner::spawn({"sing-box", "check", "-c", path}, &stderr_fd);
        if(pid < 0)
        {
            *err = std::strerror(errno);
            if(errno == ENOENT)
            {
                *err = "sing-box not found in PATH";
            }
            return false;
        }

        std::string out;
        int         status = 0;
        for(;;)
        {
            ssh_runner::read_stderr(stderr_fd, out);
            const pid_t r = waitpid(pid, &status, 0);
            if(r == pid)
            {
                break;
            }
            if(r < 0 && errno != EINTR)
            {
                *err = "waitpid failed: " + std::string(std::strerror(errno));
                close(stderr_fd);
                return false;
            }
        }
        ssh_runner::read_stderr(stderr_fd, out);
        close(stderr_fd);

        if(WIFEXITED(status) && WEXITSTATUS(status) == 0)
        {
            return true;
        }
        *err = last_nonempty_line(out);
        if(err->empty())
        {
            *err = "sing-box check failed";
        }
        return false;
    }

} // namespace

SingboxController::SingboxController(Model& model, Log& log) :
    model_(model),
    log_(log)
{
    dispatcher_ = std::make_unique<Glib::Dispatcher>();
    dispatcher_->connect([this] {
        bool        unexpected = false;
        int         status     = 0;
        std::string stderr_text;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if(!exited_)
            {
                return;
            }
            exited_     = false;
            unexpected  = unexpected_;
            status      = exit_status_;
            stderr_text = exit_stderr_;
        }
        if(unexpected)
        {
            ++generation_; // cancel any pending on-activate hook
            std::string msg = "sing-box exited unexpectedly";
            if(WIFEXITED(status))
            {
                msg += " (status " + std::to_string(WEXITSTATUS(status)) + ")";
            } else if(WIFSIGNALED(status))
            {
                msg += " (signal " + std::to_string(WTERMSIG(status)) + ")";
            }
            const std::string detail = last_nonempty_line(stderr_text);
            if(!detail.empty())
            {
                msg += " — " + detail;
            }
            log_.append(msg);
            run_hook(model_.route_off_command, "stop");
        }
        active_tunnel_id_.clear();
        signal_changed.emit();
    });
}

SingboxController::~SingboxController()
{
    stop();
}

std::string SingboxController::config_path()
{
    return std::string(g_get_user_config_dir()) + "/ssh-vpn-manager/sing-box.json";
}

std::string SingboxController::generate_config(const Tunnel& t, std::string* err) const
{
    // Every server referenced by at least one tunnel gets routed direct (and its
    // IPs excluded from the TUN route) so the tunnels' own SSH connections never
    // loop back through the TUN.
    std::vector<std::string> hosts;
    for(const auto& tn : model_.tunnels)
    {
        const Server* s = model_.find_server(tn.server_id);
        if(!s || s->host.empty())
        {
            continue;
        }
        if(std::find(hosts.begin(), hosts.end(), s->host) != hosts.end())
        {
            continue;
        }
        hosts.push_back(s->host);
    }

    std::vector<std::string> domain_suffixes;
    std::vector<std::string> server_cidrs;
    for(const auto& host : hosts)
    {
        if(!is_ip_literal(host))
        {
            domain_suffixes.push_back(host);
        }
        std::vector<std::string> cidrs = resolve_host(host);
        for(auto& c : cidrs)
        {
            server_cidrs.push_back(std::move(c));
        }
    }

    json tun = {
        {"type",          "tun"                         },
        {"tag",           "tun-in"                      },
        {"address",       json::array({"172.19.0.1/30"})},
        {"auto_route",    true                          },
        {"strict_route",  true                          },
        {"auto_redirect", true                          },
        {"dns_mode",      "disabled"                    },
    };
    if(!server_cidrs.empty())
    {
        tun["route_exclude_address"] = server_cidrs;
    }

    json outbounds = json::array({
        {{"type", "socks"}, {"tag", "proxy"}, {"server", "127.0.0.1"}, {"server_port", t.local_port}, {"version", "5"}},
        {{"type", "direct"}, {"tag", "direct"}},
    });

    json rules = json::array();
    rules.push_back({
        {"action", "sniff"}
    });
    if(!domain_suffixes.empty())
    {
        rules.push_back({
            {"action",        "route"        },
            {"outbound",      "direct"       },
            {"domain_suffix", domain_suffixes}
        });
    }
    if(!server_cidrs.empty())
    {
        rules.push_back({
            {"action",   "route"     },
            {"outbound", "direct"    },
            {"ip_cidr",  server_cidrs}
        });
    }
    // Keep local/private traffic direct (LAN, loopback, link-local, IPv6 ULA).
    rules.push_back({
        {"action",   "route"                                                            },
        {"outbound", "direct"                                                           },
        {"ip_cidr",  json::array({"10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16", "127.0.0.0/8", "169.254.0.0/16",
                                 "::1/128", "fc00::/7", "fe80::/10"})}
    });

    json j;
    j["log"] = {
        {"level",     "info"},
        {"timestamp", true  }
    };
    j["inbounds"]  = json::array({tun});
    j["outbounds"] = outbounds;
    j["route"]     = {
        {"auto_detect_interface", true   },
        {"rules",                 rules  },
        {"final",                 "proxy"},
    };

    const std::filesystem::path p(config_path());
    std::filesystem::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::trunc);
    if(!f)
    {
        *err = "cannot open sing-box config for writing";
        return "";
    }
    f << j.dump(2) << "\n";
    if(!f)
    {
        *err = "failed to write sing-box config";
        return "";
    }
    return p.string();
}

std::string SingboxController::start(const std::string& tunnel_id)
{
    const Tunnel* t = model_.find_tunnel(tunnel_id);
    if(!t)
    {
        return "tunnel not found";
    }

    // Validate (generate + check) before stopping the current instance, so a
    // bad config or missing binary leaves the existing route untouched.
    std::string       err;
    const std::string path = generate_config(*t, &err);
    if(path.empty())
    {
        log_.append("sing-box: " + err);
        return err;
    }

    if(!run_check(path, &err))
    {
        log_.append("sing-box config check failed: " + err);
        return "config check failed: " + err;
    }

    stop();

    int         stderr_fd = -1;
    const pid_t pid       = singbox_runner::spawn({"sing-box", "run", "-c", path}, &stderr_fd);
    if(pid < 0)
    {
        err = std::strerror(errno);
        if(errno == ENOENT)
        {
            err = "sing-box not found in PATH";
        }
        log_.append("sing-box: failed to start — " + err);
        return "failed to start sing-box: " + err;
    }

    stderr_fd_ = stderr_fd;
    pid_.store(pid);
    active_tunnel_id_ = tunnel_id;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        exited_      = false;
        unexpected_  = false;
        exit_status_ = 0;
        exit_stderr_.clear();
    }
    stop_requested_.store(false);
    watch_thread_ = std::thread(&SingboxController::watch_run, this);

    const int gen = ++generation_;
    Glib::signal_timeout().connect_once(
        [this, gen] {
            if(gen == generation_)
            {
                run_hook(model_.route_on_command, "start");
            }
        },
        kActivateHookDelayMs);

    log_.append("sing-box: routing all traffic through tunnel 127.0.0.1:" + std::to_string(t->local_port));
    signal_changed.emit();
    return "";
}

void SingboxController::stop()
{
    const pid_t pid         = pid_.load();
    const bool  was_running = pid > 0;

    stop_requested_.store(true);
    ++generation_; // cancel any pending on-activate hook

    if(pid > 0)
    {
        ssh_runner::terminate(pid);
    }
    pid_.store(-1);

    if(watch_thread_.joinable())
    {
        watch_thread_.join();
    }

    active_tunnel_id_.clear();
    if(was_running)
    {
        run_hook(model_.route_off_command, "stop");
    }
    signal_changed.emit();
}

void SingboxController::run_hook(const std::string& command, const char* what)
{
    if(command.empty())
    {
        return;
    }

    log_.append(std::string("sing-box: running ") + what + " hook");

    int         stderr_fd = -1;
    const pid_t pid       = singbox_runner::spawn({"/bin/sh", "-c", command}, &stderr_fd);
    if(pid < 0)
    {
        log_.append(std::string("sing-box: failed to run ") + what + " hook: " + std::strerror(errno));
        return;
    }

    std::string out;
    int         status = 0;
    for(;;)
    {
        ssh_runner::read_stderr(stderr_fd, out);
        const pid_t r = waitpid(pid, &status, 0);
        if(r == pid)
        {
            break;
        }
        if(r < 0 && errno != EINTR)
        {
            break;
        }
    }
    ssh_runner::read_stderr(stderr_fd, out);
    close(stderr_fd);

    if(!(WIFEXITED(status) && WEXITSTATUS(status) == 0))
    {
        std::string       msg    = std::string("sing-box: ") + what + " hook failed";
        const std::string detail = last_nonempty_line(out);
        if(!detail.empty())
        {
            msg += " — " + detail;
        }
        log_.append(msg);
    }
}

void SingboxController::watch_run()
{
    std::string stderr_acc;
    int         status = 0;
    bool        exited = false;
    while(!stop_requested_.load())
    {
        ssh_runner::read_stderr(stderr_fd_, stderr_acc);

        const pid_t pid = pid_.load();
        if(pid <= 0)
        {
            break;
        }
        const pid_t r = waitpid(pid, &status, WNOHANG);
        if(r == pid)
        {
            exited = true;
            pid_.store(-1);
            break;
        }
        usleep(100 * 1000);
    }

    ssh_runner::read_stderr(stderr_fd_, stderr_acc);
    close(stderr_fd_);
    stderr_fd_ = -1;

    {
        std::lock_guard<std::mutex> lk(mutex_);
        exited_      = true;
        unexpected_  = exited && !stop_requested_.load();
        exit_status_ = status;
        exit_stderr_ = std::move(stderr_acc);
    }
    dispatcher_->emit();
}

bool SingboxController::is_running() const
{
    return pid_.load() > 0;
}

std::string SingboxController::active_tunnel_id() const
{
    return active_tunnel_id_;
}

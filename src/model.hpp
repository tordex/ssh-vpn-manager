#pragma once

#include <cstdint>
#include <string>
#include <vector>

// An SSH server the user can forward through. The password is intentionally NOT
// stored here: it lives encrypted in the Secret Service keyring, keyed by `id`.
struct Server
{
    std::string id;
    std::string name;
    std::string host;
    int         port = 22;
    std::string login;
    std::string ping_host = "1.1.1.1";
    int         ping_port = 80;
};

// A local SOCKS5 dynamic-forward tunnel bound to 127.0.0.1:local_port and routed
// through the referenced server.
struct Tunnel
{
    std::string id;
    int         local_port = 1080;
    std::string server_id;
    std::string ping_host              = "1.1.1.1";
    int         ping_port              = 80;
    int         ping_interval_sec      = 10;
    int         connect_timeout_sec    = 10;
    int         max_reconnect_attempts = 5; // 0 = keep retrying forever
    int         server_alive_interval  = 0; // seconds; 0 = disabled
    int         server_alive_count_max = 3; // silent intervals before disconnect
    bool        run_on_startup         = false;
};

// Owns all persisted configuration (servers + tunnels). Serialized to
// ~/.config/ssh-vpn-manager/config.json. Passwords are never written here.
class Model
{
  public:
    std::vector<Server> servers;
    std::vector<Tunnel> tunnels;

    bool load(std::string* error = nullptr);
    bool save(std::string* error = nullptr) const;

    Server*       find_server(const std::string& id);
    const Server* find_server(const std::string& id) const;
    Tunnel*       find_tunnel(const std::string& id);
    const Tunnel* find_tunnel(const std::string& id) const;

    bool remove_server(const std::string& id); // also drops tunnels that use it
    bool remove_tunnel(const std::string& id);

    static std::string new_id();
    static std::string config_path();
};

#include "model.hpp"

#include <glib.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>

#include <nlohmann/json.hpp>

using nlohmann::json;

std::string Model::config_path() {
  return std::string(g_get_user_config_dir()) + "/ssh-vpn-manager/config.json";
}

std::string Model::new_id() {
  static std::mt19937_64 rng{std::random_device{}()};
  std::uniform_int_distribution<std::uint64_t> dist;
  const std::uint64_t a = dist(rng);
  const std::uint64_t b = dist(rng);
  char buf[40];
  std::snprintf(buf, sizeof buf, "%016llx%016llx",
                static_cast<unsigned long long>(a),
                static_cast<unsigned long long>(b));
  return buf;
}

bool Model::load(std::string* error) {
  servers.clear();
  tunnels.clear();

  std::ifstream f(config_path());
  if (!f) {
    return true;  // no config yet: an empty model is valid
  }

  try {
    json j = json::parse(f);
    if (j.contains("servers") && j["servers"].is_array()) {
      for (const auto& s : j["servers"]) {
        Server sv;
        sv.id = s.value("id", "");
        sv.name = s.value("name", "");
        sv.host = s.value("host", "");
        sv.port = s.value("port", 22);
        sv.login = s.value("login", "");
        sv.ping_host = s.value("ping_host", "1.1.1.1");
        sv.ping_port = s.value("ping_port", 80);
        if (!sv.id.empty()) {
          servers.push_back(std::move(sv));
        }
      }
    }
    if (j.contains("tunnels") && j["tunnels"].is_array()) {
      for (const auto& t : j["tunnels"]) {
        Tunnel tn;
        tn.id = t.value("id", "");
        tn.local_port = t.value("local_port", 1080);
        tn.server_id = t.value("server_id", "");
        tn.ping_host = t.value("ping_host", "1.1.1.1");
        tn.ping_port = t.value("ping_port", 80);
        tn.ping_interval_sec = t.value("ping_interval_sec", 10);
        tn.connect_timeout_sec = t.value("connect_timeout_sec", 10);
        tn.max_reconnect_attempts = t.value("max_reconnect_attempts", 5);
        tn.run_on_startup = t.value("run_on_startup", false);
        if (!tn.id.empty()) {
          tunnels.push_back(std::move(tn));
        }
      }
    }
  } catch (const std::exception& e) {
    if (error) {
      *error = e.what();
    }
    return false;
  }
  return true;
}

bool Model::save(std::string* error) const {
  json j;
  j["servers"] = json::array();
  for (const auto& s : servers) {
    j["servers"].push_back({{"id", s.id},
                            {"name", s.name},
                            {"host", s.host},
                            {"port", s.port},
                            {"login", s.login},
                            {"ping_host", s.ping_host},
                            {"ping_port", s.ping_port}});
  }
  j["tunnels"] = json::array();
  for (const auto& t : tunnels) {
    j["tunnels"].push_back({{"id", t.id},
                            {"local_port", t.local_port},
                            {"server_id", t.server_id},
                            {"ping_host", t.ping_host},
                            {"ping_port", t.ping_port},
                            {"ping_interval_sec", t.ping_interval_sec},
                            {"connect_timeout_sec", t.connect_timeout_sec},
                            {"max_reconnect_attempts", t.max_reconnect_attempts},
                            {"run_on_startup", t.run_on_startup}});
  }

  try {
    const std::filesystem::path p(config_path());
    std::filesystem::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::trunc);
    if (!f) {
      if (error) {
        *error = "cannot open config file for writing";
      }
      return false;
    }
    f << j.dump(2) << "\n";
    if (!f) {
      if (error) {
        *error = "failed to write config file";
      }
      return false;
    }
  } catch (const std::exception& e) {
    if (error) {
      *error = e.what();
    }
    return false;
  }
  return true;
}

Server* Model::find_server(const std::string& id) {
  for (auto& s : servers) {
    if (s.id == id) {
      return &s;
    }
  }
  return nullptr;
}

const Server* Model::find_server(const std::string& id) const {
  for (const auto& s : servers) {
    if (s.id == id) {
      return &s;
    }
  }
  return nullptr;
}

Tunnel* Model::find_tunnel(const std::string& id) {
  for (auto& t : tunnels) {
    if (t.id == id) {
      return &t;
    }
  }
  return nullptr;
}

const Tunnel* Model::find_tunnel(const std::string& id) const {
  for (const auto& t : tunnels) {
    if (t.id == id) {
      return &t;
    }
  }
  return nullptr;
}

bool Model::remove_server(const std::string& id) {
  bool removed = false;
  for (auto it = servers.begin(); it != servers.end(); ++it) {
    if (it->id == id) {
      servers.erase(it);
      removed = true;
      break;
    }
  }
  tunnels.erase(std::remove_if(tunnels.begin(), tunnels.end(),
                               [&](const Tunnel& t) { return t.server_id == id; }),
                tunnels.end());
  return removed;
}

bool Model::remove_tunnel(const std::string& id) {
  for (auto it = tunnels.begin(); it != tunnels.end(); ++it) {
    if (it->id == id) {
      tunnels.erase(it);
      return true;
    }
  }
  return false;
}

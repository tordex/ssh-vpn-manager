#include "log.hpp"

#include <glib.h>
#include <glibmm/datetime.h>

#include <filesystem>
#include <system_error>

Log::Log(std::string file_path) : file_path_(std::move(file_path)) {
  open_file();
}

Log::~Log() {
  if (file_.is_open()) {
    file_.close();
  }
}

std::string Log::default_file_path() {
  return std::string(g_get_user_state_dir()) + "/ssh-vpn-manager/app.log";
}

void Log::open_file() {
  const std::filesystem::path p(file_path_);
  if (!p.parent_path().empty()) {
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
  }
  file_.open(file_path_, std::ios::app);
  if (!file_.is_open()) {
    g_warning("cannot open log file for writing: %s", file_path_.c_str());
  }
}

void Log::append(const std::string& message) {
  const std::string ts =
      Glib::DateTime::create_now_local().format("%H:%M:%S");
  const std::string line = ts + "  " + message;

  entries_.push_back(line);
  while (entries_.size() > max_entries_) {
    entries_.pop_front();
  }

  write_to_file(line);
  signal_changed.emit();
}

void Log::clear() {
  entries_.clear();
  signal_changed.emit();
}

void Log::write_to_file(const std::string& line) {
  if (!file_.is_open()) {
    return;
  }
  file_ << line << "\n";
  file_.flush();
}

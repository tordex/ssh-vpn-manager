#pragma once

#include <sigc++/sigc++.h>

#include <deque>
#include <fstream>
#include <string>

// Append-only application log: a timestamped in-memory ring buffer plus an
// optional on-disk log file. All methods must be called from the GTK main
// thread (worker-thread events arrive via Glib::Dispatcher and are replayed
// here).
class Log
{
  public:
    explicit Log(std::string file_path);
    ~Log();

    // Appends a timestamped line and emits `signal_changed`.
    void append(const std::string& message);

    // Clears the in-memory view only; the on-disk file keeps the full history.
    void clear();

    const std::deque<std::string>& entries() const
    {
        return entries_;
    }

    sigc::signal<void()> signal_changed;

    static std::string default_file_path();

  private:
    void open_file();
    void write_to_file(const std::string& line);

    std::deque<std::string> entries_;
    std::size_t             max_entries_ = 2000;
    std::string             file_path_;
    std::ofstream           file_;
};

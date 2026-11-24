#pragma once

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

enum class LogLevel { Info, Warning, Error };

// One line of the in-app log (the Log tab, LogView).
struct LogEntry {
  LogLevel    level  {LogLevel::Info};
  std::string source;          // "" for the app's own messages, "naja" for the loader's
  std::string text;            // one line: a multi-line message is one entry per line
  std::time_t time   {0};      // wall clock, seconds
};

// The app's log. Every message is echoed where it always went (stdout/
// stderr natively, the browser console in WASM) and kept, the last
// kMaxEntries lines, for the Log tab. Thread-safe: a design loads on a
// worker thread (DesignLoader), and naja logs from it.
class Console {
  public:
    static void Log(const std::string& msg);
    static void Warn(const std::string& msg);
    static void Error(const std::string& msg);
    // Keep a message without echoing it: naja's own messages, which its
    // logger already prints.
    static void record(LogLevel level, std::string_view source, std::string_view msg);
    // Native: false drops Log() messages (Warn()/Error() still go to
    // stderr) and keeps nothing below a warning. The headless renderer
    // turns them off: they echo every message.
    static void setVerbose(bool verbose);

    static constexpr size_t kMaxEntries = 5000;
    // A copy of the kept lines, oldest first.
    static std::vector<LogEntry> entries();
    // Bumped by every kept line and by clear(): a view copies entries()
    // only when it changed.
    static uint64_t revision();
    // Error messages (not lines) since the last clear(), also those
    // dropped past kMaxEntries: the Log tab's badge.
    static size_t errorCount();
    static void clear();
};

#include "Console.h"

#include <deque>
#include <mutex>

#ifdef __EMSCRIPTEN__
#include <emscripten/val.h>
#else
#include <iostream>
#endif

namespace {

struct Buffer {
  std::mutex mutex;
  std::deque<LogEntry> entries;
  uint64_t revision {0};
  size_t errors {0};
  bool verbose {true};
};

Buffer& buffer() {
  static Buffer b;
  return b;
}

void keep(LogLevel level, std::string_view source, std::string_view msg) {
  Buffer& b = buffer();
  const std::time_t now = std::time(nullptr);
  std::lock_guard<std::mutex> lock(b.mutex);
  if (!b.verbose && level == LogLevel::Info) return;
  // One entry per line, so the view's rows are all one line high. A
  // trailing newline makes no empty line.
  size_t start = 0;
  for (;;) {
    const size_t end = msg.find('\n', start);
    std::string_view line = msg.substr(start, end == std::string_view::npos ? end : end - start);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    b.entries.push_back({level, std::string(source), std::string(line), now});
    if (b.entries.size() > Console::kMaxEntries) b.entries.pop_front();
    if (end == std::string_view::npos || end + 1 == msg.size()) break;
    start = end + 1;
  }
  // Counted once per message: the badge counts errors, not their lines.
  if (level == LogLevel::Error) ++b.errors;
  ++b.revision;
}

bool verbose() {
  Buffer& b = buffer();
  std::lock_guard<std::mutex> lock(b.mutex);
  return b.verbose;
}

}  // namespace

void Console::record(LogLevel level, std::string_view source, std::string_view msg) {
  keep(level, source, msg);
}

#ifdef __EMSCRIPTEN__

void Console::Log(const std::string& msg) {
  keep(LogLevel::Info, "", msg);
  emscripten::val::global("console").call<void>("log", msg);
}

void Console::Warn(const std::string& msg) {
  keep(LogLevel::Warning, "", msg);
  emscripten::val::global("console").call<void>("warn", msg);
}

void Console::Error(const std::string& msg) {
  keep(LogLevel::Error, "", msg);
  emscripten::val::global("console").call<void>("error", msg);
}

#else

void Console::Log(const std::string& msg) {
  if (!verbose()) return;
  keep(LogLevel::Info, "", msg);
  std::cout << "[LOG] " << msg << "\n";
}

void Console::Warn(const std::string& msg) {
  keep(LogLevel::Warning, "", msg);
  std::cerr << "[WRN] " << msg << "\n";
}

void Console::Error(const std::string& msg) {
  keep(LogLevel::Error, "", msg);
  std::cerr << "[ERR] " << msg << "\n";
}

#endif

void Console::setVerbose(bool v) {
  Buffer& b = buffer();
  std::lock_guard<std::mutex> lock(b.mutex);
  b.verbose = v;
}

std::vector<LogEntry> Console::entries() {
  Buffer& b = buffer();
  std::lock_guard<std::mutex> lock(b.mutex);
  return {b.entries.begin(), b.entries.end()};
}

uint64_t Console::revision() {
  Buffer& b = buffer();
  std::lock_guard<std::mutex> lock(b.mutex);
  return b.revision;
}

size_t Console::errorCount() {
  Buffer& b = buffer();
  std::lock_guard<std::mutex> lock(b.mutex);
  return b.errors;
}

void Console::clear() {
  Buffer& b = buffer();
  std::lock_guard<std::mutex> lock(b.mutex);
  b.entries.clear();
  b.errors = 0;
  ++b.revision;
}

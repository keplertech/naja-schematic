#pragma once

#include <cstddef>
#include <string>

struct LogEntry;

// The "Log" bottom-panel tab: Console's kept lines (the app's messages and
// naja's: load errors, warnings), filtered by level, newest at the bottom.
class LogView {
  public:
    static void render();
    // Errors logged since the tab was last shown: its badge.
    static size_t unseenErrors();
    // How a line reads in the tab and in Copy: "12:03:41 [naja] text".
    static std::string format(const LogEntry& e);
};

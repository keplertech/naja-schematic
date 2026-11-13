#pragma once

#include <string>

class Console {
  public:
    static void Log(const std::string& msg);
    static void Error(const std::string& msg);
    // Native: false drops Log() messages (Error() still goes to stderr).
    // The headless renderer turns them off: they echo every message.
    static void setVerbose(bool verbose);
};

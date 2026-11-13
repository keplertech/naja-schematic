#include "Console.h"

#ifdef __EMSCRIPTEN__
#include <emscripten/val.h>

void Console::Log(const std::string& msg) {
  emscripten::val::global("console").call<void>("log", msg);
}

void Console::Error(const std::string& msg) {
  emscripten::val::global("console").call<void>("error", msg);
}

void Console::setVerbose(bool) {}

#else
#include <iostream>

static bool g_verbose = true;

void Console::setVerbose(bool verbose) { g_verbose = verbose; }

void Console::Log(const std::string& msg) {
  if (g_verbose) std::cout << "[LOG] " << msg << "\n";
}

void Console::Error(const std::string& msg) {
  std::cerr << "[ERR] " << msg << "\n";
}

#endif

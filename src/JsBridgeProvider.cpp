#ifdef __EMSCRIPTEN__

#include "JsBridgeProvider.h"

#include <emscripten.h>
#include <emscripten/bind.h>

#include "Console.h"

EM_JS(int, naja_bridge_available, (), {
  return typeof Module['najaSend'] === 'function' ? 1 : 0;
});

EM_JS(void, naja_bridge_send, (const char* msg), {
  Module['najaSend'](UTF8ToString(msg));
});

// One viewer per module instance (the host instantiates the module once per
// view), so a single current provider is enough.
static JsBridgeProvider* g_bridge = nullptr;

static void deliverMessage(const std::string& msg) {
  if (g_bridge) {
    g_bridge->deliver(msg);
  } else {
    Console::Error("deliverMessage called before the bridge provider exists");
  }
}

EMSCRIPTEN_BINDINGS(naja_bridge) {
  emscripten::function("deliverMessage", &deliverMessage);
}

JsBridgeProvider::JsBridgeProvider() { g_bridge = this; }

JsBridgeProvider::~JsBridgeProvider() {
  if (g_bridge == this) g_bridge = nullptr;
}

bool JsBridgeProvider::available() { return naja_bridge_available() != 0; }

void JsBridgeProvider::send(const std::string& msg) {
  naja_bridge_send(msg.c_str());
}

void JsBridgeProvider::on_open(std::function<void()> callback) {
  open_cb_ = std::move(callback);
}

void JsBridgeProvider::on_message(std::function<void(const std::string&)> callback) {
  msg_cb_ = std::move(callback);
}

// The host channel's lifetime is the page's: there's no close/error to report.
void JsBridgeProvider::on_close(std::function<void()>) {}
void JsBridgeProvider::on_error(std::function<void(const std::string&)>) {}

void JsBridgeProvider::start() {
  if (open_cb_) open_cb_();
}

void JsBridgeProvider::deliver(const std::string& msg) {
  if (msg_cb_) msg_cb_(msg);
}

#endif // __EMSCRIPTEN__

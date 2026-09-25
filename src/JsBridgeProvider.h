#pragma once
#ifdef __EMSCRIPTEN__

#include "INetlistProvider.h"
#include <string>

// INetlistProvider implementation for embedded mode (Jupyter/Colab widget,
// editor webview): the page hosting the WASM module owns the transport.
// Requests go out through the host-supplied Module.najaSend(json) hook;
// replies and pushes come back through the embind-exported
// Module.deliverMessage(json). Used by main_wasm.cpp whenever the host sets
// Module.najaSend, instead of WebSocketProvider.
class JsBridgeProvider : public INetlistProvider {
  public:
    JsBridgeProvider();
    ~JsBridgeProvider() override;

    // True when the host page provided Module.najaSend.
    static bool available();

    void send(const std::string& msg) override;
    void on_open(std::function<void()> callback) override;
    void on_message(std::function<void(const std::string&)> callback) override;
    void on_close(std::function<void()> callback) override;
    void on_error(std::function<void(const std::string&)> callback) override;
    // The host channel is already up: fires on_open synchronously.
    void start() override;

    void deliver(const std::string& msg);

  private:
    std::function<void()> open_cb_;
    std::function<void(const std::string&)> msg_cb_;
};

#endif // __EMSCRIPTEN__

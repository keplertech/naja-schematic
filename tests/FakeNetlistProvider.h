#pragma once

#include <vector>

#include "INetlistProvider.h"

// Minimal INetlistProvider stand-in for tests that need a NetlistTree but
// never talk to a real backend. Records every send() so tests can assert
// on outgoing requests (e.g. load_equipotential) without a websocket or SNL.
class FakeNetlistProvider : public INetlistProvider {
  public:
    void send(const std::string& msg) override { sent.push_back(msg); }
    void on_open(std::function<void()>) override {}
    void on_message(std::function<void(const std::string&)> cb) override { msg_cb = std::move(cb); }
    void on_close(std::function<void()>) override {}
    void on_error(std::function<void(const std::string&)>) override {}

    // Plays a message from the backend to whoever registered on_message.
    void deliver(const std::string& msg) { if (msg_cb) msg_cb(msg); }

    std::vector<std::string> sent;
    std::function<void(const std::string&)> msg_cb;
};

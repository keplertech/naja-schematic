#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "INetlistProvider.h"

// Keeps the viewer on one design generation of a host that can replace its
// design (the Python ViewerServer/widget), by wrapping the real transport.
//
// The host numbers its designs: every reply and push carries the
// "generation" it was produced for, and design_changed announces the next
// one. This wrapper
// - adopts the generation of the first stamped message it receives;
// - on design_changed(N), newer than the current one, switches to N and
//   passes the push on (AppLogic then reloads); an older or repeated one is
//   dropped;
// - drops every other message stamped with another generation: a reply to a
//   request made for a replaced design names nodes and instance ids that
//   mean nothing -- or, when ids are reused, something else -- in the new one;
// - stamps every outgoing request with the current generation, so the host
//   can in turn drop requests (selections, property queries, ...) made for a
//   design it has already replaced.
// Messages without "generation" (a host predating it, or LocalSNLProvider,
// which never replaces its design under the viewer) pass through untouched,
// and requests go out unstamped until a generation is known.
class GenerationProvider : public INetlistProvider {
  public:
    explicit GenerationProvider(INetlistProvider* inner);  // takes ownership

    void send(const std::string& msg) override;
    void on_open(std::function<void()> callback) override;
    void on_message(std::function<void(const std::string&)> callback) override;
    void on_close(std::function<void()> callback) override;
    void on_error(std::function<void(const std::string&)> callback) override;
    void start() override;

    std::optional<uint64_t> generation() const { return generation_; }

    // Whether an incoming message is for the current generation (updating
    // it on design_changed or on the first stamped message).
    bool accept(const std::string& msg);

  private:
    std::unique_ptr<INetlistProvider> inner_;
    std::optional<uint64_t>           generation_;
};

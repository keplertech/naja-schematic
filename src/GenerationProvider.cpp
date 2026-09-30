#include "GenerationProvider.h"

#include <nlohmann/json.hpp>

#include "Console.h"

using json = nlohmann::json;

namespace {

std::optional<uint64_t> readGeneration(const json& j) {
  if (!j.is_object()) return std::nullopt;
  auto it = j.find("generation");
  if (it == j.end() || !it->is_number_unsigned()) return std::nullopt;
  return it->get<uint64_t>();
}

}  // namespace

GenerationProvider::GenerationProvider(INetlistProvider* inner) : inner_(inner) {}

void GenerationProvider::send(const std::string& msg) {
  if (!generation_) {
    inner_->send(msg);
    return;
  }
  json j = json::parse(msg, nullptr, /*allow_exceptions=*/false);
  if (!j.is_object()) {
    inner_->send(msg);
    return;
  }
  j["generation"] = *generation_;
  inner_->send(j.dump());
}

bool GenerationProvider::accept(const std::string& msg) {
  // Some transports hand over a NUL-terminated buffer (see AppLogic).
  json j = json::parse(msg.substr(0, msg.find('\0')), nullptr, /*allow_exceptions=*/false);
  auto stamped = readGeneration(j);
  if (!stamped) return true;  // unstamped: a host without generations
  const bool designChanged = j.value("response", "") == "design_changed";
  if (designChanged) {
    if (generation_ && *stamped <= *generation_) {
      Console::Log("Dropping stale design_changed for generation " + std::to_string(*stamped));
      return false;
    }
    generation_ = *stamped;
    return true;
  }
  if (!generation_) generation_ = *stamped;
  if (*stamped == *generation_) return true;
  Console::Log("Dropping " + j.value("response", std::string("message")) +
               " for design generation " + std::to_string(*stamped) +
               " (showing " + std::to_string(*generation_) + ")");
  return false;
}

void GenerationProvider::on_open(std::function<void()> callback) {
  inner_->on_open(std::move(callback));
}

void GenerationProvider::on_message(std::function<void(const std::string&)> callback) {
  inner_->on_message([this, callback = std::move(callback)](const std::string& msg) {
    if (accept(msg)) callback(msg);
  });
}

void GenerationProvider::on_close(std::function<void()> callback) {
  inner_->on_close(std::move(callback));
}

void GenerationProvider::on_error(std::function<void(const std::string&)> callback) {
  inner_->on_error(std::move(callback));
}

void GenerationProvider::start() { inner_->start(); }

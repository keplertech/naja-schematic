#ifndef __EMSCRIPTEN__

#include "DesignLoader.h"

#include "Console.h"
#include "LocalSNLProvider.h"

DesignLoader::~DesignLoader() {
  // Quitting during a load: stop it rather than wait for it.
  if (thread_.joinable()) {
    cancelled_ = true;
    thread_.join();
  }
}

bool DesignLoader::start(LocalSNLProvider& provider, std::string title,
                         std::function<bool(LocalSNLProvider&)> load) {
  if (thread_.joinable()) return false;
  provider_ = &provider;
  title_ = std::move(title);
  done_ = false;
  ok_ = false;
  cancelled_ = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    progress_ = {};
  }
  started_ = std::chrono::steady_clock::now();

  provider.takeDroppedRequests();
  provider.setLoading(true);
  provider.setProgressCallback([this](const naja::NajaProgress& p) {
    std::lock_guard<std::mutex> lock(mutex_);
    progress_.stage.assign(p.stage);
    progress_.detail.assign(p.detail);
    progress_.current = p.current;
    progress_.total   = p.total;
    return !cancelled_.load();
  });
  thread_ = std::thread([this, load = std::move(load)] {
    bool ok = false;
    try {
      ok = load(*provider_);
    } catch (const std::exception& e) {
      // The provider's loaders catch their own errors: not one of them.
      Console::Error(title_ + " failed: " + e.what());
    } catch (...) {
      Console::Error(title_ + " failed");
    }
    ok_ = ok;
    done_ = true;
  });
  return true;
}

bool DesignLoader::takeResult(bool& ok) {
  if (!thread_.joinable() || !done_) return false;
  thread_.join();
  provider_->setProgressCallback({});
  provider_->setLoading(false);
  ok = ok_;
  return true;
}

DesignLoader::Progress DesignLoader::progress() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return progress_;
}

double DesignLoader::elapsedSeconds() const {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
}

std::string DesignLoader::stageLabel(const std::string& stage) {
  if (stage.empty())          return "Starting";
  if (stage == "parse")       return "Parsing";
  if (stage == "first_pass")  return "Reading modules";
  if (stage == "second_pass") return "Building the netlist";
  if (stage == "elaborate")   return "Elaborating";
  if (stage == "construct")   return "Building the netlist";
  if (stage == "liberty")     return "Reading Liberty";
  if (stage == "done")        return "Finishing";
  return stage;
}

#endif // __EMSCRIPTEN__

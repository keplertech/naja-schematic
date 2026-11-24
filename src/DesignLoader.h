#pragma once
#ifndef __EMSCRIPTEN__

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

class LocalSNLProvider;

// Runs one design load (a LocalSNLProvider loader) on a worker thread, so
// the window keeps drawing -- the "Loading design" dialog in appFrame() --
// and records naja's progress for it. While it runs, the provider drops
// requests (LocalSNLProvider::setLoading()); nothing else may touch naja.
class DesignLoader {
  public:
    // What naja last reported. `total` 0: the amount of work isn't known.
    struct Progress {
      std::string stage;     // naja's stage ("parse", "elaborate", ...)
      std::string detail;    // e.g. the file or design being read
      uint64_t    current {0};
      uint64_t    total   {0};
    };

    ~DesignLoader();

    // Start `load` (calls one of the provider's loaders, returns its
    // result) on a worker thread. False, doing nothing, if one is running.
    bool start(LocalSNLProvider& provider, std::string title,
               std::function<bool(LocalSNLProvider&)> load);
    // A load was started and not yet taken.
    bool active() const { return thread_.joinable(); }
    // Once the load is over: joins it, hands the provider back, and gives
    // whether it loaded. False (`ok` untouched) while it still runs.
    bool takeResult(bool& ok);

    // Ask naja to stop at its next progress report: the load then fails,
    // keeping the design loaded before.
    void cancel() { cancelled_ = true; }
    bool cancelling() const { return cancelled_; }

    const std::string& title() const { return title_; }
    Progress progress() const;
    double elapsedSeconds() const;

    // How a stage reads in the dialog ("parse" -> "Parsing").
    static std::string stageLabel(const std::string& stage);

  private:
    LocalSNLProvider*  provider_ {nullptr};
    std::thread        thread_;
    std::atomic<bool>  done_ {false};
    std::atomic<bool>  ok_ {false};
    std::atomic<bool>  cancelled_ {false};
    std::string        title_;
    std::chrono::steady_clock::time_point started_;
    mutable std::mutex mutex_;
    Progress           progress_;
};

#endif // __EMSCRIPTEN__

// Loading a design on a worker thread (DesignLoader) through the
// provider's loaders: naja's progress reaches the loader, Cancel stops the
// load, and a load that fails or is cancelled keeps the design shown.

#include "DesignLoader.h"
#include "LocalSNLProvider.h"
#include "Console.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>
#include <gtest/gtest.h>

#include "NLUniverse.h"

using json = nlohmann::json;
using naja::NL::NLUniverse;

namespace {

std::string testData(const std::string& rel) {
  return std::string(NAJA_SCHEMATIC_TEST_DATA) + "/" + rel;
}

struct Provider {
  LocalSNLProvider provider;
  json             replies = json::array();

  Provider() {
    if (!NLUniverse::get()) NLUniverse::create();
    provider.on_message([this](const std::string& msg) { replies.push_back(json::parse(msg)); });
  }

  // The top's name, or "" if no reply.
  std::string topName() {
    replies = json::array();
    provider.send(R"({"request":"load_root"})");
    if (replies.empty()) return "";
    return replies[0]["root"].value("name", "");
  }
};

// Waits for the load to be over (it's on another thread) and takes it.
bool finish(DesignLoader& loader) {
  bool ok = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  while (!loader.takeResult(ok)) {
    if (std::chrono::steady_clock::now() > deadline) ADD_FAILURE() << "load never finished";
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return ok;
}

}  // namespace

TEST(DesignLoader, LoadsOnAWorkerThreadAndReportsNajaStages) {
  Provider p;
  DesignLoader loader;
  std::atomic<bool> onAnotherThread {false};
  const auto ui = std::this_thread::get_id();
  ASSERT_TRUE(loader.start(p.provider, "Loading SystemVerilog", [&](LocalSNLProvider& provider) {
    onAnotherThread = std::this_thread::get_id() != ui;
    return provider.loadSystemVerilog({testData("sv/gates.sv")});
  }));
  EXPECT_TRUE(loader.active());
  EXPECT_FALSE(loader.start(p.provider, "again", [](LocalSNLProvider&) { return true; }));
  ASSERT_TRUE(finish(loader));
  EXPECT_FALSE(loader.active());
  EXPECT_TRUE(onAnotherThread);
  EXPECT_TRUE(p.provider.hasDesign());
  EXPECT_FALSE(p.provider.loading());
  // The last report is naja's: the load got to the end.
  EXPECT_EQ(loader.progress().stage, "done");
  EXPECT_FALSE(p.topName().empty());
}

TEST(DesignLoader, RequestsAreDroppedWhileLoading) {
  Provider p;
  DesignLoader loader;
  std::atomic<bool> go {false};
  ASSERT_TRUE(loader.start(p.provider, "Loading VHDL", [&](LocalSNLProvider& provider) {
    while (!go) std::this_thread::yield();
    return provider.loadVHDL({testData("vhdl/top.vhd"), testData("vhdl/leaf.vhd")});
  }));
  EXPECT_TRUE(p.provider.loading());
  EXPECT_EQ(p.topName(), "");   // unanswered: the DB is being replaced
  go = true;
  ASSERT_TRUE(finish(loader));
  EXPECT_EQ(p.provider.takeDroppedRequests(), 1u);
  EXPECT_EQ(p.provider.takeDroppedRequests(), 0u);
  EXPECT_EQ(p.topName(), "top");
}

TEST(DesignLoader, CancelKeepsTheDesignLoadedBefore) {
  Provider p;
  ASSERT_TRUE(p.provider.loadVHDL({testData("vhdl/top.vhd"), testData("vhdl/leaf.vhd")}));
  DesignLoader loader;
  std::atomic<bool> go {false};
  ASSERT_TRUE(loader.start(p.provider, "Loading VHDL", [&](LocalSNLProvider& provider) {
    while (!go) std::this_thread::yield();
    return provider.loadVHDL({testData("vhdl/top.vhd"), testData("vhdl/leaf.vhd")}, "leaf");
  }));
  loader.cancel();   // before naja's first report, which then stops it
  EXPECT_TRUE(loader.cancelling());
  go = true;
  EXPECT_FALSE(finish(loader));
  EXPECT_EQ(p.topName(), "top");   // not "leaf": the earlier design is kept
}

TEST(DesignLoader, AFailedLoadKeepsTheDesignAndLogsWhy) {
  Provider p;
  ASSERT_TRUE(p.provider.loadVHDL({testData("vhdl/top.vhd"), testData("vhdl/leaf.vhd")}));
  Console::clear();
  DesignLoader loader;
  ASSERT_TRUE(loader.start(p.provider, "Loading VHDL", [&](LocalSNLProvider& provider) {
    return provider.loadVHDL({testData("vhdl/missing.vhd")});
  }));
  EXPECT_FALSE(finish(loader));
  EXPECT_TRUE(p.provider.hasDesign());
  EXPECT_EQ(p.topName(), "top");
  EXPECT_GE(Console::errorCount(), 1u);
}

TEST(DesignLoader, StageLabels) {
  EXPECT_EQ(DesignLoader::stageLabel("parse"), "Parsing");
  EXPECT_EQ(DesignLoader::stageLabel("elaborate"), "Elaborating");
  EXPECT_EQ(DesignLoader::stageLabel(""), "Starting");
  EXPECT_EQ(DesignLoader::stageLabel("something_new"), "something_new");
}

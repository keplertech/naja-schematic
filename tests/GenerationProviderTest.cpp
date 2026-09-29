#include "GenerationProvider.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "FakeNetlistProvider.h"

using json = nlohmann::json;

namespace {

// A GenerationProvider over a FakeNetlistProvider, recording what gets
// through to the viewer.
class GenerationProviderTest : public ::testing::Test {
  protected:
    void SetUp() override {
      transport = new FakeNetlistProvider();  // owned by `provider`
      provider  = std::make_unique<GenerationProvider>(transport);
      provider->on_message([this](const std::string& m) { received.push_back(json::parse(m)); });
    }

    void fromHost(const json& j) { transport->deliver(j.dump()); }
    json lastSent() const { return json::parse(transport->sent.back()); }
    std::vector<std::string> receivedTypes() const {
      std::vector<std::string> types;
      for (const auto& j : received) types.push_back(j.value("response", ""));
      return types;
    }

    FakeNetlistProvider*                transport {nullptr};
    std::unique_ptr<GenerationProvider> provider;
    std::vector<json>                   received;
};

json reply(const std::string& type, uint64_t generation) {
  return {{"response", type}, {"generation", generation}};
}

}  // namespace

TEST_F(GenerationProviderTest, RequestsGoOutUnstampedUntilAGenerationIsKnown) {
  provider->send(R"({"request":"load_root"})");
  EXPECT_FALSE(lastSent().contains("generation"));
  fromHost(reply("root_response", 4));
  EXPECT_EQ(provider->generation(), 4u);
  provider->send(R"({"request":"load_terms","gui_id":2})");
  EXPECT_EQ(lastSent()["generation"], 4);
  EXPECT_EQ(lastSent()["gui_id"], 2);
}

TEST_F(GenerationProviderTest, UnstampedMessagesPassThrough) {
  // A host without generations (or LocalSNLProvider): nothing is filtered
  // or stamped.
  fromHost({{"response", "root_response"}});
  fromHost({{"response", "terms_response"}});
  EXPECT_EQ(receivedTypes(), (std::vector<std::string>{"root_response", "terms_response"}));
  provider->send(R"({"request":"load_root"})");
  EXPECT_FALSE(lastSent().contains("generation"));
}

// Replies to requests made for the old design reach the viewer after the
// design_changed push: they must not touch the reloaded tree.
TEST_F(GenerationProviderTest, DelayedRepliesForTheOldDesignAreDropped) {
  fromHost(reply("root_response", 0));
  fromHost(reply("design_changed", 1));
  fromHost(reply("terms_response", 0));
  fromHost(reply("properties_response", 0));
  fromHost(reply("instance_resolved", 0));
  fromHost(reply("root_response", 1));
  EXPECT_EQ(receivedTypes(),
            (std::vector<std::string>{"root_response", "design_changed", "root_response"}));
}

// AppLogic answers design_changed with load_root: it must already carry the
// new generation, as must a selection made right after.
TEST_F(GenerationProviderTest, RequestsAfterDesignChangedCarryTheNewGeneration) {
  provider->on_message([this](const std::string& m) {
    if (json::parse(m).value("response", "") == "design_changed")
      provider->send(R"({"request":"load_root"})");
  });
  fromHost(reply("root_response", 0));
  fromHost(reply("design_changed", 1));
  EXPECT_EQ(lastSent()["request"], "load_root");
  EXPECT_EQ(lastSent()["generation"], 1);
  provider->send(R"({"request":"instance_selected","id_path":[0]})");
  EXPECT_EQ(lastSent()["generation"], 1);
}

TEST_F(GenerationProviderTest, StaleOrRepeatedDesignChangedIsDropped) {
  fromHost(reply("root_response", 2));
  fromHost(reply("design_changed", 2));
  fromHost(reply("design_changed", 1));
  EXPECT_EQ(receivedTypes(), (std::vector<std::string>{"root_response"}));
  EXPECT_EQ(provider->generation(), 2u);
}

// Two replacements in a row: only the last design's messages get through,
// including pushes the host sent for the one in between.
TEST_F(GenerationProviderTest, OverlappingReplacementsKeepOnlyTheLast) {
  fromHost(reply("root_response", 0));
  fromHost(reply("design_changed", 1));
  fromHost(reply("design_changed", 2));
  fromHost(reply("root_response", 1));
  fromHost(reply("diagnosis_response", 1));
  fromHost(reply("root_response", 2));
  fromHost(reply("diagnosis_response", 2));
  EXPECT_EQ(receivedTypes(),
            (std::vector<std::string>{"root_response", "design_changed", "design_changed",
                                      "root_response", "diagnosis_response"}));
  EXPECT_EQ(received[3]["generation"], 2);
}

TEST_F(GenerationProviderTest, AdoptsTheGenerationOfADesignChangedBeforeAnyRoot) {
  // The host replaced the design before answering the first load_root.
  fromHost(reply("design_changed", 3));
  EXPECT_EQ(provider->generation(), 3u);
  fromHost(reply("root_response", 3));
  EXPECT_EQ(receivedTypes(), (std::vector<std::string>{"design_changed", "root_response"}));
}

TEST_F(GenerationProviderTest, ToleratesATrailingNul) {
  fromHost(reply("root_response", 0));
  transport->deliver(reply("terms_response", 7).dump() + std::string(1, '\0'));
  EXPECT_EQ(receivedTypes(), (std::vector<std::string>{"root_response"}));
}

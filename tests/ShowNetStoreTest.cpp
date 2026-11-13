#include "ShowNetStore.h"
#include "TraceStore.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace {

json push(json extra = json::object()) {
  json p = {{"response", "show_net"}, {"id_path", {0, 0}}, {"path", {"u_sub", "u_and"}},
            {"term_id", 2}, {"label", "u_sub/u_and/O (net)"}, {"add", true}};
  for (auto& [k, v] : extra.items()) p[k] = v;
  return p;
}

class ShowNetStoreTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ShowNetStore::forget();
    TraceStore::clear();
  }
  void TearDown() override {
    ShowNetStore::forget();
    TraceStore::clear();
  }
  int clears = 0;
  std::function<void()> clearView = [this] {
    ++clears;
    TraceStore::clear();  // as GUIData::clearEquipotentials() does
  };
};

}  // namespace

// Same request as the tree's Show Equipotential: ids, term id, the bit, and
// the trace registered under the host's label.
TEST_F(ShowNetStoreTest, AsksForTheNetAsATaggedOneNetTrace) {
  auto req = ShowNetStore::request(push({{"bit", 1}}), clearView);
  ASSERT_TRUE(req);
  EXPECT_EQ((*req)["request"], "load_equipotential");
  EXPECT_EQ((*req)["path"], json({0, 0}));
  EXPECT_EQ((*req)["term_id"], 2);
  EXPECT_EQ((*req)["bit"], 1);
  const int id = (*req)["trace_id"];
  ASSERT_TRUE(TraceStore::find(id));
  EXPECT_EQ(TraceStore::find(id)->label, "u_sub/u_and/O (net)");
  EXPECT_FALSE(TraceStore::find(id)->customColor);
  EXPECT_EQ(clears, 0);
  EXPECT_EQ(ShowNetStore::takeReply(id), id);
}

TEST_F(ShowNetStoreTest, NoBitMeansAScalarPin) {
  auto req = ShowNetStore::request(push({{"bit", nullptr}}), clearView);
  ASSERT_TRUE(req);
  EXPECT_FALSE(req->contains("bit"));
}

TEST_F(ShowNetStoreTest, HostColorReplacesThePalettes) {
  auto req = ShowNetStore::request(push({{"color", "#ff8800"}}), clearView);
  ASSERT_TRUE(req);
  const Trace* t = TraceStore::find((*req)["trace_id"]);
  ASSERT_TRUE(t && t->customColor);
  EXPECT_EQ(*t->customColor, IM_COL32(0xff, 0x88, 0x00, 255));
}

// A color the Traces tab would refuse (near-black, near-white) or can't
// read leaves the palette color: the net is still shown.
TEST_F(ShowNetStoreTest, RefusedColorFallsBackToThePalette) {
  for (const char* color : {"#000000", "#ffffff", "red", "#12345"}) {
    auto req = ShowNetStore::request(push({{"color", color}, {"term_id", 3}}), clearView);
    ASSERT_TRUE(req) << color;
    EXPECT_FALSE(TraceStore::find((*req)["trace_id"])->customColor) << color;
    ShowNetStore::forget();
  }
}

TEST_F(ShowNetStoreTest, AddFalseClearsTheViewFirst) {
  ASSERT_TRUE(ShowNetStore::request(push(), clearView));
  auto req = ShowNetStore::request(push({{"term_id", 0}, {"add", false}}), clearView);
  ASSERT_TRUE(req);
  EXPECT_EQ(clears, 1);
  ASSERT_EQ(TraceStore::traces().size(), 1u);
  EXPECT_EQ(TraceStore::traces()[0].id, (*req)["trace_id"]);
}

// The host re-sends its pushes after each root load: a net already shown
// as a host net isn't stacked a second trace.
TEST_F(ShowNetStoreTest, NetAlreadyShownIsSkipped) {
  auto first = ShowNetStore::request(push(), clearView);
  ASSERT_TRUE(first);
  EXPECT_FALSE(ShowNetStore::request(push(), clearView));  // still in flight
  ShowNetStore::takeReply((*first)["trace_id"]);
  EXPECT_FALSE(ShowNetStore::request(push(), clearView));  // drawn
  EXPECT_TRUE(ShowNetStore::request(push({{"bit", 0}}), clearView));  // another pin
  EXPECT_EQ(TraceStore::traces().size(), 2u);

  // Once its trace is removed (Traces tab), it can be shown again.
  TraceStore::remove((*first)["trace_id"]);
  EXPECT_TRUE(ShowNetStore::request(push(), clearView));
}

// focus_instance, then show_net: the focus's instance_resolved clears the
// view (and its traces) before the net's reply arrives. The reply gets its
// trace back, same label and color.
TEST_F(ShowNetStoreTest, TraceClearedWhileInFlightIsRegisteredAgain) {
  auto req = ShowNetStore::request(push({{"color", "#ff8800"}}), clearView);
  ASSERT_TRUE(req);
  const int id = (*req)["trace_id"];
  TraceStore::clear();  // instance_resolved: clearEquipotentials()

  const int again = ShowNetStore::takeReply(id);
  ASSERT_GT(again, 0);
  EXPECT_NE(again, id);
  const Trace* t = TraceStore::find(again);
  ASSERT_TRUE(t);
  EXPECT_EQ(t->label, "u_sub/u_and/O (net)");
  EXPECT_EQ(*t->customColor, IM_COL32(0xff, 0x88, 0x00, 255));
  EXPECT_EQ(ShowNetStore::takeReply(id), id);  // only once
}

// A view clear while the net is in flight (a new focus, the user's Clear
// nets, a reload): its reply isn't drawn, and leaves no Traces row.
TEST_F(ShowNetStoreTest, ForgottenNetsReplyIsDropped) {
  auto req = ShowNetStore::request(push(), clearView);
  ASSERT_TRUE(req);
  const int id = (*req)["trace_id"];
  ShowNetStore::forget();
  EXPECT_FALSE(TraceStore::find(id));
  EXPECT_EQ(ShowNetStore::takeReply(id), -1);
  EXPECT_EQ(ShowNetStore::takeReply(id), id);  // dropped once only
  // And it no longer holds its pin back.
  EXPECT_TRUE(ShowNetStore::request(push(), clearView));
}

TEST_F(ShowNetStoreTest, OtherRepliesPassThrough) {
  EXPECT_EQ(ShowNetStore::takeReply(0), 0);
  const int tree = TraceStore::begin("u1/A (net)");
  EXPECT_EQ(ShowNetStore::takeReply(tree), tree);
}

TEST_F(ShowNetStoreTest, MalformedPushIsIgnored) {
  for (json bad : {push({{"id_path", "u_sub/u_and"}}), push({{"id_path", {0, -1}}}),
                   push({{"term_id", "O"}}), push({{"bit", "0"}}), push({{"bit", -1}})}) {
    EXPECT_FALSE(ShowNetStore::request(bad, clearView)) << bad.dump();
  }
  json noTerm = push();
  noTerm.erase("term_id");
  EXPECT_FALSE(ShowNetStore::request(noTerm, clearView));
  EXPECT_TRUE(TraceStore::traces().empty());
}

TEST_F(ShowNetStoreTest, DefaultLabel) {
  json p = push();
  p.erase("label");
  auto req = ShowNetStore::request(p, clearView);
  ASSERT_TRUE(req);
  EXPECT_EQ(TraceStore::find((*req)["trace_id"])->label, "Net");
}

TEST(ShowNetStoreColor, ParsesHashRrggbbOnly) {
  EXPECT_EQ(ShowNetStore::parseColor("#0A14fa"), IM_COL32(0x0a, 0x14, 0xfa, 255));
  for (const char* bad : {"", "0a14fa", "#0a14f", "#0a14fa0", "#0g14fa"})
    EXPECT_FALSE(ShowNetStore::parseColor(bad)) << bad;
}

// "trace": true asks for the pin's fan-in cone, tagged the same way; the
// same pin shown as a net and as a trace are two different host nets.
TEST_F(ShowNetStoreTest, TraceAsksForTheFanInCone) {
  auto net = ShowNetStore::request(push(), clearView);
  auto trace = ShowNetStore::request(push({{"trace", true}, {"label", "u_sub/u_and/O"}}), clearView);
  ASSERT_TRUE(net && trace);
  EXPECT_EQ((*net)["request"], "load_equipotential");
  EXPECT_EQ((*trace)["request"], "trace_driver");
  EXPECT_EQ((*trace)["path"], json({0, 0}));
  EXPECT_EQ((*trace)["term_id"], 2);
  const int id = (*trace)["trace_id"];
  EXPECT_NE(id, (*net)["trace_id"].get<int>());
  EXPECT_EQ(TraceStore::find(id)->label, "u_sub/u_and/O");
  EXPECT_FALSE(ShowNetStore::request(push({{"trace", true}}), clearView));  // already shown
  EXPECT_EQ(ShowNetStore::takeReply(id), id);
}

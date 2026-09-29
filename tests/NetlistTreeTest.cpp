#include "NetlistTree.h"
#include "TraceStore.h"
#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include "FakeNetlistProvider.h"

namespace {

DesignRef someDesign() { return DesignRef{1, 1, 1}; }

} // namespace

TEST(NetlistTree, RootInstancePathIsEmpty) {
  FakeNetlistProvider provider;
  NetlistTree tree(&provider);
  tree.createRootNode("top", someDesign(), /*hasTerms=*/false,
                       /*hasPrimitives=*/false, /*hasInstances=*/true,
                       /*hasNets=*/false);

  EXPECT_TRUE(tree.getRoot()->isRoot());
  EXPECT_EQ(tree.getRoot()->getInstancePath(), (InstancePath{}));
}

// The tree only exposes children through render()/expand(), which draws via
// ImGui and isn't something a unit test should invoke. Instead this test
// walks the same create*Node calls AppLogic's message handler uses, then
// reaches the created node directly via NetlistTree::getNode(), which is
// populated by every node's constructor regardless of expand()/render().
TEST(NetlistTree, InstancePathViaGetNode) {
  FakeNetlistProvider provider;
  NetlistTree tree(&provider);
  tree.createRootNode("top", someDesign(), false, false, true, false);
  auto* root = tree.getRoot();

  root->createChildren();
  root->createInstanceNode("u1", "MOD_A", 1, someDesign(), false, false, true, false);
  // Root's guiID is 0 (first node inserted); u1 is inserted right after.
  auto* u1 = tree.getNode(1);
  ASSERT_NE(u1, nullptr);
  EXPECT_EQ(u1->getInstancePath(), (InstancePath{"u1"}));

  u1->createChildren();
  u1->createInstanceNode("u2", "MOD_B", 2, someDesign(), false, false, false, false);
  auto* u2 = tree.getNode(2);
  ASSERT_NE(u2, nullptr);
  EXPECT_EQ(u2->getInstancePath(), (InstancePath{"u1", "u2"}));
}

TEST(NetlistTree, InstancePathKeepsSlashesInsideNames) {
  FakeNetlistProvider provider;
  NetlistTree tree(&provider);
  tree.createRootNode("top", someDesign(), false, false, true, false);
  auto* root = tree.getRoot();

  root->createChildren();
  root->createInstanceNode("a/b", "MOD_A", 1, someDesign(), false, false, true, false);
  auto* ab = tree.getNode(1);
  ab->createChildren();
  ab->createInstanceNode("u2", "MOD_B", 2, someDesign(), false, false, false, false);
  auto* u2 = tree.getNode(2);
  ASSERT_NE(u2, nullptr);
  EXPECT_EQ(u2->getInstancePath(), (InstancePath{"a/b", "u2"}));
}

TEST(NetlistTree, TermNodeBusBitsCountDownFromMsb) {
  FakeNetlistProvider provider;
  NetlistTree tree(&provider);
  tree.createRootNode("top", someDesign(), true, false, false, false);
  auto* root = tree.getRoot();

  root->createChildren();
  root->createTermNode("data", 10, Direction::Input, /*msb=*/7, /*lsb=*/0);
  auto* term = tree.getNode(1);
  ASSERT_NE(term, nullptr);

  EXPECT_TRUE(term->isBus());
  EXPECT_FALSE(term->isBitTerm());
  auto bits = term->busBits();
  ASSERT_EQ(bits.size(), 8u);
  EXPECT_EQ(bits.front(), 7);
  EXPECT_EQ(bits.back(), 0);
}

TEST(NetlistTree, TermNodeBusBitsCountUpWhenLsbAboveMsb) {
  FakeNetlistProvider provider;
  NetlistTree tree(&provider);
  tree.createRootNode("top", someDesign(), true, false, false, false);
  auto* root = tree.getRoot();

  root->createChildren();
  root->createTermNode("data", 10, Direction::Output, /*msb=*/0, /*lsb=*/3);
  auto* term = tree.getNode(1);
  ASSERT_NE(term, nullptr);

  auto bits = term->busBits();
  EXPECT_EQ(bits, (std::vector<int>{0, 1, 2, 3}));
}

TEST(NetlistTree, ScalarTermIsBitTermNotBus) {
  FakeNetlistProvider provider;
  NetlistTree tree(&provider);
  tree.createRootNode("top", someDesign(), true, false, false, false);
  auto* root = tree.getRoot();

  root->createChildren();
  root->createTermNode("q", 11, Direction::Output, std::nullopt, std::nullopt);
  auto* term = tree.getNode(1);
  ASSERT_NE(term, nullptr);

  EXPECT_TRUE(term->isBitTerm());
  EXPECT_FALSE(term->isBus());
  EXPECT_TRUE(term->busBits().empty());
}

TEST(NetlistTree, NetNodeBusBitsCountDownFromMsb) {
  FakeNetlistProvider provider;
  NetlistTree tree(&provider);
  tree.createRootNode("top", someDesign(), false, false, false, true);
  auto* root = tree.getRoot();

  root->createChildren();
  root->createNetNode("data", /*msb=*/7, /*lsb=*/0);
  auto* net = tree.getNode(1);
  ASSERT_NE(net, nullptr);

  EXPECT_TRUE(net->isBusNet());
  EXPECT_FALSE(net->isBitNet());
  auto bits = net->busBits();
  ASSERT_EQ(bits.size(), 8u);
  EXPECT_EQ(bits.front(), 7);
  EXPECT_EQ(bits.back(), 0);
}

TEST(NetlistTree, ScalarNetIsBitNetNotBus) {
  FakeNetlistProvider provider;
  NetlistTree tree(&provider);
  tree.createRootNode("top", someDesign(), false, false, false, true);
  auto* root = tree.getRoot();

  root->createChildren();
  root->createNetNode("n1", std::nullopt, std::nullopt);
  auto* net = tree.getNode(1);
  ASSERT_NE(net, nullptr);

  EXPECT_TRUE(net->isBitNet());
  EXPECT_FALSE(net->isBusNet());
  EXPECT_TRUE(net->busBits().empty());
}

TEST(NetlistTree, SendLoadEquipotentialFormatsRequestAndFiresCallback) {
  FakeNetlistProvider provider;
  NetlistTree tree(&provider);
  bool callbackFired = false;
  tree.setOnEquipotentialRequest([&] { callbackFired = true; });

  tree.sendLoadEquipotential({1, 2}, NetlistTree::TermID{9, true, 3});

  ASSERT_EQ(provider.sent.size(), 1u);
  EXPECT_EQ(provider.sent[0],
            R"({"request":"load_equipotential","path":[1,2],"term_id":9,"bit":3})");
  EXPECT_TRUE(callbackFired);
}

TEST(NetlistTree, SendLoadEquipotentialOmitsBitForNonBusBit) {
  FakeNetlistProvider provider;
  NetlistTree tree(&provider);

  tree.sendLoadEquipotential({}, NetlistTree::TermID{4, false, 0});

  ASSERT_EQ(provider.sent.size(), 1u);
  EXPECT_EQ(provider.sent[0], R"({"request":"load_equipotential","path":[],"term_id":4})");
}

TEST(NetlistTree, SendTraceDriverFormatsScalarRequestAndFiresCallback) {
  FakeNetlistProvider provider;
  NetlistTree tree(&provider);
  bool callbackFired = false;
  tree.setOnEquipotentialRequest([&] { callbackFired = true; });

  tree.sendTraceDriver({1, 2}, 9, {}, "u1/A");

  ASSERT_EQ(provider.sent.size(), 1u);
  auto req = nlohmann::json::parse(provider.sent[0]);
  EXPECT_EQ(req["request"], "trace_driver");
  EXPECT_EQ(req["path"], (std::vector<unsigned>{1, 2}));
  EXPECT_EQ(req["term_id"], 9u);
  EXPECT_FALSE(req.contains("bit"));
  EXPECT_FALSE(req.contains("bits"));
  EXPECT_TRUE(callbackFired);
  // Registered as a trace, whose id the response will carry back.
  const Trace* trace = TraceStore::find(req["trace_id"].get<int>());
  ASSERT_NE(trace, nullptr);
  EXPECT_EQ(trace->label, "u1/A");
  TraceStore::clear();
}

// "Add Trace to View": the trace is overlaid, the view isn't cleared.
TEST(NetlistTree, SendTraceDriverCanKeepTheView) {
  FakeNetlistProvider provider;
  NetlistTree tree(&provider);
  bool callbackFired = false;
  tree.setOnEquipotentialRequest([&] { callbackFired = true; });

  tree.sendTraceDriver({}, 4, {}, "d", /*clearView=*/false);

  ASSERT_EQ(provider.sent.size(), 1u);
  EXPECT_FALSE(callbackFired);
  TraceStore::clear();
}

TEST(NetlistTree, SendTraceDriverSingleBitUsesBitKey) {
  FakeNetlistProvider provider;
  NetlistTree tree(&provider);

  tree.sendTraceDriver({}, 4, {3}, "d[3]");

  ASSERT_EQ(provider.sent.size(), 1u);
  auto req = nlohmann::json::parse(provider.sent[0]);
  EXPECT_EQ(req["bit"], 3);
  EXPECT_FALSE(req.contains("bits"));
}

TEST(NetlistTree, SendTraceDriverBusSendsOneRequestWithAllBits) {
  FakeNetlistProvider provider;
  NetlistTree tree(&provider);

  tree.sendTraceDriver({}, 4, {0, 1, 2}, "d");

  ASSERT_EQ(provider.sent.size(), 1u);
  auto req = nlohmann::json::parse(provider.sent[0]);
  EXPECT_EQ(req["bits"], (std::vector<int>{0, 1, 2}));
  EXPECT_FALSE(req.contains("bit"));
}

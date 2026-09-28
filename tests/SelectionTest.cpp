#include "NetlistTree.h"
#include "SelectionStore.h"

#include <imgui.h>
#include <nlohmann/json.hpp>
#include <gtest/gtest.h>

#include "FakeNetlistProvider.h"

// ---------------------------------------------------------------------------
// SelectionStore
// ---------------------------------------------------------------------------

class SelectionStoreTest : public ::testing::Test {
 protected:
  void SetUp() override {
    SelectionStore::clear();
    SelectionStore::setListener([this](const std::string& k) { notified.push_back(k); });
  }
  void TearDown() override {
    SelectionStore::setListener(nullptr);
    SelectionStore::clear();
  }
  std::vector<std::string> notified;
};

TEST_F(SelectionStoreTest, SelectNotifiesOncePerChange) {
  EXPECT_FALSE(SelectionStore::hasSelection());
  const unsigned rev = SelectionStore::revision();

  SelectionStore::select("u1/u2", SelectionStore::Origin::Schematic);
  EXPECT_TRUE(SelectionStore::isSelected("u1/u2"));
  EXPECT_FALSE(SelectionStore::isSelected("u1"));
  EXPECT_EQ(SelectionStore::origin(), SelectionStore::Origin::Schematic);
  EXPECT_EQ(SelectionStore::revision(), rev + 1);

  SelectionStore::select("u1/u2", SelectionStore::Origin::Tree);  // same: no-op
  EXPECT_EQ(SelectionStore::revision(), rev + 1);
  EXPECT_EQ(notified, std::vector<std::string>{"u1/u2"});
}

TEST_F(SelectionStoreTest, TopDesignIsSelectable) {
  SelectionStore::select("", SelectionStore::Origin::Host);
  EXPECT_TRUE(SelectionStore::hasSelection());
  EXPECT_TRUE(SelectionStore::isSelected(""));
  EXPECT_EQ(notified, std::vector<std::string>{""});
}

TEST_F(SelectionStoreTest, ClearDoesNotNotify) {
  SelectionStore::select("u1", SelectionStore::Origin::Tree);
  SelectionStore::clear();
  EXPECT_FALSE(SelectionStore::hasSelection());
  EXPECT_FALSE(SelectionStore::isSelected("u1"));
  EXPECT_EQ(notified.size(), 1u);
}

// ---------------------------------------------------------------------------
// NetlistTree::reveal -- driven by the load requests it actually sends, the
// replies simulated the way AppLogic handles instances/primitives responses.
// ---------------------------------------------------------------------------

namespace {

DesignRef design(int id) { return DesignRef{1, 1, id}; }

class TreeReveal : public ::testing::Test {
 protected:
  void SetUp() override {
    tree.createRootNode("top", design(0), /*hasTerms=*/true, /*hasPrimitives=*/true,
                        /*hasInstances=*/true, /*hasNets=*/false);
  }

  // Requests of `type` sent since the last call, as parsed JSON.
  std::vector<nlohmann::json> take(const std::string& type) {
    std::vector<nlohmann::json> out, rest;
    for (const auto& s : provider.sent) {
      auto j = nlohmann::json::parse(s);
      (j.value("request", "") == type ? out : rest).push_back(j);
    }
    provider.sent.clear();
    for (const auto& j : rest) provider.sent.push_back(j.dump());
    return out;
  }

  struct Child { std::string name; int id; bool hierarchical; };
  // Answers a load request for group node `guiId` with `children`.
  void answer(unsigned guiId, const std::vector<Child>& children) {
    NetlistTreeNode* group = tree.getNode(guiId);
    ASSERT_NE(group, nullptr);
    group->createChildren();
    for (const auto& c : children)
      group->createInstanceNode(c.name, "M_" + c.name, c.id, design(c.id),
                                /*hasTerms=*/true, /*hasPrimitives=*/c.hierarchical,
                                /*hasInstances=*/c.hierarchical, /*hasNets=*/false);
  }

  FakeNetlistProvider provider;
  NetlistTree         tree{&provider};
};

} // namespace

TEST_F(TreeReveal, WalksDownLoadingOnlyWhatIsNeeded) {
  tree.reveal("u1/u2");
  tree.advanceReveal();
  EXPECT_TRUE(tree.isRevealPending());

  // Level 1 isn't loaded: both instance groups of the root are requested
  // (u1 could be hierarchical or a primitive), but no terms/nets.
  auto inst = take("load_instances");
  auto prim = take("load_primitives");
  ASSERT_EQ(inst.size(), 1u);
  ASSERT_EQ(prim.size(), 1u);
  EXPECT_TRUE(provider.sent.empty());

  // Waiting for replies: advancing again doesn't re-send.
  tree.advanceReveal();
  EXPECT_TRUE(provider.sent.empty());

  answer(inst[0]["gui_id"], {{"u0", 10, true}, {"u1", 11, true}});
  answer(prim[0]["gui_id"], {{"r", 12, false}});
  tree.advanceReveal();
  EXPECT_TRUE(tree.isRevealPending());
  inst = take("load_instances");
  prim = take("load_primitives");
  ASSERT_EQ(inst.size(), 1u);
  ASSERT_EQ(prim.size(), 1u);
  // Requests for u1's own groups, addressed by u1's model.
  EXPECT_EQ(inst[0]["design_ref"]["design_id"], 11);

  answer(inst[0]["gui_id"], {});
  answer(prim[0]["gui_id"], {{"u2", 20, false}});
  tree.advanceReveal();
  EXPECT_FALSE(tree.isRevealPending());
  NetlistTreeNode* u2 = tree.findInstance("u1/u2");
  ASSERT_NE(u2, nullptr);
  EXPECT_EQ(u2->getPathKey(), "u1/u2");
}

TEST_F(TreeReveal, AlreadyLoadedLevelsNeedNoRequest) {
  tree.reveal("u1");
  tree.advanceReveal();
  auto inst = take("load_instances");
  auto prim = take("load_primitives");
  answer(inst[0]["gui_id"], {{"u1", 11, true}});
  answer(prim[0]["gui_id"], {});
  tree.advanceReveal();
  ASSERT_FALSE(tree.isRevealPending());

  tree.reveal("u1");  // again: everything is there
  tree.advanceReveal();
  EXPECT_FALSE(tree.isRevealPending());
  EXPECT_TRUE(provider.sent.empty());
}

TEST_F(TreeReveal, FindsAnInstanceInTheFirstLoadedGroupWithoutWaitingForOthers) {
  tree.reveal("u1");
  tree.advanceReveal();
  auto inst = take("load_instances");
  take("load_primitives");  // never answered
  answer(inst[0]["gui_id"], {{"u1", 11, true}});
  tree.advanceReveal();
  EXPECT_FALSE(tree.isRevealPending());
  EXPECT_NE(tree.findInstance("u1"), nullptr);
}

TEST_F(TreeReveal, UnknownNameStopsTheReveal) {
  tree.reveal("nope");
  tree.advanceReveal();
  answer(take("load_instances")[0]["gui_id"], {{"u1", 11, true}});
  answer(take("load_primitives")[0]["gui_id"], {});
  tree.advanceReveal();
  EXPECT_FALSE(tree.isRevealPending());
  EXPECT_EQ(tree.findInstance("nope"), nullptr);
}

TEST_F(TreeReveal, RootIsRevealedImmediately) {
  tree.reveal("");
  tree.advanceReveal();
  EXPECT_FALSE(tree.isRevealPending());
  EXPECT_EQ(tree.findInstance(""), tree.getRoot());
}

// A selection made outside the tree (schematic click, host focus) makes the
// tree reveal it on its next render; one made in the tree doesn't.
TEST_F(TreeReveal, RevealsASelectionMadeElsewhereOnRender) {
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(800.f, 600.f);
  io.DeltaTime   = 1.f / 60.f;
  io.IniFilename = nullptr;
  unsigned char* px; int w, h;
  io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
  auto frame = [&] {
    ImGui::NewFrame();
    ImGui::Begin("Tree");
    tree.render();
    ImGui::End();
    ImGui::Render();
  };

  SelectionStore::clear();
  frame();
  provider.sent.clear();

  SelectionStore::select("u1", SelectionStore::Origin::Tree);
  frame();
  EXPECT_FALSE(tree.isRevealPending());

  SelectionStore::select("u9", SelectionStore::Origin::Schematic);
  frame();
  EXPECT_TRUE(tree.isRevealPending());
  EXPECT_FALSE(take("load_instances").empty());

  SelectionStore::clear();
  ImGui::DestroyContext();
}

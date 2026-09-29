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
    SelectionStore::setListener([this](const InstancePath& p) { notified.push_back(p); });
  }
  void TearDown() override {
    SelectionStore::setListener(nullptr);
    SelectionStore::clear();
  }
  std::vector<InstancePath> notified;
};

TEST_F(SelectionStoreTest, SelectNotifiesOncePerChange) {
  EXPECT_FALSE(SelectionStore::hasSelection());
  const unsigned rev = SelectionStore::revision();

  SelectionStore::select(InstancePath{{1, "u1"}, {2, "u2"}}, SelectionStore::Origin::Schematic);
  EXPECT_TRUE(SelectionStore::isSelected(InstancePath{{1, "u1"}, {2, "u2"}}));
  EXPECT_FALSE(SelectionStore::isSelected(InstancePath{{1, "u1"}}));
  EXPECT_EQ(SelectionStore::origin(), SelectionStore::Origin::Schematic);
  EXPECT_EQ(SelectionStore::revision(), rev + 1);

  SelectionStore::select(InstancePath{{1, "u1"}, {2, "u2"}}, SelectionStore::Origin::Tree);  // same: no-op
  EXPECT_EQ(SelectionStore::revision(), rev + 1);
  EXPECT_EQ(notified, (std::vector<InstancePath>{{{1, "u1"}, {2, "u2"}}}));
}

// Anonymous siblings share the name "": the id tells them apart, so
// switching from one to the other is a selection change.
TEST_F(SelectionStoreTest, AnonymousSiblingsAreDistinctSelections) {
  SelectionStore::select(InstancePath{{3, ""}}, SelectionStore::Origin::Schematic);
  EXPECT_TRUE(SelectionStore::isSelected(InstancePath{{3, ""}}));
  EXPECT_FALSE(SelectionStore::isSelected(InstancePath{{4, ""}}));
  SelectionStore::select(InstancePath{{4, ""}}, SelectionStore::Origin::Schematic);
  EXPECT_EQ(notified, (std::vector<InstancePath>{{{3, ""}}, {{4, ""}}}));
  EXPECT_EQ(pathIds(notified.back()), (std::vector<unsigned>{4}));
}

TEST_F(SelectionStoreTest, TopDesignIsSelectable) {
  SelectionStore::select(InstancePath{}, SelectionStore::Origin::Host);
  EXPECT_TRUE(SelectionStore::hasSelection());
  EXPECT_TRUE(SelectionStore::isSelected(InstancePath{}));
  EXPECT_EQ(notified, std::vector<InstancePath>{InstancePath{}});
}

TEST_F(SelectionStoreTest, ClearDoesNotNotify) {
  SelectionStore::select(InstancePath{{1, "u1"}}, SelectionStore::Origin::Tree);
  SelectionStore::clear();
  EXPECT_FALSE(SelectionStore::hasSelection());
  EXPECT_FALSE(SelectionStore::isSelected(InstancePath{{1, "u1"}}));
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
  tree.reveal(InstancePath{{11, "u1"}, {20, "u2"}});
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
  NetlistTreeNode* u2 = tree.findInstance(InstancePath{{11, "u1"}, {20, "u2"}});
  ASSERT_NE(u2, nullptr);
  EXPECT_EQ(u2->getInstancePath(), (InstancePath{{11, "u1"}, {20, "u2"}}));
  EXPECT_EQ(pathNames(u2->getInstancePath()), (std::vector<std::string>{"u1", "u2"}));
}

// An escaped instance name can contain '/': it's one level, not two.
TEST_F(TreeReveal, NamesContainingSlashesAreOneLevel) {
  const InstancePath key{{11, "a/b"}, {20, "u2"}};
  tree.reveal(key);
  tree.advanceReveal();
  auto inst = take("load_instances");
  auto prim = take("load_primitives");
  answer(inst[0]["gui_id"], {{"a", 10, true}, {"a/b", 11, true}});
  answer(prim[0]["gui_id"], {});
  tree.advanceReveal();
  inst = take("load_instances");
  prim = take("load_primitives");
  ASSERT_EQ(inst.size(), 1u);
  EXPECT_EQ(inst[0]["design_ref"]["design_id"], 11);  // under "a/b", not "a"
  answer(inst[0]["gui_id"], {});
  answer(prim[0]["gui_id"], {{"u2", 20, false}});
  tree.advanceReveal();
  EXPECT_FALSE(tree.isRevealPending());
  NetlistTreeNode* u2 = tree.findInstance(key);
  ASSERT_NE(u2, nullptr);
  EXPECT_EQ(u2->getInstancePath(), key);
  EXPECT_EQ(pathNames(u2->getInstancePath()), (std::vector<std::string>{"a/b", "u2"}));
}

// Anonymous instances all have the name "": the reveal walks by id, so it
// reaches the right one of several anonymous siblings, at every level.
TEST_F(TreeReveal, AnonymousSiblingsAreRevealedById) {
  const InstancePath key{{11, ""}, {21, ""}};
  tree.reveal(key);
  tree.advanceReveal();
  auto inst = take("load_instances");
  auto prim = take("load_primitives");
  answer(inst[0]["gui_id"], {{"", 10, true}, {"", 11, true}});
  answer(prim[0]["gui_id"], {});
  tree.advanceReveal();
  inst = take("load_instances");
  prim = take("load_primitives");
  ASSERT_EQ(inst.size(), 1u);
  EXPECT_EQ(inst[0]["design_ref"]["design_id"], 11);  // the second anonymous one
  answer(inst[0]["gui_id"], {});
  answer(prim[0]["gui_id"], {{"", 20, false}, {"", 21, false}});
  tree.advanceReveal();
  EXPECT_FALSE(tree.isRevealPending());
  NetlistTreeNode* leaf = tree.findInstance(key);
  ASSERT_NE(leaf, nullptr);
  EXPECT_EQ(leaf->getChildID(), 21u);
  EXPECT_EQ(leaf->getInstancePath(), key);
  EXPECT_EQ(leaf->getLabel(), "<#21> (M_)");
  EXPECT_NE(tree.findInstance(InstancePath{{11, ""}, {20, ""}}), leaf);
}

TEST_F(TreeReveal, AlreadyLoadedLevelsNeedNoRequest) {
  tree.reveal(InstancePath{{11, "u1"}});
  tree.advanceReveal();
  auto inst = take("load_instances");
  auto prim = take("load_primitives");
  answer(inst[0]["gui_id"], {{"u1", 11, true}});
  answer(prim[0]["gui_id"], {});
  tree.advanceReveal();
  ASSERT_FALSE(tree.isRevealPending());

  tree.reveal(InstancePath{{11, "u1"}});  // again: everything is there
  tree.advanceReveal();
  EXPECT_FALSE(tree.isRevealPending());
  EXPECT_TRUE(provider.sent.empty());
}

TEST_F(TreeReveal, FindsAnInstanceInTheFirstLoadedGroupWithoutWaitingForOthers) {
  tree.reveal(InstancePath{{11, "u1"}});
  tree.advanceReveal();
  auto inst = take("load_instances");
  take("load_primitives");  // never answered
  answer(inst[0]["gui_id"], {{"u1", 11, true}});
  tree.advanceReveal();
  EXPECT_FALSE(tree.isRevealPending());
  EXPECT_NE(tree.findInstance(InstancePath{{11, "u1"}}), nullptr);
}

TEST_F(TreeReveal, UnknownIdStopsTheReveal) {
  tree.reveal(InstancePath{{99, "u1"}});  // the name alone doesn't make it u1
  tree.advanceReveal();
  answer(take("load_instances")[0]["gui_id"], {{"u1", 11, true}});
  answer(take("load_primitives")[0]["gui_id"], {});
  tree.advanceReveal();
  EXPECT_FALSE(tree.isRevealPending());
  EXPECT_EQ(tree.findInstance(InstancePath{{99, "u1"}}), nullptr);
}

TEST_F(TreeReveal, RootIsRevealedImmediately) {
  tree.reveal(InstancePath{});
  tree.advanceReveal();
  EXPECT_FALSE(tree.isRevealPending());
  EXPECT_EQ(tree.findInstance(InstancePath{}), tree.getRoot());
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

  SelectionStore::select(InstancePath{{1, "u1"}}, SelectionStore::Origin::Tree);
  frame();
  EXPECT_FALSE(tree.isRevealPending());

  SelectionStore::select(InstancePath{{9, "u9"}}, SelectionStore::Origin::Schematic);
  frame();
  EXPECT_TRUE(tree.isRevealPending());
  EXPECT_FALSE(take("load_instances").empty());

  SelectionStore::clear();
  ImGui::DestroyContext();
}

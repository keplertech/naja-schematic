#include "NetlistTree.h"
#include "SelectionStore.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <cfloat>
#include <cmath>
#include <optional>
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

// A review "go to" a pin: on into the instance's Terms group, to the bus
// bit's row, which is highlighted.
TEST_F(TreeReveal, RevealsATermBitRowInsideTheInstance) {
  tree.reveal(InstancePath{{11, "u1"}},
              NetlistTree::RevealLeaf{NetlistTree::RevealLeaf::Kind::Term, 5, 1});
  tree.advanceReveal();
  answer(take("load_instances")[0]["gui_id"], {{"u1", 11, true}});
  answer(take("load_primitives")[0]["gui_id"], {});
  tree.advanceReveal();
  // At u1: its terms are loaded, nothing else.
  EXPECT_TRUE(tree.isRevealPending());
  auto terms = take("load_terms");
  ASSERT_EQ(terms.size(), 1u);
  EXPECT_TRUE(provider.sent.empty());
  tree.advanceReveal();  // waiting: not sent again
  EXPECT_TRUE(take("load_terms").empty());

  NetlistTreeNode* group = tree.getNode(terms[0]["gui_id"].get<unsigned>());
  group->createChildren();
  group->createTermNode("a", 4, Direction::Input, std::nullopt, std::nullopt);
  group->createTermNode("d", 5, Direction::Input, 3, 0);
  tree.advanceReveal();
  EXPECT_FALSE(tree.isRevealPending());
  NetlistTreeNode* row = tree.highlightedNode();
  ASSERT_NE(row, nullptr);
  EXPECT_TRUE(row->isBusBit());
  EXPECT_EQ(row->getBusBit(), 1);
  EXPECT_EQ(row->getTermBaseName(), "d");
  EXPECT_EQ(row->getInstancePath(), (InstancePath{{11, "u1"}}));

  // A term without a bit: the whole bus row.
  tree.reveal(InstancePath{{11, "u1"}},
              NetlistTree::RevealLeaf{NetlistTree::RevealLeaf::Kind::Term, 5, std::nullopt});
  tree.advanceReveal();
  ASSERT_NE(tree.highlightedNode(), nullptr);
  EXPECT_TRUE(tree.highlightedNode()->isBus());

  // A new reveal drops the highlight; an unknown term just stops.
  tree.reveal(InstancePath{{11, "u1"}},
              NetlistTree::RevealLeaf{NetlistTree::RevealLeaf::Kind::Term, 9, std::nullopt});
  tree.advanceReveal();
  EXPECT_FALSE(tree.isRevealPending());
  EXPECT_EQ(tree.highlightedNode(), nullptr);
}

TEST(TreeRevealNet, RevealsANetRowByItsId) {
  FakeNetlistProvider provider;
  NetlistTree tree{&provider};
  tree.createRootNode("top", DesignRef{1, 1, 0}, /*hasTerms=*/true, /*hasPrimitives=*/false,
                      /*hasInstances=*/false, /*hasNets=*/true);
  tree.reveal(InstancePath{}, NetlistTree::RevealLeaf{NetlistTree::RevealLeaf::Kind::Net, 7,
                                                      std::nullopt});
  tree.advanceReveal();
  ASSERT_EQ(provider.sent.size(), 1u);
  auto req = nlohmann::json::parse(provider.sent[0]);
  EXPECT_EQ(req["request"], "load_nets");
  NetlistTreeNode* group = tree.getNode(req["gui_id"].get<unsigned>());
  group->createChildren();
  group->createNetNode("", std::nullopt, std::nullopt, false, 3u);
  group->createNetNode("", std::nullopt, std::nullopt, false, 7u);  // anonymous: the id finds it
  tree.advanceReveal();
  ASSERT_NE(tree.highlightedNode(), nullptr);
  EXPECT_EQ(tree.highlightedNode()->getNetID(), 7u);
}

// The review tab selects the thread's instance, then (next frame) asks for
// the reveal down to its pin: the selection's own reveal of the instance
// alone must not replace it.
TEST_F(TreeReveal, ALeafRevealAfterASelectionIsNotReplacedByIt) {
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
  SelectionStore::select(InstancePath{}, SelectionStore::Origin::Host);
  tree.reveal(InstancePath{}, NetlistTree::RevealLeaf{NetlistTree::RevealLeaf::Kind::Term, 5,
                                                      std::nullopt});
  frame();
  // Still going to the term: its group was requested.
  EXPECT_EQ(take("load_terms").size(), 1u);
  SelectionStore::clear();
  ImGui::DestroyContext();
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

namespace {

// The filter is static: each test puts it back.
struct TreeFilterReset {
  bool terms = NetlistTree::showTermsInTree;
  bool nets  = NetlistTree::showNetsInTree;
  ~TreeFilterReset() {
    NetlistTree::showTermsInTree = terms;
    NetlistTree::showNetsInTree  = nets;
  }
};

}  // namespace

// Choosing a context-menu entry must not select the tree row that happens
// to lie under the menu item: the click belongs to the menu.
TEST_F(TreeReveal, ContextMenuClickDoesNotSelectTheRowUnderIt) {
  TreeFilterReset reset;  // rows are clicked by position: the Terms group drawn
  NetlistTree::showTermsInTree = true;
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(800.f, 600.f);
  io.DeltaTime   = 1.f / 60.f;
  io.IniFilename = nullptr;
  unsigned char* px; int w, h;
  io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
  ImVec2 origin;
  float pitch = 0.f;
  auto frame = [&] {
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(400, 500));
    ImGui::Begin("Tree", nullptr, ImGuiWindowFlags_NoMove);
    origin = ImGui::GetCursorScreenPos();
    pitch  = ImGui::GetTextLineHeightWithSpacing();
    tree.render();
    ImGui::End();
    ImGui::Render();
  };
  // Center of tree row `row` (0 = top), `x` from the window's content start.
  auto at = [&](int row, float x) {
    return ImVec2(origin.x + x, origin.y + row * pitch + ImGui::GetFontSize() * 0.5f);
  };
  auto click = [&](ImVec2 pos, ImGuiMouseButton button) {
    io.AddMousePosEvent(pos.x, pos.y);
    frame();
    io.AddMouseButtonEvent(button, true);
    frame();
    io.AddMouseButtonEvent(button, false);
    frame();
    frame();
  };

  SelectionStore::clear();
  frame();
  click(at(0, 4.f), ImGuiMouseButton_Left);   // open "top"
  // Its groups: Terms, Primitives, Instances. Open Instances (on its arrow,
  // one indent in): that loads it.
  click(at(3, ImGui::GetStyle().IndentSpacing + 5.f), ImGuiMouseButton_Left);
  auto load = take("load_instances");
  ASSERT_EQ(load.size(), 1u);
  answer(load[0]["gui_id"].get<unsigned>(),
         {{"u0", 0, false}, {"u1", 1, false}, {"u2", 2, false}});
  frame();
  SelectionStore::clear();
  provider.sent.clear();

  click(at(4, 60.f), ImGuiMouseButton_Right);  // u0's context menu
  // Its first entry ("Show in Schematic") lies over the next row, u1's.
  click(ImVec2(at(4, 60.f).x + 30.f, at(5, 0.f).y), ImGuiMouseButton_Left);

  auto resolve = take("resolve_instance");
  ASSERT_EQ(resolve.size(), 1u);
  EXPECT_EQ(resolve[0]["id_path"], nlohmann::json::array({0}));  // u0's
  EXPECT_FALSE(SelectionStore::isSelected(InstancePath{{1, "u1"}}));
  EXPECT_FALSE(SelectionStore::isSelected(InstancePath{{2, "u2"}}));

  SelectionStore::clear();
  ImGui::DestroyContext();
}

// A level with thousands of rows (a memory built of flops) costs what its
// visible rows do: the rest are only stepped over, at their own height, so
// the scroll range and every row's place are as if all were drawn.
TEST_F(TreeReveal, OffScreenRowsAreSkippedAtTheirHeight) {
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(800.f, 600.f);
  io.DeltaTime   = 1.f / 60.f;
  io.IniFilename = nullptr;
  unsigned char* px; int w, h;
  io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
  ImVec2 origin;
  float pitch = 0.f, end = 0.f;
  std::optional<float> scrollTo;
  auto frame = [&] {
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(400, 500));
    ImGui::Begin("Tree", nullptr, ImGuiWindowFlags_NoMove);
    if (scrollTo) ImGui::SetScrollY(*scrollTo == FLT_MAX ? ImGui::GetScrollMaxY() : *scrollTo);
    origin = ImGui::GetCursorScreenPos();
    pitch  = ImGui::GetTextLineHeightWithSpacing();
    tree.render();
    end = ImGui::GetCursorScreenPos().y;
    ImGui::End();
    ImGui::Render();
  };
  constexpr int kRows = 5000;
  SelectionStore::clear();
  tree.reveal(InstancePath{{0, "u0"}});
  frame();
  auto inst = take("load_instances");
  auto prim = take("load_primitives");
  ASSERT_EQ(inst.size(), 1u);
  ASSERT_EQ(prim.size(), 1u);
  std::vector<Child> children;
  for (int i = 0; i < kRows; ++i) children.push_back({"u" + std::to_string(i), i, false});
  answer(inst[0]["gui_id"].get<unsigned>(), children);
  answer(prim[0]["gui_id"].get<unsigned>(), {});
  frame();
  frame();
  EXPECT_FALSE(tree.isRevealPending());

  scrollTo = 0.f;
  frame();
  frame();
  const float rows = (end - origin.y) / pitch;
  EXPECT_NEAR(rows, std::round(rows), 1e-3f);
  EXPECT_GE(rows, kRows + 1.f);  // the top, its groups and every child
  EXPECT_LT(tree.rowsDrawnForTesting(), 60u);
  // At the bottom: the last row is where it would be with every row drawn.
  scrollTo = FLT_MAX;
  frame();
  frame();
  EXPECT_LT(tree.rowsDrawnForTesting(), 60u);
  const int last = static_cast<int>(std::round((end - origin.y) / pitch)) - 1;
  // On the label, past the arrow (the children are two indents in).
  const ImVec2 at(origin.x + 2 * ImGui::GetStyle().IndentSpacing + 2 * ImGui::GetFontSize(),
                  origin.y + last * pitch + ImGui::GetFontSize() * 0.5f);
  ASSERT_LT(at.y, 500.f);
  scrollTo.reset();
  io.AddMousePosEvent(at.x, at.y);
  frame();
  io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
  frame();
  io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
  frame();
  EXPECT_TRUE(SelectionStore::isSelected(InstancePath{{kRows - 1, "u" + std::to_string(kRows - 1)}}));

  SelectionStore::clear();
  ImGui::DestroyContext();
}

// A skipped row still counts toward the tree's width, as it was last drawn:
// the horizontal extent doesn't shrink when the longest name scrolls away.
TEST_F(TreeReveal, OffScreenRowsKeepTheirWidth) {
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(800.f, 600.f);
  io.DeltaTime   = 1.f / 60.f;
  io.IniFilename = nullptr;
  unsigned char* px; int w, h;
  io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
  float pitch = 0.f, right = 0.f;
  std::optional<float> scrollTo;
  auto frame = [&] {
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(300, 500));
    ImGui::Begin("Tree", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_HorizontalScrollbar);
    if (scrollTo) ImGui::SetScrollY(*scrollTo);
    pitch = ImGui::GetTextLineHeightWithSpacing();
    tree.render();
    right = ImGui::GetCurrentWindow()->DC.CursorMaxPos.x;
    ImGui::End();
    ImGui::Render();
  };
  constexpr int kRows = 5000, kLong = 2500;
  SelectionStore::clear();
  tree.reveal(InstancePath{{0, "u0"}});
  frame();
  auto inst = take("load_instances");
  auto prim = take("load_primitives");
  ASSERT_EQ(inst.size(), 1u);
  ASSERT_EQ(prim.size(), 1u);
  std::vector<Child> children;
  for (int i = 0; i < kRows; ++i)
    children.push_back({i == kLong ? std::string(200, 'x') : "u" + std::to_string(i), i, false});
  answer(inst[0]["gui_id"].get<unsigned>(), children);
  answer(prim[0]["gui_id"].get<unsigned>(), {});
  scrollTo = 0.f;
  frame(); frame();
  const float narrow = right;

  scrollTo = (kLong + 4) * pitch - 200.f;  // the long row in view
  frame(); frame();
  const float wide = right;
  EXPECT_GT(wide, narrow + 500.f);

  scrollTo = 0.f;  // and out of view again
  frame(); frame();
  EXPECT_FLOAT_EQ(right, wide);
  EXPECT_LT(tree.rowsDrawnForTesting(), 60u);

  SelectionStore::clear();
  ImGui::DestroyContext();
}

namespace {

// A headless ImGui context drawing the tree's selection panel, as the
// bottom-left panel of the app does.
class SelectionPanel : public TreeReveal {
 protected:
  void SetUp() override {
    TreeReveal::SetUp();
    SelectionStore::clear();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800.f, 600.f);
    io.DeltaTime   = 1.f / 60.f;
    io.IniFilename = nullptr;
    unsigned char* px; int w, h;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
  }
  void TearDown() override {
    SelectionStore::clear();
    ImGui::DestroyContext();
  }
  void frame() {
    ImGui::NewFrame();
    ImGui::Begin("Panel");
    tree.renderSelectionPanel();
    ImGui::End();
    ImGui::Render();
  }
};

}  // namespace

// Nothing selected: the panel shows the top design, and asks for its terms
// once (the Terms tab is the first one, so the one shown).
TEST_F(SelectionPanel, ShowsTheTopsTermsWhenNothingIsSelected) {
  EXPECT_EQ(tree.selectionPanelInstance(), tree.getRoot());
  frame();
  auto terms = take("load_terms");
  ASSERT_EQ(terms.size(), 1u);
  EXPECT_EQ(terms[0]["design_ref"]["design_id"], 0);
  frame();  // waiting: not sent again
  EXPECT_TRUE(take("load_terms").empty());
}

// A selected instance whose tree level isn't loaded yet: nothing to show
// (and nothing requested) until the tree's reveal has loaded it; then its
// own terms.
TEST_F(SelectionPanel, FollowsTheSelectedInstance) {
  SelectionStore::select(InstancePath{{11, "u1"}}, SelectionStore::Origin::Schematic);
  EXPECT_EQ(tree.selectionPanelInstance(), nullptr);
  frame();
  EXPECT_TRUE(take("load_terms").empty());

  tree.reveal(InstancePath{{11, "u1"}});
  tree.advanceReveal();
  answer(take("load_instances")[0]["gui_id"], {{"u1", 11, true}});
  answer(take("load_primitives")[0]["gui_id"], {});
  tree.advanceReveal();
  NetlistTreeNode* u1 = tree.selectionPanelInstance();
  ASSERT_NE(u1, nullptr);
  EXPECT_EQ(u1->getInstancePath(), (InstancePath{{11, "u1"}}));
  frame();
  auto terms = take("load_terms");
  ASSERT_EQ(terms.size(), 1u);
  EXPECT_EQ(terms[0]["design_ref"]["design_id"], 11);
}

// A review "go to" a net: the panel switches to its Nets tab (for one
// frame), and the row is the highlighted one.
TEST_F(SelectionPanel, ALeafRevealSelectsItsTab) {
  FakeNetlistProvider netProvider;
  NetlistTree netTree{&netProvider};
  netTree.createRootNode("top", design(0), /*hasTerms=*/true, /*hasPrimitives=*/false,
                         /*hasInstances=*/false, /*hasNets=*/true);
  netTree.reveal(InstancePath{},
                 NetlistTree::RevealLeaf{NetlistTree::RevealLeaf::Kind::Net, 7, std::nullopt});
  netTree.advanceReveal();
  auto req = nlohmann::json::parse(netProvider.sent.back());
  ASSERT_EQ(req["request"], "load_nets");
  NetlistTreeNode* group = netTree.getNode(req["gui_id"].get<unsigned>());
  group->createChildren();
  group->createNetNode("n", std::nullopt, std::nullopt, false, 7u);
  EXPECT_FALSE(netTree.selectionPanelFocus().has_value());
  netTree.advanceReveal();
  EXPECT_EQ(netTree.selectionPanelFocus(), NetlistTree::RevealLeaf::Kind::Net);
  ASSERT_NE(netTree.highlightedNode(), nullptr);
  EXPECT_EQ(netTree.highlightedNode()->getNetID(), 7u);

  ImGui::NewFrame();
  ImGui::Begin("Panel");
  netTree.renderSelectionPanel();
  ImGui::End();
  ImGui::Render();
  EXPECT_FALSE(netTree.selectionPanelFocus().has_value());
}

// ---------------------------------------------------------------------------
// The tree's Show menu: Pins and Nets unchecked leave the hierarchy alone.
// ---------------------------------------------------------------------------


// Hidden by default.
TEST(TreeFilter, PinsAndNetsAreHiddenByDefault) {
  EXPECT_FALSE(NetlistTree::showTermsInTree);
  EXPECT_FALSE(NetlistTree::showNetsInTree);
}

TEST_F(TreeReveal, HiddenPinsAndNetsAreNotDrawnInTheTree) {
  TreeFilterReset reset;
  NetlistTree::showTermsInTree = NetlistTree::showNetsInTree = true;
  FakeNetlistProvider netProvider;
  NetlistTree netTree{&netProvider};
  netTree.createRootNode("top", design(0), /*hasTerms=*/true, /*hasPrimitives=*/false,
                         /*hasInstances=*/true, /*hasNets=*/true);
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
    netTree.render();
    ImGui::End();
    ImGui::Render();
  };
  SelectionStore::clear();
  netTree.reveal(InstancePath{{1, "u1"}});
  netTree.advanceReveal();
  auto req = nlohmann::json::parse(netProvider.sent.back());
  ASSERT_EQ(req["request"], "load_instances");
  NetlistTreeNode* group = netTree.getNode(req["gui_id"].get<unsigned>());
  group->createChildren();
  // A cell: pins and nets only.
  group->createInstanceNode("u1", "M", 1, design(1), /*hasTerms=*/true, /*hasPrimitives=*/false,
                            /*hasInstances=*/false, /*hasNets=*/true);
  frame();
  frame();
  ASSERT_FALSE(netTree.isRevealPending());
  NetlistTreeNode* u1 = netTree.findInstance(InstancePath{{1, "u1"}});
  ASSERT_NE(u1, nullptr);
  // top, Ports, Nets, Instances, u1.
  EXPECT_EQ(netTree.rowsDrawnForTesting(), 5u);
  EXPECT_FALSE(u1->isLeaf());

  NetlistTree::showTermsInTree = false;
  frame();
  EXPECT_EQ(netTree.rowsDrawnForTesting(), 4u);  // no Ports
  EXPECT_FALSE(u1->isLeaf());
  NetlistTree::showNetsInTree = false;
  frame();
  EXPECT_EQ(netTree.rowsDrawnForTesting(), 3u);  // top, Instances, u1
  EXPECT_TRUE(u1->isLeaf());                     // nothing left under it
  EXPECT_FALSE(netTree.getRoot()->isLeaf());

  // Shown again: as before.
  NetlistTree::showTermsInTree = NetlistTree::showNetsInTree = true;
  frame();
  EXPECT_EQ(netTree.rowsDrawnForTesting(), 5u);

  SelectionStore::clear();
  ImGui::DestroyContext();
}

// A leaf reveal (review Go To a pin) with pins hidden from the tree still
// finds the row, for the selection panel, which shows them either way.
TEST_F(TreeReveal, ALeafRevealWithPinsHiddenGoesToThePanel) {
  TreeFilterReset reset;
  NetlistTree::showTermsInTree = false;
  tree.reveal(InstancePath{{11, "u1"}},
              NetlistTree::RevealLeaf{NetlistTree::RevealLeaf::Kind::Term, 5, 1});
  tree.advanceReveal();
  answer(take("load_instances")[0]["gui_id"], {{"u1", 11, true}});
  answer(take("load_primitives")[0]["gui_id"], {});
  tree.advanceReveal();
  auto terms = take("load_terms");
  ASSERT_EQ(terms.size(), 1u);
  NetlistTreeNode* group = tree.getNode(terms[0]["gui_id"].get<unsigned>());
  group->createChildren();
  group->createTermNode("d", 5, Direction::Input, 3, 0);
  tree.advanceReveal();
  EXPECT_FALSE(tree.isRevealPending());
  ASSERT_NE(tree.highlightedNode(), nullptr);
  EXPECT_EQ(tree.highlightedNode()->getBusBit(), 1);
  EXPECT_EQ(tree.selectionPanelFocus(), NetlistTree::RevealLeaf::Kind::Term);
}

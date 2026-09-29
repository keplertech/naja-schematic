// Headless ImGui tests of the schematic's pin interactions: a real ImGui
// context (no render backend), simulated mouse input, and the real
// EquipotentialView::renderSchematic() -- asserting on the requests it sends
// through a FakeNetlistProvider and on the geometry it builds.
#include "EquipotentialView.h"
#include "SchematicView.h"
#include "SelectionStore.h"

#include <imgui.h>
#include <nlohmann/json.hpp>
#include <gtest/gtest.h>

#include "FakeNetlistProvider.h"

namespace {

InstTermOccurrence occ(const std::string& inst, unsigned instId, const std::string& term,
                       unsigned termId, Direction dir, size_t bitTermCount,
                       std::optional<int> bit = std::nullopt) {
  InstTermOccurrence o;
  o.path           = {{instId, inst}};
  o.pathIds        = {instId};
  o.pathModels     = {""};
  o.term           = BitTerm{term, termId, dir, bit};
  o.bit_term_count = bitTermCount;
  return o;
}

class SchematicClicks : public ::testing::Test {
 protected:
  void SetUp() override {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1200.f, 800.f);
    io.DeltaTime   = 1.f / 60.f;
    io.IniFilename = nullptr;
    unsigned char* px; int w, h;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    EquipotentialView::setProvider(&provider);
    EquipotentialView::resetLayout();
    SelectionStore::clear();
  }

  void TearDown() override {
    EquipotentialView::setProvider(nullptr);
    EquipotentialView::resetLayout();
    SelectionStore::clear();
    for (auto* eq : eqs) delete eq;
    ImGui::DestroyContext();
  }

  void add(Equipotential eq) { eqs.push_back(new Equipotential(std::move(eq))); }

  void frame(int n = 1) {
    for (int i = 0; i < n; ++i) {
      ImGui::NewFrame();
      ImGui::SetNextWindowPos(ImVec2(0.f, 0.f));
      ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
      ImGui::Begin("Schematic", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
      EquipotentialView::renderSchematic(eqs);
      ImGui::End();
      ImGui::Render();
    }
  }

  const SchematicView& sv() const { return EquipotentialView::schematicForTesting(); }

  const InstanceShape* shape(const InstancePath& path) const {
    for (const auto& s : sv().instances)
      if (!s.isHierGroup && s.path == path) return &s;
    return nullptr;
  }

  const Port* pin(const InstancePath& inst, const std::string& name) const {
    const InstanceShape* s = shape(inst);
    if (!s) return nullptr;
    for (const auto& p : s->ports)
      if (p.name == name) return &p;
    return nullptr;
  }

  ImVec2 toScreen(ImVec2 world) const {
    return sv().worldToScreen(world, EquipotentialView::canvasOriginForTesting(), ImVec2());
  }

  // Middle of the pin's tick, where a user would aim.
  ImVec2 pinScreen(const InstancePath& inst, const std::string& name) const {
    const InstanceShape* s = shape(inst);
    const Port* p = pin(inst, name);
    EXPECT_NE(p, nullptr) << displayPath(inst) << "/" << name;
    if (!p) return ImVec2();
    ImVec2 a = portAnchor(*s, *p);
    return toScreen(ImVec2(a.x + (p->lx < 0.f ? -4.f : 4.f), a.y));
  }

  ImVec2 boxCenterScreen(const InstancePath& inst) const {
    const InstanceShape* s = shape(inst);
    EXPECT_NE(s, nullptr) << displayPath(inst);
    return s ? toScreen(ImVec2(s->x + s->w * 0.5f, s->y + s->h * 0.5f)) : ImVec2();
  }

  void moveTo(ImVec2 p) {
    ImGui::GetIO().AddMousePosEvent(p.x, p.y);
    frame();
  }
  void click() {
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    frame();
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    frame();
  }
  void doubleClick() { click(); click(); }

  std::vector<nlohmann::json> sent(const std::string& request) const {
    std::vector<nlohmann::json> out;
    for (const auto& s : provider.sent) {
      auto j = nlohmann::json::parse(s);
      if (j.value("request", "") == request) out.push_back(j);
    }
    return out;
  }

  // u1.Q -> u2.A, where u2's model has 3 bit terms (A, B, Y): u2 is drawn
  // with a partial interface, and after expansion B and Y are open pins.
  void showFirstNet() {
    Equipotential e{true, {}, {}};
    e.occurrences.push_back(occ("u1", 1, "Q", 10, Direction::Output, 1));
    e.occurrences.push_back(occ("u2", 2, "A", 20, Direction::Input, 3));
    add(e);
    frame(3);
  }
  void expandU2() {
    EquipotentialView::applyInstanceExpansion(InstancePath{{2, "u2"}}, {
        {"A", Direction::Input, 20, std::nullopt},
        {"B", Direction::Input, 21, std::nullopt},
        {"Y", Direction::Output, 22, std::nullopt},
    });
    frame(2);
  }

  FakeNetlistProvider         provider;
  std::vector<Equipotential*> eqs;
};

} // namespace

TEST_F(SchematicClicks, DoubleClickOnPartialBoxRequestsItsFullInterface) {
  showFirstNet();
  ASSERT_NE(shape({{2, "u2"}}), nullptr);
  EXPECT_TRUE(shape({{2, "u2"}})->partialInterface);

  moveTo(boxCenterScreen({{2, "u2"}}));
  doubleClick();

  auto reqs = sent("expand_instance_terms");
  ASSERT_EQ(reqs.size(), 1u);
  EXPECT_EQ(readPath(reqs[0], "instance_path", "instance_id_path"), (InstancePath{{2, "u2"}}));
}

TEST_F(SchematicClicks, PartialGateSymbolLoadsItsFullInterfaceUnasked) {
  // u2 is an AND reached through one input: with no dashed border or pin
  // names on a gate, it would look like a complete one-input AND.
  Equipotential e{true, {}, {}};
  e.occurrences.push_back(occ("u1", 1, "Q", 10, Direction::Output, 1));
  e.occurrences.push_back(occ("u2", 2, "A", 20, Direction::Input, 3));
  e.occurrences.back().primitiveType = PrimitiveType::And;
  add(e);
  frame(3);

  auto reqs = sent("expand_instance_terms");
  ASSERT_EQ(reqs.size(), 1u);  // once, not every frame
  EXPECT_EQ(readPath(reqs[0], "instance_path", "instance_id_path"), (InstancePath{{2, "u2"}}));

  expandU2();
  EXPECT_FALSE(shape({{2, "u2"}})->partialInterface);
  EXPECT_NE(pin({{2, "u2"}}, "B"), nullptr);
  EXPECT_EQ(sent("expand_instance_terms").size(), 1u);
}

TEST_F(SchematicClicks, DoubleClickOnAPinIsNotABoxDoubleClick) {
  showFirstNet();
  moveTo(pinScreen({{2, "u2"}}, "A"));
  doubleClick();
  EXPECT_TRUE(sent("expand_instance_terms").empty());
  EXPECT_TRUE(sent("load_equipotential").empty());  // A's net is already shown
}

TEST_F(SchematicClicks, ExpandedInterfaceMarksPinsNotInTheViewAsOpen) {
  showFirstNet();
  expandU2();
  ASSERT_NE(pin({{2, "u2"}}, "B"), nullptr);
  EXPECT_FALSE(shape({{2, "u2"}})->partialInterface);
  EXPECT_FALSE(pin({{2, "u2"}}, "A")->open);  // on the net already shown
  EXPECT_TRUE(pin({{2, "u2"}}, "B")->open);
  EXPECT_TRUE(pin({{2, "u2"}}, "Y")->open);
}

TEST_F(SchematicClicks, HoveringAPinHighlightsIt) {
  showFirstNet();
  expandU2();
  moveTo(pinScreen({{2, "u2"}}, "B"));
  EXPECT_EQ(sv().hoveredPortId, pin({{2, "u2"}}, "B")->id);

  moveTo(boxCenterScreen({{2, "u2"}}));
  EXPECT_EQ(sv().hoveredPortId, -1);
}

TEST_F(SchematicClicks, SingleClickOnOpenPinLoadsItsNetOnce) {
  showFirstNet();
  expandU2();
  moveTo(pinScreen({{2, "u2"}}, "B"));
  click();

  auto reqs = sent("load_equipotential");
  ASSERT_EQ(reqs.size(), 1u);
  EXPECT_EQ(reqs[0]["path"], nlohmann::json::array({2}));
  EXPECT_EQ(reqs[0]["term_id"], 21);
  EXPECT_TRUE(pin({{2, "u2"}}, "B")->pending);

  // Impatient second click (or the second half of a double-click) while
  // the net is loading: nothing more is sent.
  click();
  doubleClick();
  EXPECT_EQ(sent("load_equipotential").size(), 1u);
}

TEST_F(SchematicClicks, ClickOnAPinWhoseNetIsShownSendsNothing) {
  showFirstNet();
  expandU2();
  moveTo(pinScreen({{2, "u2"}}, "A"));
  click();
  EXPECT_TRUE(provider.sent.empty());
}

TEST_F(SchematicClicks, ArrivingNetClosesThePinAndKeepsTheClickedBoxInPlace) {
  showFirstNet();
  expandU2();
  moveTo(pinScreen({{2, "u2"}}, "B"));
  click();
  const ImVec2 before = boxCenterScreen({{2, "u2"}});
  const float scaleBefore = sv().transform.scale;

  // The reply: u9 (inside module "core", so hierarchy frames re-lay out the
  // whole drawing) drives u2.B.
  Equipotential e{true, {}, {}};
  InstTermOccurrence drv;
  drv.path = {{5, "core"}, {9, "u9"}}; drv.pathIds = {5, 9}; drv.pathModels = {"Core", ""};
  drv.term = BitTerm{"Z", 90, Direction::Output, std::nullopt};
  drv.bit_term_count = 1;
  e.occurrences.push_back(drv);
  e.occurrences.push_back(occ("u2", 2, "B", 21, Direction::Input, 3));
  add(e);
  frame(2);

  ASSERT_NE(shape({{5, "core"}, {9, "u9"}}), nullptr);
  EXPECT_FALSE(pin({{2, "u2"}}, "B")->open);
  EXPECT_FALSE(pin({{2, "u2"}}, "B")->pending);
  const ImVec2 after = boxCenterScreen({{2, "u2"}});
  EXPECT_NEAR(after.x, before.x, 0.5f);
  EXPECT_NEAR(after.y, before.y, 0.5f);
  EXPECT_EQ(sv().transform.scale, scaleBefore);  // no re-fit zoom either
}

TEST_F(SchematicClicks, ClickOnMergedBusPinShowsItsBits) {
  for (int b = 0; b < 2; ++b) {
    Equipotential e{true, {}, {}};
    e.occurrences.push_back(occ("u1", 1, "Q", 10, Direction::Output, 2, b));
    e.occurrences.push_back(occ("u2", 2, "D", 20, Direction::Input, 2, b));
    add(e);
  }
  frame(3);
  ASSERT_NE(pin({{2, "u2"}}, "D[1:0]"), nullptr);
  EXPECT_TRUE(pin({{2, "u2"}}, "D[1:0]")->isBus);

  moveTo(pinScreen({{2, "u2"}}, "D[1:0]"));
  click();
  frame();
  EXPECT_EQ(pin({{2, "u2"}}, "D[1:0]"), nullptr);
  EXPECT_NE(pin({{2, "u2"}}, "D[0]"), nullptr);
  EXPECT_NE(pin({{2, "u2"}}, "D[1]"), nullptr);
  EXPECT_TRUE(provider.sent.empty());  // all bits were already loaded
}

// The same net listed twice (e.g. a trace re-listing a net already shown)
// used to be drawn as a thick "bus" wire.
TEST_F(SchematicClicks, SameNetTwiceIsOneThinWire) {
  showFirstNet();
  Equipotential again{true, {}, {}};
  again.occurrences.push_back(occ("u1", 1, "Q", 10, Direction::Output, 1));
  again.occurrences.push_back(occ("u2", 2, "A", 20, Direction::Input, 3));
  add(again);
  frame(2);

  ASSERT_EQ(sv().nets.size(), 1u);
  EXPECT_FALSE(sv().nets[0].isBus);
}

// Only the first press of a double-click acts: double-clicking the hierarchy
// glyph used to toggle it open and straight back closed.
TEST_F(SchematicClicks, DoubleClickOnHierarchyGlyphTogglesItOnce) {
  Equipotential e{true, {}, {}};
  e.occurrences.push_back(occ("u1", 1, "Q", 10, Direction::Output, 1));
  auto sub = occ("u2", 2, "A", 20, Direction::Input, 1);
  sub.has_instances = true;
  e.occurrences.push_back(sub);
  add(e);
  frame(3);
  ASSERT_NE(shape({{2, "u2"}}), nullptr);
  ASSERT_TRUE(canShowHierToggle(*shape({{2, "u2"}})));

  float x0, y0, x1, y1;
  hierToggleGlyphRect(*shape({{2, "u2"}}), x0, y0, x1, y1);
  moveTo(toScreen(ImVec2((x0 + x1) * 0.5f, (y0 + y1) * 0.5f)));
  doubleClick();

  EXPECT_TRUE(shape({{2, "u2"}})->hierExpanded);
  EXPECT_EQ(sent("load_instance_internals").size(), 1u);
}

// ---------------------------------------------------------------------------
// Starting the view from one instance (a host's focus_instance)
// ---------------------------------------------------------------------------

namespace {

// An instance_resolved reply as protocol.py / LocalSNLProvider build it, for
// top/core/u7 (an AND2 with pins A, B, Y).
nlohmann::json resolvedU7() {
  return nlohmann::json::parse(R"({
    "response": "instance_resolved", "path": ["core", "u7"], "found": true,
    "instance": {
      "path": [["core", 5, "Core"], ["u7", 7, "AND2"]],
      "design_ref": {"db_id": 1, "library_id": 2, "design_id": 3},
      "primitive_type": "and",
      "has_instances": false, "source_loc": null,
      "terms": [{"name": "A", "child_id": 0, "direction": 0},
                {"name": "B", "child_id": 1, "direction": 0},
                {"name": "Y", "child_id": 2, "direction": 1}]
    }})");
}

} // namespace

TEST(StartInstance, ParsedFromAnInstanceResolvedReply) {
  auto start = EquipotentialView::startInstanceFromResolved(resolvedU7());
  ASSERT_TRUE(start.has_value());
  EXPECT_EQ(start->path, (InstancePath{{5, "core"}, {7, "u7"}}));
  EXPECT_EQ(pathNames(start->path), (std::vector<std::string>{"core", "u7"}));
  EXPECT_EQ(start->pathIds, (std::vector<unsigned>{5, 7}));
  EXPECT_EQ(start->pathModels, (std::vector<std::string>{"Core", "AND2"}));
  EXPECT_EQ(start->designRef.design_id, 3u);
  EXPECT_EQ(start->primitiveType, PrimitiveType::And);
  ASSERT_EQ(start->ports.size(), 3u);
  EXPECT_EQ(start->ports[2].name, "Y");
  EXPECT_EQ(start->ports[2].direction, Direction::Output);

  auto notFound = nlohmann::json::parse(R"({"response":"instance_resolved","path":["x"],"found":false})");
  EXPECT_FALSE(EquipotentialView::startInstanceFromResolved(notFound).has_value());
  auto top = nlohmann::json::parse(R"({"response":"instance_resolved","path":[],"found":true})");
  EXPECT_FALSE(EquipotentialView::startInstanceFromResolved(top).has_value());
}

TEST_F(SchematicClicks, StartInstanceIsDrawnAloneWithAllPinsOpen) {
  EquipotentialView::showInstance(*EquipotentialView::startInstanceFromResolved(resolvedU7()));
  frame(3);

  const InstanceShape* u7 = shape({{5, "core"}, {7, "u7"}});
  ASSERT_NE(u7, nullptr);
  EXPECT_FALSE(u7->partialInterface);  // its whole interface is shown
  EXPECT_EQ(u7->primitiveType, PrimitiveType::And);  // drawn as an AND gate
  for (auto name : {"A", "B", "Y"}) {
    ASSERT_NE(pin({{5, "core"}, {7, "u7"}}, name), nullptr) << name;
    EXPECT_TRUE(pin({{5, "core"}, {7, "u7"}}, name)->open) << name;
  }
  // Drawn inside its module's frame, like any traced leaf.
  bool framed = false;
  for (const auto& s : sv().instances) framed = framed || (s.isHierGroup && s.name == "core (Core)");
  EXPECT_TRUE(framed);
}

TEST_F(SchematicClicks, ClickingAStartInstancePinLoadsItsNetWithTheInstancePath) {
  EquipotentialView::showInstance(*EquipotentialView::startInstanceFromResolved(resolvedU7()));
  frame(3);
  moveTo(pinScreen({{5, "core"}, {7, "u7"}}, "Y"));
  click();

  auto reqs = sent("load_equipotential");
  ASSERT_EQ(reqs.size(), 1u);
  EXPECT_EQ(reqs[0]["path"], nlohmann::json::array({5, 7}));
  EXPECT_EQ(reqs[0]["term_id"], 2);
}

TEST_F(SchematicClicks, ResetLayoutDropsTheStartInstance) {
  EquipotentialView::showInstance(*EquipotentialView::startInstanceFromResolved(resolvedU7()));
  frame(2);
  ASSERT_NE(shape({{5, "core"}, {7, "u7"}}), nullptr);
  EquipotentialView::resetLayout();
  frame(2);
  EXPECT_EQ(shape({{5, "core"}, {7, "u7"}}), nullptr);
}

// An escaped instance name can contain '/': the top-level instance "a/b" and
// instance b inside a are two boxes, and each click reports its own path.
TEST_F(SchematicClicks, NamesContainingSlashesStayOneLevel) {
  Equipotential e{true, {}, {}};
  InstTermOccurrence drv = occ("a/b", 1, "Q", 10, Direction::Output, 1);
  InstTermOccurrence rcv = occ("a", 2, "A", 20, Direction::Input, 3);
  rcv.path       = {{2, "a"}, {3, "b"}};
  rcv.pathIds    = {2, 3};
  rcv.pathModels = {"", ""};
  e.occurrences  = {drv, rcv};
  add(e);
  frame(3);

  const InstanceShape* flat   = shape({{1, "a/b"}});
  const InstanceShape* nested = shape({{2, "a"}, {3, "b"}});
  ASSERT_NE(flat, nullptr);
  ASSERT_NE(nested, nullptr);
  EXPECT_NE(flat->id, nested->id);

  moveTo(boxCenterScreen(InstancePath{{1, "a/b"}}));
  click();
  EXPECT_TRUE(SelectionStore::isSelected(InstancePath{{1, "a/b"}}));

  moveTo(toScreen(ImVec2(nested->x + nested->w * 0.5f, nested->y + nested->h * 0.5f)));
  doubleClick();
  auto reqs = sent("expand_instance_terms");
  ASSERT_EQ(reqs.size(), 1u);
  EXPECT_EQ(readPath(reqs[0], "instance_path", "instance_id_path"), (InstancePath{{2, "a"}, {3, "b"}}));
  EXPECT_TRUE(SelectionStore::isSelected(InstancePath{{2, "a"}, {3, "b"}}));
}

// Anonymous instances all have the name "": two anonymous siblings are two
// boxes, each click selects its own, and requests about one carry its ids.
TEST_F(SchematicClicks, AnonymousSiblingsAreSeparateBoxes) {
  Equipotential e{true, {}, {}};
  e.occurrences.push_back(occ("", 4, "Q", 10, Direction::Output, 1));
  e.occurrences.push_back(occ("", 5, "A", 20, Direction::Input, 3));
  add(e);
  frame(3);

  const InstanceShape* a4 = shape({{4, ""}});
  const InstanceShape* a5 = shape({{5, ""}});
  ASSERT_NE(a4, nullptr);
  ASSERT_NE(a5, nullptr);
  EXPECT_NE(a4->id, a5->id);
  EXPECT_EQ(a5->name, "<#5>");

  moveTo(boxCenterScreen({{5, ""}}));
  click();
  EXPECT_TRUE(SelectionStore::isSelected(InstancePath{{5, ""}}));
  EXPECT_FALSE(SelectionStore::isSelected(InstancePath{{4, ""}}));

  doubleClick();  // a5 shows 1 of its 3 pins
  auto reqs = sent("expand_instance_terms");
  ASSERT_EQ(reqs.size(), 1u);
  EXPECT_EQ(reqs[0]["instance_id_path"], nlohmann::json::array({5}));
  EXPECT_EQ(reqs[0]["instance_path"], nlohmann::json::array({""}));

  // The reply, tagged with the echoed ids, expands a5 and not a4.
  EquipotentialView::applyInstanceExpansion(
      *readPath(reqs[0], "instance_path", "instance_id_path"),
      {{"A", Direction::Input, 20, std::nullopt},
       {"B", Direction::Input, 21, std::nullopt},
       {"Y", Direction::Output, 22, std::nullopt}});
  frame(2);
  EXPECT_NE(pin({{5, ""}}, "B"), nullptr);
  EXPECT_EQ(pin({{4, ""}}, "B"), nullptr);
}

TEST_F(SchematicClicks, ClickingABoxSelectsItAndDrawsItSelected) {
  showFirstNet();
  moveTo(boxCenterScreen({{2, "u2"}}));
  click();
  EXPECT_TRUE(SelectionStore::isSelected(InstancePath{{2, "u2"}}));
  EXPECT_EQ(SelectionStore::origin(), SelectionStore::Origin::Schematic);
  frame();
  EXPECT_TRUE(shape({{2, "u2"}})->selected);
  EXPECT_FALSE(shape({{1, "u1"}})->selected);
}

TEST_F(SchematicClicks, ClickingAPinDoesNotChangeTheSelection) {
  showFirstNet();
  expandU2();
  moveTo(pinScreen({{2, "u2"}}, "B"));
  click();
  EXPECT_FALSE(SelectionStore::hasSelection());
}

TEST_F(SchematicClicks, ASelectionMadeElsewhereIsDrawnInTheSchematic) {
  showFirstNet();
  SelectionStore::select(InstancePath{{1, "u1"}}, SelectionStore::Origin::Host);
  frame();
  EXPECT_TRUE(shape({{1, "u1"}})->selected);
}

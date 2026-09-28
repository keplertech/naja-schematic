// Headless ImGui tests of the schematic's pin interactions: a real ImGui
// context (no render backend), simulated mouse input, and the real
// EquipotentialView::renderSchematic() -- asserting on the requests it sends
// through a FakeNetlistProvider and on the geometry it builds.
#include "EquipotentialView.h"
#include "SchematicView.h"

#include <imgui.h>
#include <nlohmann/json.hpp>
#include <gtest/gtest.h>

#include "FakeNetlistProvider.h"

namespace {

InstTermOccurrence occ(const std::string& inst, unsigned instId, const std::string& term,
                       unsigned termId, Direction dir, size_t bitTermCount,
                       std::optional<int> bit = std::nullopt) {
  InstTermOccurrence o;
  o.path           = {inst};
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
  }

  void TearDown() override {
    EquipotentialView::setProvider(nullptr);
    EquipotentialView::resetLayout();
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

  const InstanceShape* shape(const std::string& name) const {
    for (const auto& s : sv().instances)
      if (!s.isHierGroup && s.name == name) return &s;
    return nullptr;
  }

  const Port* pin(const std::string& inst, const std::string& name) const {
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
  ImVec2 pinScreen(const std::string& inst, const std::string& name) const {
    const InstanceShape* s = shape(inst);
    const Port* p = pin(inst, name);
    EXPECT_NE(p, nullptr) << inst << "/" << name;
    if (!p) return ImVec2();
    ImVec2 a = portAnchor(*s, *p);
    return toScreen(ImVec2(a.x + (p->lx < 0.f ? -4.f : 4.f), a.y));
  }

  ImVec2 boxCenterScreen(const std::string& inst) const {
    const InstanceShape* s = shape(inst);
    EXPECT_NE(s, nullptr) << inst;
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
    EquipotentialView::applyInstanceExpansion("u2", {
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
  ASSERT_NE(shape("u2"), nullptr);
  EXPECT_TRUE(shape("u2")->partialInterface);

  moveTo(boxCenterScreen("u2"));
  doubleClick();

  auto reqs = sent("expand_instance_terms");
  ASSERT_EQ(reqs.size(), 1u);
  EXPECT_EQ(reqs[0]["path_key"], "u2");
}

TEST_F(SchematicClicks, DoubleClickOnAPinIsNotABoxDoubleClick) {
  showFirstNet();
  moveTo(pinScreen("u2", "A"));
  doubleClick();
  EXPECT_TRUE(sent("expand_instance_terms").empty());
  EXPECT_TRUE(sent("load_equipotential").empty());  // A's net is already shown
}

TEST_F(SchematicClicks, ExpandedInterfaceMarksPinsNotInTheViewAsOpen) {
  showFirstNet();
  expandU2();
  ASSERT_NE(pin("u2", "B"), nullptr);
  EXPECT_FALSE(shape("u2")->partialInterface);
  EXPECT_FALSE(pin("u2", "A")->open);  // on the net already shown
  EXPECT_TRUE(pin("u2", "B")->open);
  EXPECT_TRUE(pin("u2", "Y")->open);
}

TEST_F(SchematicClicks, HoveringAPinHighlightsIt) {
  showFirstNet();
  expandU2();
  moveTo(pinScreen("u2", "B"));
  EXPECT_EQ(sv().hoveredPortId, pin("u2", "B")->id);

  moveTo(boxCenterScreen("u2"));
  EXPECT_EQ(sv().hoveredPortId, -1);
}

TEST_F(SchematicClicks, SingleClickOnOpenPinLoadsItsNetOnce) {
  showFirstNet();
  expandU2();
  moveTo(pinScreen("u2", "B"));
  click();

  auto reqs = sent("load_equipotential");
  ASSERT_EQ(reqs.size(), 1u);
  EXPECT_EQ(reqs[0]["path"], nlohmann::json::array({2}));
  EXPECT_EQ(reqs[0]["term_id"], 21);
  EXPECT_TRUE(pin("u2", "B")->pending);

  // Impatient second click (or the second half of a double-click) while
  // the net is loading: nothing more is sent.
  click();
  doubleClick();
  EXPECT_EQ(sent("load_equipotential").size(), 1u);
}

TEST_F(SchematicClicks, ClickOnAPinWhoseNetIsShownSendsNothing) {
  showFirstNet();
  expandU2();
  moveTo(pinScreen("u2", "A"));
  click();
  EXPECT_TRUE(provider.sent.empty());
}

TEST_F(SchematicClicks, ArrivingNetClosesThePinAndKeepsTheClickedBoxInPlace) {
  showFirstNet();
  expandU2();
  moveTo(pinScreen("u2", "B"));
  click();
  const ImVec2 before = boxCenterScreen("u2");
  const float scaleBefore = sv().transform.scale;

  // The reply: u9 (inside module "core", so hierarchy frames re-lay out the
  // whole drawing) drives u2.B.
  Equipotential e{true, {}, {}};
  InstTermOccurrence drv;
  drv.path = {"core", "u9"}; drv.pathIds = {5, 9}; drv.pathModels = {"Core", ""};
  drv.term = BitTerm{"Z", 90, Direction::Output, std::nullopt};
  drv.bit_term_count = 1;
  e.occurrences.push_back(drv);
  e.occurrences.push_back(occ("u2", 2, "B", 21, Direction::Input, 3));
  add(e);
  frame(2);

  ASSERT_NE(shape("core/u9"), nullptr);
  EXPECT_FALSE(pin("u2", "B")->open);
  EXPECT_FALSE(pin("u2", "B")->pending);
  const ImVec2 after = boxCenterScreen("u2");
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
  ASSERT_NE(pin("u2", "D[1:0]"), nullptr);
  EXPECT_TRUE(pin("u2", "D[1:0]")->isBus);

  moveTo(pinScreen("u2", "D[1:0]"));
  click();
  frame();
  EXPECT_EQ(pin("u2", "D[1:0]"), nullptr);
  EXPECT_NE(pin("u2", "D[0]"), nullptr);
  EXPECT_NE(pin("u2", "D[1]"), nullptr);
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
  ASSERT_NE(shape("u2"), nullptr);
  ASSERT_TRUE(canShowHierToggle(*shape("u2")));

  float x0, y0, x1, y1;
  hierToggleGlyphRect(*shape("u2"), x0, y0, x1, y1);
  moveTo(toScreen(ImVec2((x0 + x1) * 0.5f, (y0 + y1) * 0.5f)));
  doubleClick();

  EXPECT_TRUE(shape("u2")->hierExpanded);
  EXPECT_EQ(sent("load_instance_internals").size(), 1u);
}

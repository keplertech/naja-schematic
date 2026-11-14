// StyleStore: parsing a style_response, matching items to instances, pins
// and nets, and the styling reaching the schematic and its SVG export.
#include "StyleStore.h"
#include "DiagnosisStore.h"
#include "EquipotentialView.h"
#include "SchematicView.h"
#include "SelectionStore.h"
#include "TraceStore.h"

#include <imgui.h>
#include <nlohmann/json.hpp>
#include <gtest/gtest.h>

#include "FakeNetlistProvider.h"

using nlohmann::json;

namespace {

InstTermOccurrence occ(const std::string& inst, unsigned instId, const std::string& term,
                       unsigned termId, Direction dir, std::optional<int> bit = std::nullopt) {
  InstTermOccurrence o;
  o.path           = {{instId, inst}};
  o.pathIds        = {instId};
  o.pathModels     = {""};
  o.term           = BitTerm{term, termId, dir, bit};
  o.bit_term_count = 3;
  return o;
}

InstancePath u(unsigned id) { return {{id, "u" + std::to_string(id)}}; }

class StyleStoreTest : public ::testing::Test {
 protected:
  void TearDown() override { StyleStore::clear(); }
  void set(const json& items) {
    std::vector<std::string> errors;
    StyleStore::setStyles(StyleStore::parse(json{{"items", items}}, &errors));
    EXPECT_TRUE(errors.empty()) << errors.front();
  }
};

} // namespace

TEST_F(StyleStoreTest, ParsesColorsWithAndWithoutAlpha) {
  EXPECT_EQ(StyleStore::parseColor("#ff8000"), IM_COL32(255, 128, 0, 255));
  EXPECT_EQ(StyleStore::parseColor("#FF800040"), IM_COL32(255, 128, 0, 64));
  EXPECT_FALSE(StyleStore::parseColor("ff8000"));
  EXPECT_FALSE(StyleStore::parseColor("#ff80"));
  EXPECT_FALSE(StyleStore::parseColor("#gg8000"));
}

TEST_F(StyleStoreTest, MalformedItemsAreSkippedAndReported) {
  std::vector<std::string> errors;
  auto items = StyleStore::parse(json::array({
      {{"kind", "net"}, {"path", json::array()}},                         // no terminal
      {{"kind", "instance"}, {"path", {"u1"}}, {"color", "red"}},         // bad color
      {{"kind", "wire"}, {"path", {"u1"}}},                               // unknown kind
      {{"kind", "instance"}, {"path", {""}}},                             // anonymous by name
      {{"kind", "instance"}, {"id_path", {1}}, {"fill", "#00ff0080"}},    // fine
  }), &errors);
  EXPECT_EQ(items.size(), 1u);
  EXPECT_EQ(errors.size(), 4u);
}

TEST_F(StyleStoreTest, InstancesMatchByIdPathElseByNamesAndMergeInOrder) {
  set(json::array({
      {{"kind", "instance"}, {"path", {"u1"}}, {"fill", "#112233"}, {"label", "first"}},
      {{"kind", "instance"}, {"id_path", {1}}, {"label", "second"}, {"width", 4}},
      {{"kind", "instance"}, {"id_path", {7}}, {"path", {"u1"}}, {"color", "#ff0000"}},
  }));
  const StyleAttrs s = StyleStore::instanceStyle(u(1));
  EXPECT_EQ(s.fill, IM_COL32(0x11, 0x22, 0x33, 255));
  EXPECT_EQ(s.label, "second");
  EXPECT_EQ(s.width, 4.0f);
  EXPECT_EQ(s.color, 0u);  // its id_path names another instance
  EXPECT_TRUE(StyleStore::instanceStyle(u(2)).empty());
}

TEST_F(StyleStoreTest, PinsMatchByTermIdOrBaseNameAndBit) {
  set(json::array({
      {{"kind", "term"}, {"id_path", {1}}, {"terminal", "D"}, {"color", "#0000ff"}},
      {{"kind", "term"}, {"id_path", {1}}, {"terminal", "D[2]"}, {"bit", 2}, {"width", 3}},
      {{"kind", "term"}, {"id_path", {1}}, {"term_id", 5}, {"label", "by id"}},
  }));
  EXPECT_EQ(StyleStore::termStyle(u(1), "D[0]", 4, 0).color, IM_COL32(0, 0, 255, 255));
  EXPECT_EQ(StyleStore::termStyle(u(1), "D[0]", 4, 0).width, 0.0f);
  EXPECT_EQ(StyleStore::termStyle(u(1), "D[2]", 4, 2).width, 3.0f);
  // A merged bus pin: only the items without a bit.
  EXPECT_EQ(StyleStore::termStyle(u(1), "D[3:0]", 4, std::nullopt).width, 0.0f);
  EXPECT_EQ(StyleStore::termStyle(u(1), "", 5, std::nullopt).label, "by id");
  EXPECT_TRUE(StyleStore::termStyle(u(2), "D[0]", 4, 0).empty());
}

TEST_F(StyleStoreTest, ANetIsStyledThroughAnyOfItsPins) {
  set(json::array({
      {{"kind", "net"}, {"path", json::array()}, {"terminal", "en"}, {"color", "#ff8800"}},
      {{"kind", "net"}, {"id_path", {3}}, {"terminal", "Y"}, {"color", "#0000ff"}, {"width", 3}},
  }));
  Equipotential en{true, {}, {}};
  en.terms.push_back(BitTerm{"en", 0, Direction::Input});
  en.occurrences.push_back(occ("u1", 1, "E", 2, Direction::Input));
  EXPECT_EQ(StyleStore::netStyle(en).color, IM_COL32(0xff, 0x88, 0, 255));

  Equipotential y{true, {}, {}};
  y.occurrences.push_back(occ("u3", 3, "Y", 9, Direction::Output));
  y.occurrences.push_back(occ("u4", 4, "A", 0, Direction::Input));
  EXPECT_EQ(StyleStore::netStyle(y).color, IM_COL32(0, 0, 0xff, 255));
  EXPECT_EQ(StyleStore::netStyle(y).width, 3.0f);

  Equipotential other{true, {}, {}};
  other.occurrences.push_back(occ("u4", 4, "Y", 9, Direction::Output));
  EXPECT_TRUE(StyleStore::netStyle(other).empty());
}

// --- In the schematic and its SVG export --------------------------------

namespace {
class StyledSchematic : public ::testing::Test {
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
    // u0.Y drives u1.A and u2.A; u1.Y drives u3.A.
    auto net = [](std::vector<InstTermOccurrence> ends) {
      Equipotential e{true, {}, {}};
      e.occurrences = std::move(ends);
      return new Equipotential(std::move(e));
    };
    eqs.push_back(net({occ("u0", 0, "Y", 9, Direction::Output), occ("u1", 1, "A", 0, Direction::Input),
                       occ("u2", 2, "A", 0, Direction::Input)}));
    eqs.push_back(net({occ("u1", 1, "Y", 9, Direction::Output), occ("u3", 3, "A", 0, Direction::Input)}));
  }
  void TearDown() override {
    EquipotentialView::setProvider(nullptr);
    EquipotentialView::resetLayout();
    StyleStore::clear();
    DiagnosisStore::clear();
    TraceStore::clear();
    for (auto* eq : eqs) delete eq;
    ImGui::DestroyContext();
  }
  void frame(int n = 3) {
    for (int i = 0; i < n; ++i) {
      unsigned char* px; int w, h;
      ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
      ImGui::NewFrame();
      ImGui::SetNextWindowPos(ImVec2(0.f, 0.f));
      ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
      ImGui::Begin("Schematic", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
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
  // The routed tree whose wires reach the pin `pin` of u<id>.
  const SchematicView::NetRoute* routeAt(unsigned id, const std::string& pin) const {
    const InstanceShape* s = shape(u(id));
    if (!s) return nullptr;
    for (const auto& p : s->ports) {
      if (p.name != pin) continue;
      ImVec2 at = portAnchor(*s, p);
      for (const auto& r : sv().routes)
        for (const auto& seg : r.segments)
          for (ImVec2 q : {seg.a, seg.b})
            if (std::abs(q.x - at.x) < 0.5f && std::abs(q.y - at.y) < 0.5f) return &r;
    }
    return nullptr;
  }
  void style(const json& items) { StyleStore::setStyles(StyleStore::parse(items)); }

  FakeNetlistProvider provider;
  std::vector<Equipotential*> eqs;
};
} // namespace

TEST_F(StyledSchematic, NetsGetColorWidthAndLabelInstancesAFill) {
  style(json::array({
      {{"kind", "net"}, {"id_path", {2}}, {"terminal", "A"}, {"color", "#1f77b4"}, {"width", 5},
       {"label", "copy 1"}},
      {{"kind", "instance"}, {"id_path", {3}}, {"fill", "#2ca02c"}, {"label", "sink"}},
      {{"kind", "term"}, {"id_path", {1}}, {"terminal", "Y"}, {"color", "#9467bd"}},
  }));
  frame();
  const auto* r = routeAt(0, "Y");
  ASSERT_NE(r, nullptr);
  EXPECT_EQ(r->color, IM_COL32(0x1f, 0x77, 0xb4, 255));
  EXPECT_EQ(r->width, 5.0f);
  EXPECT_EQ(r->label, "copy 1");
  ASSERT_NE(routeAt(1, "Y"), nullptr);
  EXPECT_EQ(routeAt(1, "Y")->color, NetWire{}.color);
  EXPECT_EQ(shape(u(3))->fill, IM_COL32(0x2c, 0xa0, 0x2c, 255));
  EXPECT_EQ(shape(u(3))->label, "sink");
  for (const auto& p : shape(u(1))->ports)
    EXPECT_EQ(p.styleColor, p.name == "Y" ? IM_COL32(0x94, 0x67, 0xbd, 255) : 0u);

  const std::string svg = EquipotentialView::exportSvg();
  EXPECT_NE(svg.find("stroke=\"rgb(31,119,180)\" stroke-width=\"5\""), std::string::npos);
  EXPECT_NE(svg.find("fill=\"rgb(44,160,44)\""), std::string::npos);
  EXPECT_NE(svg.find(">copy 1</text>"), std::string::npos);
  EXPECT_NE(svg.find(">sink</text>"), std::string::npos);
}

// A translucent fill tints the default paper fill (still opaque, so it
// hides the wires under the box).
TEST_F(StyledSchematic, ATranslucentFillTintsTheBox) {
  style(json::array({{{"kind", "instance"}, {"id_path", {3}}, {"fill", "#ff000080"}}}));
  frame();
  EXPECT_NE(EquipotentialView::exportSvg().find("fill=\"rgb(235,108,110)\""), std::string::npos);
}

// Precedence: diagnosis > style > traces.
TEST_F(StyledSchematic, AStyleColorBeatsATraceAndLosesToADiagnosis) {
  int t = TraceStore::begin("u1/A");
  eqs[0]->traceIds = {t};
  style(json::array({{{"kind", "net"}, {"id_path", {0}}, {"terminal", "Y"}, {"color", "#ff8800"}}}));
  frame();
  ASSERT_NE(routeAt(0, "Y"), nullptr);
  EXPECT_EQ(routeAt(0, "Y")->color, IM_COL32(0xff, 0x88, 0, 255));

  DiagnosisItem d;
  d.kind = DiagnosisKind::Net;
  d.idPath = std::vector<unsigned>{0};
  d.terminal = "Y";
  d.severity = DiagnosisSeverity::Error;
  DiagnosisStore::setDiagnostics({d});
  frame();
  // The diagnosis UI may be switched off (kDiagnosisUIHidden): then no
  // diagnosis color reaches the pin, and the style's stays.
  const ImU32 diag = DiagnosisStore::netColor(u(0), "Y");
  EXPECT_EQ(routeAt(0, "Y")->color, diag != 0 ? diag : IM_COL32(0xff, 0x88, 0, 255));
}

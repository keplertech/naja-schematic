// Drawing options (ViewOptions): instance labels (name / model / both), the
// sink grid of layeredPlacement(), and fanout summaries -- the planning
// (SchematicLayout::planFanout) and, end to end through renderSchematic(),
// the summary boxes and a click expanding one.
#include "EquipotentialView.h"
#include "SchematicLayout.h"
#include "SchematicView.h"
#include "SelectionStore.h"
#include "StyleStore.h"
#include "TraceStore.h"
#include "ViewOptions.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include <imgui.h>
#include <gtest/gtest.h>

#include "FakeNetlistProvider.h"

using namespace SchematicLayout;

namespace {

float charWidth(const std::string& s) { return 7.f * float(s.size()); }

Item sinkItem(std::vector<std::pair<unsigned, std::string>> path, const std::string& model,
              const std::string& pin, bool clock = false, const std::string& role = "") {
  Item it;
  it.label     = pin;
  it.direction = Direction::Input;
  it.clock     = clock;
  it.role      = role;
  for (const auto& [id, name] : path) {
    it.path.push_back({ id, name });
    it.pathModels.push_back("M");
  }
  it.pathModels.back() = model;
  return it;
}

Item driverItem(unsigned id, const std::string& name) {
  Item it = sinkItem({ { id, name } }, "BUF", "O");
  it.direction = Direction::Output;
  return it;
}

FanoutNet net(Item driver, int dffs, int ands, unsigned firstId = 100) {
  FanoutNet n;
  n.drivers.push_back(std::move(driver));
  for (int i = 0; i < dffs; ++i)
    n.receivers.push_back(sinkItem({ { firstId + unsigned(i), "d" + std::to_string(i) } }, "DFF", "D"));
  for (int i = 0; i < ands; ++i)
    n.receivers.push_back(sinkItem({ { firstId + 500 + unsigned(i), "a" + std::to_string(i) } }, "AND2", "A"));
  return n;
}

} // namespace

// ---------------------------------------------------------------------------
// ViewOptions
// ---------------------------------------------------------------------------
TEST(ViewOptions, AppliesKnownKeysAndRejectsBadValues) {
  ViewOptions o;
  std::string error;
  EXPECT_TRUE(o.apply({ { "instance_label", "both" }, { "max_sinks", 5 } }, error));
  EXPECT_EQ(o.instanceLabel, InstanceLabel::Both);
  EXPECT_EQ(o.maxSinks, 5);
  EXPECT_EQ(o.sinkRows, 8);  // untouched
  EXPECT_FALSE(o.apply({ { "group_by", "role" }, { "sink_rows", -1 } }, error));
  EXPECT_NE(error.find("sink_rows"), std::string::npos);
  EXPECT_EQ(o.groupBy, FanoutGroupBy::Model);  // nothing applied
  EXPECT_FALSE(o.apply({ { "instance_label", "cell" } }, error));
  EXPECT_TRUE(o.apply({ { "group_by", "hierarchy" } }, error));
  EXPECT_EQ(o.toJson()["group_by"], "hierarchy");
  EXPECT_TRUE(o.apply({ { "group_by", "role" } }, error));
  EXPECT_EQ(o.groupBy, FanoutGroupBy::Role);
  EXPECT_TRUE(o.apply({ { "group_by", "pin" } }, error));
  EXPECT_EQ(o.toJson()["group_by"], "pin");
}

TEST(ViewOptions, ExportDecorationsAreKeysButNotLayout) {
  ViewOptions o;
  std::string error;
  EXPECT_TRUE(o.apply({ { "title", "T" }, { "caption", "C" }, { "legend", true }, { "hide_ui_glyphs", true } },
                      error));
  EXPECT_EQ(o.title, "T");
  EXPECT_EQ(o.caption, "C");
  EXPECT_TRUE(o.legend);
  EXPECT_TRUE(o.hideUiGlyphs);
  EXPECT_EQ(o.toJson()["title"], "T");
  EXPECT_FALSE(o.layoutJson().contains("title"));
  EXPECT_EQ(o.layoutJson(), ViewOptions{}.layoutJson());
  EXPECT_FALSE(o.apply({ { "legend", "yes" } }, error));
  EXPECT_NE(error.find("legend"), std::string::npos);
  EXPECT_FALSE(o.apply({ { "title", 3 } }, error));
  EXPECT_EQ(o.title, "T");
}

// ---------------------------------------------------------------------------
// group_by "role": naja's term role of the receiving pin, not its name
// ---------------------------------------------------------------------------
TEST(PlanFanout, GroupsByTheReceivingPinsRole) {
  // Flops whose pins are named nothing like their role, on clock, reset
  // (async and sync), enable and data pins; LUT inputs with no role.
  FanoutNet n;
  n.drivers.push_back(driverItem(1, "drv"));
  unsigned id = 100;
  auto add = [&](int count, const std::string& model, const std::string& pin, bool clock,
                 const std::string& role) {
    for (int i = 0; i < count; ++i, ++id)
      n.receivers.push_back(sinkItem({ { id, "s" + std::to_string(id) } }, model, pin, clock, role));
  };
  add(6, "FF", "CK", true, "clock");
  add(2, "FF", "XC", true, "");             // modeled as a clock, no role set: still a clock
  add(3, "FF", "R", false, "async_reset");
  add(2, "FF", "SR", false, "sync_reset");
  add(4, "FF", "L", false, "enable");
  add(3, "FF", "D", false, "");             // DataInput: no role sent
  add(5, "LUT", "I1", false, "");
  add(2, "LUT", "I2", false, "");
  const std::vector<FanoutNet> nets{ n };

  FanoutPlan plan = planFanout(nets, 1, FanoutGroupBy::Role, {}, nullptr);
  std::map<std::string, const FanoutGroup*> byLabel;
  for (const auto& g : plan.groups) byLabel[g.label] = &g;
  // The first sink (a CK pin) is drawn; the 26 others gathered in 4 groups.
  ASSERT_EQ(plan.groups.size(), 4u);
  ASSERT_TRUE(byLabel.count("clock \xC3\x97" "7"));
  ASSERT_TRUE(byLabel.count("reset \xC3\x97" "5"));
  ASSERT_TRUE(byLabel.count("enable \xC3\x97" "4"));
  ASSERT_TRUE(byLabel.count("data \xC3\x97" "10"));
  EXPECT_TRUE(byLabel["clock \xC3\x97" "7"]->clock);
  EXPECT_EQ(byLabel["clock \xC3\x97" "7"]->pin, "clock");  // CK and XC
  EXPECT_EQ(byLabel["reset \xC3\x97" "5"]->pin, "reset");  // R and SR
  EXPECT_EQ(byLabel["enable \xC3\x97" "4"]->pin, "L");     // they all share it
  EXPECT_FALSE(byLabel["data \xC3\x97" "10"]->clock);

  // "pin": the same sinks by name, the clock pins together.
  plan = planFanout(nets, 1, FanoutGroupBy::Pin, {}, nullptr);
  std::set<std::string> labels;
  for (const auto& g : plan.groups) labels.insert(g.label);
  EXPECT_EQ(labels, (std::set<std::string>{ "clock \xC3\x97" "7", "R \xC3\x97" "3",
                                            "SR \xC3\x97" "2", "L \xC3\x97" "4",
                                            "D \xC3\x97" "3", "I1 \xC3\x97" "5",
                                            "I2 \xC3\x97" "2" }));
}

TEST(PlanFanout, RoleGroupOfEveryNajaRole) {
  auto group = [](const std::string& role, bool clock = false) {
    return fanoutRoleGroup(sinkItem({ { 1, "u" } }, "M", "P", clock, role));
  };
  EXPECT_EQ(group("clock"), "clock");
  EXPECT_EQ(group("", true), "clock");
  EXPECT_EQ(group("async_reset"), "reset");
  EXPECT_EQ(group("sync_reset"), "reset");
  EXPECT_EQ(group("async_set"), "set");
  EXPECT_EQ(group("sync_set"), "set");
  EXPECT_EQ(group("enable"), "enable");
  EXPECT_EQ(group("memory_write_enable"), "enable");
  EXPECT_EQ(group("scan_input"), "scan");
  EXPECT_EQ(group("scan_enable"), "scan");
  EXPECT_EQ(group("memory_read_address"), "address");
  EXPECT_EQ(group("memory_write_address"), "address");
  EXPECT_EQ(group("memory_write_data"), "data");
  EXPECT_EQ(group(""), "data");
  EXPECT_EQ(group("something_new"), "data");
}

// ---------------------------------------------------------------------------
// Instance labels: the model row under the symbol
// ---------------------------------------------------------------------------
TEST(InstanceLabels, ModelRowIsPartOfTheFootprint) {
  InstanceShape s;
  s.name = "u1"; s.model = "A_VERY_LONG_CELL_NAME";
  s.x = 10.f; s.y = 50.f; s.w = 60.f; s.h = 40.f;
  EXPECT_EQ(shownModel(s, charWidth), "");
  EXPECT_FLOAT_EQ(footprintHeight(s, charWidth), 40.f);
  EXPECT_FALSE(modelRect(s, charWidth));

  s.showModel = true;
  EXPECT_EQ(shownModel(s, charWidth), s.model);
  EXPECT_FLOAT_EQ(footprintHeight(s, charWidth), 40.f + kNameH);
  EXPECT_FLOAT_EQ(footprintWidth(s, charWidth), charWidth(s.model));
  auto r = modelRect(s, charWidth);
  ASSERT_TRUE(r);
  EXPECT_FLOAT_EQ(r->y0, 90.f);
  EXPECT_FLOAT_EQ(r->y1, 90.f + kNameH);
  EXPECT_EQ(shownName(s, charWidth), "u1");

  s.hideName = true;  // "model" alone
  EXPECT_EQ(shownName(s, charWidth), "");
  EXPECT_FALSE(nameRect(s, charWidth));
}

// ---------------------------------------------------------------------------
// Sink grid
// ---------------------------------------------------------------------------
TEST(SinkGrid, WrapsAColumnOfSinksIntoBalancedColumns) {
  // Node 0 drives 12 sinks (1..12) and a buffer (13) that drives node 14.
  std::vector<PlaceNode> nodes(15, PlaceNode{ 60.f, 40.f });
  PlaceNet fan;
  fan.drivers.push_back({ 0, 20.f });
  for (int v = 1; v <= 13; ++v) fan.receivers.push_back({ v, 20.f });
  PlaceNet onward;
  onward.drivers.push_back({ 13, 20.f });
  onward.receivers.push_back({ 14, 20.f });

  const Placement flat = layeredPlacement(nodes, { fan, onward });
  for (int v = 1; v <= 13; ++v) EXPECT_EQ(flat.level[v], 1);

  const Placement grid = layeredPlacement(nodes, { fan, onward }, 5);
  // 12 sinks into 3 columns of 4, then the buffer's column, then node 14.
  std::map<int, int> perLevel;
  for (int v = 1; v <= 12; ++v) ++perLevel[grid.level[v]];
  EXPECT_EQ(perLevel, (std::map<int, int>{ { 1, 4 }, { 2, 4 }, { 3, 4 } }));
  EXPECT_EQ(grid.level[13], 4);
  EXPECT_EQ(grid.level[14], 5);
  // Columns left to right; the grid is much shorter than the stack.
  EXPECT_LT(grid.pos[1].x, grid.pos[5].x);
  auto height = [&](const Placement& p) {
    float lo = 1e9f, hi = -1e9f;
    for (int v = 1; v <= 12; ++v) { lo = std::min(lo, p.pos[v].y); hi = std::max(hi, p.pos[v].y + 40.f); }
    return hi - lo;
  };
  EXPECT_LT(height(grid), height(flat) * 0.5f);
  // A column within the limit is left alone.
  const Placement loose = layeredPlacement(nodes, { fan, onward }, 13);
  for (int v = 1; v <= 13; ++v) EXPECT_EQ(loose.level[v], 1);
}

// ---------------------------------------------------------------------------
// planFanout
// ---------------------------------------------------------------------------
TEST(PlanFanout, DrawsTheFirstSinksAndGathersTheRestByModel) {
  const std::vector<FanoutNet> nets{ net(driverItem(1, "drv"), 20, 10) };
  const FanoutPlan plan = planFanout(nets, 8, FanoutGroupBy::Model, {}, nullptr);
  int drawn = 0;
  for (int g : plan.groupOf[0]) drawn += g < 0;
  EXPECT_EQ(drawn, 8);
  ASSERT_EQ(plan.groups.size(), 2u);
  EXPECT_EQ(plan.groups[0].key.text, "AND2");
  EXPECT_EQ(plan.groups[0].count, 10u);
  EXPECT_EQ(plan.groups[0].pin, "A");
  EXPECT_EQ(plan.groups[0].label, "AND2 \xC3\x97" "10");
  EXPECT_EQ(plan.groups[1].key.text, "DFF");
  EXPECT_EQ(plan.groups[1].count, 12u);
  for (int i = 0; i < 8; ++i) EXPECT_EQ(plan.groupOf[0][size_t(i)], -1) << i;  // the first ones

  // No limit, or few enough sinks: nothing gathered.
  EXPECT_TRUE(planFanout(nets, 0, FanoutGroupBy::Model, {}, nullptr).groups.empty());
  EXPECT_TRUE(planFanout(nets, 30, FanoutGroupBy::Model, {}, nullptr).groups.empty());
}

TEST(PlanFanout, NeverGathersADriverAPinnedOrAnExpandedSink) {
  std::vector<FanoutNet> nets{ net(driverItem(1, "drv"), 20, 0) };
  // d15 drives a net of its own.
  nets.push_back(FanoutNet{ { nets[0].receivers[15] }, { sinkItem({ { 900, "z" } }, "Z", "I") } });
  nets[1].drivers[0].direction = Direction::Output;
  const InstancePath pinned{ { 117, "d17" } };
  FanoutPlan plan = planFanout(nets, 4, FanoutGroupBy::Model, { pinned }, nullptr);
  ASSERT_EQ(plan.groups.size(), 1u);
  EXPECT_EQ(plan.groupOf[0][15], -1);
  EXPECT_EQ(plan.groupOf[0][17], -1);
  // The pinned sinks count towards the limit: d15, d17, then d0 and d1.
  EXPECT_EQ(plan.groups[0].count, 20u - 4u);
  EXPECT_EQ(plan.groupOf[0][0], -1);
  EXPECT_EQ(plan.groupOf[0][1], -1);
  EXPECT_EQ(plan.groupOf[0][2], 0);

  // Expanded: every sink drawn.
  plan = planFanout(nets, 4, FanoutGroupBy::Model, {},
                    [](size_t ni, const FanoutGroupKey& k) { return ni == 0 && k.text == "DFF"; });
  EXPECT_TRUE(plan.groups.empty());
  for (int g : plan.groupOf[0]) EXPECT_EQ(g, -1);
}

TEST(PlanFanout, AGroupOfOneIsDrawn) {
  const std::vector<FanoutNet> nets{ net(driverItem(1, "drv"), 6, 1) };
  const FanoutPlan plan = planFanout(nets, 4, FanoutGroupBy::Model, {}, nullptr);
  ASSERT_EQ(plan.groups.size(), 1u);  // DFF x2; the lone AND2 is drawn
  EXPECT_EQ(plan.groups[0].key.text, "DFF");
  EXPECT_EQ(plan.groupOf[0].back(), -1);
}

TEST(PlanFanout, ByHierarchyAndByRole) {
  FanoutNet n;
  n.drivers.push_back(driverItem(1, "drv"));
  for (unsigned i = 0; i < 6; ++i)
    n.receivers.push_back(sinkItem({ { 10, "u_a" }, { 100 + i, "r" + std::to_string(i) } }, "DFF", "D"));
  for (unsigned i = 0; i < 4; ++i)
    n.receivers.push_back(sinkItem({ { 20, "u_b" }, { 200 + i, "c" + std::to_string(i) } }, "DFF", "CK", true));
  const std::vector<FanoutNet> nets{ n };

  FanoutPlan plan = planFanout(nets, 1, FanoutGroupBy::Hierarchy, {}, nullptr);
  ASSERT_EQ(plan.groups.size(), 2u);
  EXPECT_EQ(displayPath(plan.groups[0].key.module), "u_a");
  EXPECT_EQ(plan.groups[0].count, 5u);
  EXPECT_EQ(plan.groups[0].module, (InstancePath{ { 10, "u_a" } }));  // framed in u_a
  EXPECT_EQ(plan.groups[0].label, "u_a \xC3\x97" "5");
  EXPECT_EQ(displayPath(plan.groups[1].key.module), "u_b");

  plan = planFanout(nets, 1, FanoutGroupBy::Pin, {}, nullptr);
  ASSERT_EQ(plan.groups.size(), 2u);  // in key order: the clock pins first
  EXPECT_TRUE(plan.groups[0].key.clock);
  EXPECT_TRUE(plan.groups[0].clock);
  EXPECT_EQ(plan.groups[0].pin, "CK");
  EXPECT_EQ(plan.groups[0].label, "clock \xC3\x97" "4");
  EXPECT_FALSE(plan.groups[1].key.clock);
  EXPECT_EQ(plan.groups[1].key.text, "D");
  EXPECT_EQ(plan.groups[1].label, "D \xC3\x97" "5");

  // Model, across modules: the summary sits in their deepest common module.
  plan = planFanout(nets, 1, FanoutGroupBy::Model, {}, nullptr);
  ASSERT_EQ(plan.groups.size(), 1u);
  EXPECT_TRUE(plan.groups[0].module.empty());
  EXPECT_EQ(plan.groups[0].pin, "");  // D and CK: no shared pin
}

// ---------------------------------------------------------------------------
// End to end: renderSchematic() draws summaries; a click expands one.
// ---------------------------------------------------------------------------
namespace {

InstTermOccurrence occ(unsigned id, const std::string& name, const std::string& model,
                       const std::string& term, Direction dir) {
  InstTermOccurrence o;
  o.path           = { { id, name } };
  o.pathIds        = { id };
  o.pathModels     = { model };
  o.term           = BitTerm{ term, dir == Direction::Output ? 1u : 0u, dir, std::nullopt };
  o.bit_term_count = 2;
  return o;
}

class FanoutView : public ::testing::Test {
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
    saved = ViewOptions::current();
    // One buffer driving 30 flops.
    Equipotential e{ true, {}, {} };
    e.occurrences.push_back(occ(1, "drv", "BUF", "O", Direction::Output));
    for (unsigned i = 0; i < 30; ++i)
      e.occurrences.push_back(occ(100 + i, "f" + std::to_string(i), "DFF", "D", Direction::Input));
    eq = new Equipotential(std::move(e));
  }
  void TearDown() override {
    ViewOptions::current() = saved;
    EquipotentialView::setProvider(nullptr);
    EquipotentialView::resetLayout();
    SelectionStore::clear();
    delete eq;
    ImGui::DestroyContext();
  }
  void frame(int n = 1) {
    for (int i = 0; i < n; ++i) {
      unsigned char* px; int w, h;
      ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
      ImGui::NewFrame();
      ImGui::SetNextWindowPos(ImVec2(0.f, 0.f));
      ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
      ImGui::Begin("Schematic", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
      EquipotentialView::renderSchematic({ eq });
      ImGui::End();
      ImGui::Render();
    }
  }
  const SchematicView& sv() const { return EquipotentialView::schematicForTesting(); }
  std::vector<const InstanceShape*> summaries() const {
    std::vector<const InstanceShape*> out;
    for (const auto& s : sv().instances) if (s.isSummary) out.push_back(&s);
    return out;
  }
  int boxes() const {
    int n = 0;
    for (const auto& s : sv().instances) n += !s.isHierGroup && !s.isSummary && s.modelName != "port";
    return n;
  }

  FakeNetlistProvider provider;
  Equipotential* eq = nullptr;
  ViewOptions saved;
};

} // namespace

TEST_F(FanoutView, DrawsASummaryAndExpandsItOnClick) {
  ViewOptions::current().maxSinks = 10;
  ViewOptions::current().instanceLabel = InstanceLabel::Both;
  frame(3);
  auto sums = summaries();
  ASSERT_EQ(sums.size(), 1u);
  EXPECT_EQ(sums[0]->summaryCount, 20);
  EXPECT_EQ(sums[0]->name, "DFF \xC3\x97" "20");
  EXPECT_EQ(boxes(), 11);  // the driver and the first 10 flops
  // The model under every box; never asked from the host for the summary.
  for (const auto& s : sv().instances)
    if (!s.isSummary && !s.isHierGroup && s.modelName != "port") EXPECT_TRUE(s.showModel) << s.name;
  // Wired: one wire to the summary's pin.
  int toSummary = 0;
  for (const auto& n : sv().nets) toSummary += n.dstInstance == sums[0]->id;
  EXPECT_EQ(toSummary, 1);

  // An SVG export draws it, stacked, with its label.
  const std::string svg = EquipotentialView::exportSvg();
  EXPECT_NE(svg.find("DFF \xC3\x97" "20"), std::string::npos);

  // Click its body: every flop drawn.
  const InstanceShape& s = *sums[0];
  const ImVec2 at = sv().worldToScreen(ImVec2(s.x + s.w * 0.5f, s.y + s.h * 0.5f),
                                       EquipotentialView::canvasOriginForTesting(), ImVec2());
  ImGui::GetIO().AddMousePosEvent(at.x, at.y);
  frame();
  ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
  frame();
  ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
  frame(2);
  EXPECT_TRUE(summaries().empty());
  EXPECT_EQ(boxes(), 31);
  EXPECT_TRUE(provider.sent.empty() || provider.sent.back().find("get_properties") == std::string::npos);

  // A reset (a new view) gathers them again.
  EquipotentialView::resetLayout();
  frame(2);
  EXPECT_EQ(summaries().size(), 1u);
}

TEST_F(FanoutView, SinkGridKeepsTheDrawingCompact) {
  ViewOptions::current().maxSinks = 0;
  ViewOptions::current().sinkRows = 0;
  frame(3);
  ImVec2 lo, hi;
  ASSERT_TRUE(sv().computeWorldBounds(lo, hi));
  const float tall = hi.y - lo.y;
  ViewOptions::current().sinkRows = 6;
  frame(3);
  ASSERT_TRUE(sv().computeWorldBounds(lo, hi));
  EXPECT_LT(hi.y - lo.y, tall * 0.3f);
  EXPECT_EQ(boxes(), 31);
}

// ---------------------------------------------------------------------------
// Natural order
// ---------------------------------------------------------------------------
TEST(NaturalOrder, DigitRunsCompareByValue) {
  EXPECT_TRUE(naturalLess("i_b1_lut_111", "i_b1_lut_1037"));
  EXPECT_FALSE(naturalLess("i_b1_lut_1037", "i_b1_lut_111"));
  EXPECT_TRUE(naturalLess("u2", "u10"));
  EXPECT_TRUE(naturalLess("a9b", "a10a"));
  EXPECT_TRUE(naturalLess("x", "x1"));
  EXPECT_FALSE(naturalLess("same_7", "same_7"));
  EXPECT_TRUE(naturalLess("r07", "r7") != naturalLess("r7", "r07"));  // a strict order
  EXPECT_TRUE(naturalLess("r99999999999999999999", "r100000000000000000000"));  // no overflow
  EXPECT_TRUE(naturalPathLess({ { 9, "u2" }, { 1, "x" } }, { { 3, "u10" }, { 0, "a" } }));
}

// ---------------------------------------------------------------------------
// Several drivers' fanouts: bands, no crossings, natural order, and the
// convergence outline (the shape of a fanout-fix before/after figure).
// ---------------------------------------------------------------------------
namespace {

class MultiDriverFanout : public ::testing::Test {
 protected:
  // Top-level `en` drives three registers; each register's O drives
  // `sinks` LUTs, listed in a shuffled, non-natural order.
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
    TraceStore::clear();
    StyleStore::clear();
    saved = ViewOptions::current();
    ViewOptions::current() = ViewOptions{};
  }
  void TearDown() override {
    ViewOptions::current() = saved;
    EquipotentialView::setProvider(nullptr);
    EquipotentialView::resetLayout();
    SelectionStore::clear();
    TraceStore::clear();
    StyleStore::clear();
    for (auto* e : eqs) delete e;
    ImGui::DestroyContext();
  }
  static InstTermOccurrence pin(unsigned id, const std::string& name, const std::string& model,
                                const std::string& term, unsigned termId, Direction dir) {
    InstTermOccurrence o;
    o.path           = { { id, name } };
    o.pathIds        = { id };
    o.pathModels     = { model };
    o.term           = BitTerm{ term, termId, dir, std::nullopt };
    o.bit_term_count = 2;
    return o;
  }
  void build(unsigned sinks, bool traced = false) {
    const char* names[] = { "en_reg", "en_reg_fo1", "en_reg_fo2" };
    const int enTrace = traced ? TraceStore::begin("en") : -1;
    Equipotential en{ true, {}, {} };
    en.terms.push_back(BitTerm{ "en", 0, Direction::Input, std::nullopt });
    for (unsigned d = 0; d < 3; ++d) en.occurrences.push_back(pin(1 + d, names[d], "REG", "D", 0, Direction::Input));
    if (traced) en.traceIds = { enTrace };
    eqs.push_back(new Equipotential(en));
    for (unsigned d = 0; d < 3; ++d) {
      Equipotential o{ true, {}, {} };
      o.occurrences.push_back(pin(1 + d, names[d], "REG", "O", 1, Direction::Output));
      // Ids in a scrambled order, names with numbers of mixed widths.
      for (unsigned i = 0; i < sinks; ++i) {
        const unsigned k = (i * 7 + d) % sinks;
        const unsigned num = (k % 2 ? 1000 : 100) + k * 3 + d;
        o.occurrences.push_back(pin(100 + d * 1000 + (sinks - i), "i_b1_lut_" + std::to_string(num), "LUT1", "I0", 0,
                                    Direction::Input));
      }
      if (traced) o.traceIds = { TraceStore::begin(std::string(names[d]) + "/O") };
      eqs.push_back(new Equipotential(o));
    }
  }
  void frame(int n = 3) {
    for (int i = 0; i < n; ++i) {
      unsigned char* px; int w, h;
      ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
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
      if (!s.isHierGroup && !s.path.empty() && s.path.back().name == name) return &s;
    return nullptr;
  }
  // The boxes each driver's net reaches (its sinks), by driver shape id.
  std::map<int, std::vector<const InstanceShape*>> sinksByDriver() const {
    std::map<int, std::vector<const InstanceShape*>> out;
    for (const auto& n : sv().nets) {
      const InstanceShape* src = sv().findInstanceById(n.srcInstance);
      const InstanceShape* dst = sv().findInstanceById(n.dstInstance);
      if (src && dst && src->modelName != "port") out[src->id].push_back(dst);
    }
    return out;
  }
  // A net's sink columns: per column (box x), the rect from its first to
  // its last sink box.
  static std::vector<RouteRect> sinkColumns(const std::vector<const InstanceShape*>& sinks) {
    std::map<int, RouteRect> byX;
    for (const auto* s : sinks) {
      auto [it, fresh] = byX.try_emplace(int(std::lround(s->x)), RouteRect{ s->x, s->y, s->x + s->w, s->y + s->h });
      if (!fresh) {
        it->second.x1 = std::max(it->second.x1, s->x + s->w);
        it->second.y0 = std::min(it->second.y0, s->y);
        it->second.y1 = std::max(it->second.y1, s->y + s->h);
      }
    }
    std::vector<RouteRect> out;
    for (const auto& [x, r] : byX) out.push_back(r);
    return out;
  }
  static bool crosses(const RouteSegment& seg, const RouteRect& r) {
    constexpr float eps = 0.5f;
    const float x0 = std::min(seg.a.x, seg.b.x), x1 = std::max(seg.a.x, seg.b.x);
    const float y0 = std::min(seg.a.y, seg.b.y), y1 = std::max(seg.a.y, seg.b.y);
    return x1 > r.x0 + eps && x0 < r.x1 - eps && y1 > r.y0 + eps && y0 < r.y1 - eps;
  }

  FakeNetlistProvider         provider;
  std::vector<Equipotential*> eqs;
  ViewOptions                 saved;
};

} // namespace

// The figure's case: no wire of one net runs through another net's sink
// columns -- the area from its first column to its last, top sink to
// bottom one, channels between the columns included -- with the default
// grid (8 rows) and summaries (32 sinks).
TEST_F(MultiDriverFanout, NoNetCrossesAnotherNetsSinkColumns) {
  for (unsigned sinks : { 20u, 40u }) {
    SCOPED_TRACE(sinks);
    TearDown();
    eqs.clear();
    SetUp();
    build(sinks);
    frame();
    const auto sinks3 = sinksByDriver();
    ASSERT_EQ(sinks3.size(), 3u);
    int checked = 0;
    for (const auto& route : sv().routes) {
      if (route.ref.port) continue;  // en: drives the three registers only
      const InstanceShape* drv = nullptr;
      for (const auto& [id, list] : sinks3)
        if (sv().findInstanceById(id)->path == route.ref.path) drv = sv().findInstanceById(id);
      ASSERT_NE(drv, nullptr) << route.ref.pin;
      ASSERT_FALSE(route.segments.empty());
      for (const auto& [otherId, otherSinks] : sinks3) {
        if (otherId == drv->id) continue;
        const auto cols = sinkColumns(otherSinks);
        ASSERT_GE(cols.size(), 2u);
        RouteRect area = cols.front();
        for (const auto& c : cols) {
          area.x0 = std::min(area.x0, c.x0); area.x1 = std::max(area.x1, c.x1);
          area.y0 = std::min(area.y0, c.y0); area.y1 = std::max(area.y1, c.y1);
        }
        for (const auto& seg : route.segments) {
          ++checked;
          EXPECT_FALSE(crosses(seg, area))
              << drv->name << " crosses " << sv().findInstanceById(otherId)->name << "'s sink columns ("
              << area.x0 << "," << area.y0 << " - " << area.x1 << "," << area.y1 << ") with segment "
              << seg.a.x << "," << seg.a.y << " - " << seg.b.x << "," << seg.b.y;
        }
      }
    }
    EXPECT_GT(checked, 0);
  }
}

// Each driver's sinks sit in a band level with it, starting in the same
// column as every other driver's (no staircase), bands one under another.
TEST_F(MultiDriverFanout, EachDriversSinksAreABandLevelWithIt) {
  build(40);
  frame();
  const auto sinks3 = sinksByDriver();
  ASSERT_EQ(sinks3.size(), 3u);
  float firstX = -1.f, prevBottom = -1e9f;
  std::vector<std::pair<float, int>> byY;
  for (const auto& [id, list] : sinks3) byY.push_back({ sv().findInstanceById(id)->y, id });
  std::sort(byY.begin(), byY.end());
  for (const auto& [y, id] : byY) {
    const InstanceShape* drv = sv().findInstanceById(id);
    const auto cols = sinkColumns(sinks3.at(id));
    ASSERT_GE(cols.size(), 2u) << drv->name;  // a grid
    if (firstX < 0.f) firstX = cols.front().x0;
    EXPECT_FLOAT_EQ(cols.front().x0, firstX) << drv->name;  // aligned columns
    float top = 1e9f, bottom = -1e9f;
    for (const auto& c : cols) { top = std::min(top, c.y0); bottom = std::max(bottom, c.y1); }
    const float pinY = drv->y + drv->h * 0.5f;
    EXPECT_GT(pinY, top) << drv->name;
    EXPECT_LT(pinY, bottom) << drv->name;
    EXPECT_GT(top, prevBottom) << drv->name;  // bands don't overlap
    prevBottom = bottom;
  }
}

// The first sinks drawn, and their order down a column, are natural
// (i_b1_lut_111 before i_b1_lut_1037), whatever order the host sent.
TEST_F(MultiDriverFanout, SinksAreInNaturalOrder) {
  build(40);
  frame();
  for (const auto& [id, list] : sinksByDriver()) {
    std::vector<const InstanceShape*> drawn;
    for (const auto* s : list) if (!s->isSummary) drawn.push_back(s);
    ASSERT_EQ(drawn.size(), 32u);  // max_sinks
    // Reading order: columns left to right, each top to bottom.
    std::sort(drawn.begin(), drawn.end(), [](const InstanceShape* a, const InstanceShape* b) {
      return std::lround(a->x) != std::lround(b->x) ? a->x < b->x : a->y < b->y;
    });
    for (size_t i = 1; i < drawn.size(); ++i)
      EXPECT_TRUE(naturalLess(drawn[i - 1]->path.back().name, drawn[i]->path.back().name))
          << drawn[i - 1]->path.back().name << " before " << drawn[i]->path.back().name;
    // The summarized ones all come after the drawn ones.
    const auto summary = std::find_if(list.begin(), list.end(), [](auto* s) { return s->isSummary; });
    ASSERT_NE(summary, list.end());
    EXPECT_EQ((*summary)->summaryCount, 8);
  }
}

// A register on two visible traces (en and its own O) converges: outlined
// in near-black -- unless it's styled, or convergence_outline is off.
TEST_F(MultiDriverFanout, ConvergenceOutlineSkipsStyledBoxesAndCanBeTurnedOff) {
  build(4, /*traced=*/true);
  frame();
  ASSERT_NE(shape("en_reg"), nullptr);
  EXPECT_EQ(shape("en_reg")->diagOutline, TraceStore::convergenceColor());
  StyleStore::setStyles(StyleStore::parse(nlohmann::json::parse(
      R"({"items": [{"kind": "instance", "id_path": [1], "fill": "#1f77b455"}]})")));
  frame();
  EXPECT_EQ(shape("en_reg")->diagOutline, 0u);                                    // styled
  EXPECT_EQ(shape("en_reg_fo1")->diagOutline, TraceStore::convergenceColor());  // not styled
  ViewOptions::current().convergenceOutline = false;
  frame();
  EXPECT_EQ(shape("en_reg_fo1")->diagOutline, 0u);
  std::string error;
  EXPECT_TRUE(ViewOptions::current().apply({ { "convergence_outline", true } }, error));
  EXPECT_FALSE(ViewOptions::current().layoutJson().contains("convergence_outline"));
}

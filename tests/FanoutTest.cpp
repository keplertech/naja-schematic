// Drawing options (ViewOptions): instance labels (name / model / both), the
// sink grid of layeredPlacement(), and fanout summaries -- the planning
// (SchematicLayout::planFanout) and, end to end through renderSchematic(),
// the summary boxes and a click expanding one.
#include "EquipotentialView.h"
#include "SchematicLayout.h"
#include "SchematicView.h"
#include "SelectionStore.h"
#include "ViewOptions.h"

#include <imgui.h>
#include <gtest/gtest.h>

#include "FakeNetlistProvider.h"

using namespace SchematicLayout;

namespace {

float charWidth(const std::string& s) { return 7.f * float(s.size()); }

Item sinkItem(std::vector<std::pair<unsigned, std::string>> path, const std::string& model,
              const std::string& pin, bool clock = false) {
  Item it;
  it.label     = pin;
  it.direction = Direction::Input;
  it.clock     = clock;
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

  plan = planFanout(nets, 1, FanoutGroupBy::Role, {}, nullptr);
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

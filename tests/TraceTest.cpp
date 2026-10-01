#include "GUIData.h"
#include "TraceStore.h"
#include "Types.h"

#include <gtest/gtest.h>

namespace {

// Instance `id` of the top design, named "u<id>".
InstTermOccurrence occ(unsigned id, const std::string& term, Direction dir) {
  InstTermOccurrence o;
  o.path    = {{id, "u" + std::to_string(id)}};
  o.pathIds = {id};
  o.term = BitTerm{term, 0, dir, std::nullopt};
  return o;
}

Equipotential net(unsigned driver, unsigned receiver) {
  Equipotential e{true, {}, {}};
  e.occurrences.push_back(occ(driver, "Y", Direction::Output));
  e.occurrences.push_back(occ(receiver, "A", Direction::Input));
  return e;
}

class Traces : public ::testing::Test {
 protected:
  void SetUp() override { TraceStore::clear(); }
  void TearDown() override {
    data.clearEquipotentials();
    TraceStore::clear();
  }
  GUIData data;
};

} // namespace

// ---------------------------------------------------------------------------
// TraceStore
// ---------------------------------------------------------------------------

TEST_F(Traces, EachNewTraceTakesAFreeColor) {
  int a = TraceStore::begin("a");
  int b = TraceStore::begin("b");
  EXPECT_NE(a, b);
  EXPECT_NE(TraceStore::color(a), TraceStore::color(b));

  // A removed trace's color is the next one handed out.
  ImU32 freed = TraceStore::color(a);
  TraceStore::remove(a);
  int c = TraceStore::begin("c");
  EXPECT_EQ(TraceStore::color(c), freed);
}

TEST_F(Traces, NoColorIsTheConvergenceColor) {
  for (int s = 0; s < TraceStore::kStyleCount; ++s)
    EXPECT_NE(TraceStore::styleColor(s), TraceStore::convergenceColor());
}

TEST_F(Traces, VisibilityAndColorCanBeChanged) {
  int a = TraceStore::begin("a");
  EXPECT_TRUE(TraceStore::isVisible(a));
  TraceStore::setVisible(a, false);
  EXPECT_FALSE(TraceStore::isVisible(a));

  ImU32 palette = TraceStore::color(a);
  EXPECT_TRUE(TraceStore::setColor(a, IM_COL32(200, 40, 40, 128)));
  // Always opaque: hiding a trace is what fades it.
  EXPECT_EQ(TraceStore::color(a), IM_COL32(200, 40, 40, 255));
  TraceStore::resetColor(a);
  EXPECT_EQ(TraceStore::color(a), palette);
}

TEST_F(Traces, APickedColorCannotBeTheConvergenceColorOrTheCanvas) {
  int a = TraceStore::begin("a");
  ImU32 before = TraceStore::color(a);
  EXPECT_FALSE(TraceStore::setColor(a, TraceStore::convergenceColor()));
  EXPECT_FALSE(TraceStore::setColor(a, IM_COL32(50, 55, 60, 255)));     // near-black grey
  EXPECT_FALSE(TraceStore::setColor(a, IM_COL32(220, 225, 230, 255)));  // near-white grey
  EXPECT_EQ(TraceStore::color(a), before);
  EXPECT_TRUE(TraceStore::setColor(a, IM_COL32(20, 40, 160, 255)));     // dark blue is fine
  EXPECT_TRUE(TraceStore::setColor(a, IM_COL32(255, 240, 120, 255)));   // pale yellow is fine
  for (int s = 0; s < TraceStore::kStyleCount; ++s) {
    EXPECT_FALSE(TraceStore::tooCloseToConvergence(TraceStore::styleColor(s)));
    EXPECT_FALSE(TraceStore::tooCloseToCanvas(TraceStore::styleColor(s)));
  }
}

TEST(TraceLabel, IsThePinTheTraceStartsFrom) {
  EXPECT_EQ(traceLabel({}, "clk"), "clk");
  EXPECT_EQ(traceLabel(InstancePath{{1, "u1"}, {2, "u2"}}, "A[3]"), "u1/u2/A[3]");
}

// ---------------------------------------------------------------------------
// GUIData: which traces own which nets
// ---------------------------------------------------------------------------

TEST_F(Traces, ANetTwoTracesReachIsShownOnceAndOnBoth) {
  int a = TraceStore::begin("a");
  int b = TraceStore::begin("b");
  EXPECT_TRUE(data.addEquipotential(new Equipotential(net(0, 1)), a));
  EXPECT_FALSE(data.addEquipotential(new Equipotential(net(0, 1)), b));
  ASSERT_EQ(data.equipotentials_.size(), 1u);
  EXPECT_EQ(data.equipotentials_[0]->traceIds, (std::vector<int>{a, b}));
  EXPECT_FALSE(data.equipotentials_[0]->direct);
}

TEST_F(Traces, ANetShownOnItsOwnIsDirect) {
  data.addEquipotential(new Equipotential(net(0, 1)));
  ASSERT_EQ(data.equipotentials_.size(), 1u);
  EXPECT_TRUE(data.equipotentials_[0]->direct);
  EXPECT_TRUE(data.equipotentials_[0]->traceIds.empty());
}

TEST_F(Traces, RemovingATraceKeepsTheNetsSomethingElseShows) {
  int a = TraceStore::begin("a");
  int b = TraceStore::begin("b");
  data.addEquipotential(new Equipotential(net(0, 1)), a);   // a only
  data.addEquipotential(new Equipotential(net(1, 2)), a);   // a and b
  data.addEquipotential(new Equipotential(net(1, 2)), b);
  data.addEquipotential(new Equipotential(net(2, 3)), a);   // a, and shown directly
  data.addEquipotential(new Equipotential(net(2, 3)));

  data.removeTrace(a);

  ASSERT_EQ(data.equipotentials_.size(), 2u);
  EXPECT_EQ(data.equipotentials_[0]->traceIds, (std::vector<int>{b}));
  EXPECT_TRUE(data.equipotentials_[1]->traceIds.empty());
  EXPECT_TRUE(data.equipotentials_[1]->direct);
  EXPECT_EQ(TraceStore::find(a), nullptr);
  EXPECT_NE(TraceStore::find(b), nullptr);
}

TEST_F(Traces, ClearingTheViewForgetsItsTraces) {
  int a = TraceStore::begin("a");
  data.addEquipotential(new Equipotential(net(0, 1)), a);
  data.clearEquipotentials();
  EXPECT_TRUE(data.equipotentials_.empty());
  EXPECT_TRUE(TraceStore::traces().empty());
}

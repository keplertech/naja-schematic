#include "GUIData.h"
#include "TraceStore.h"
#include "Types.h"

#include <gtest/gtest.h>

namespace {

InstTermOccurrence occ(std::vector<std::string> path, const std::string& term, Direction dir) {
  InstTermOccurrence o;
  o.path = std::move(path);
  o.term = BitTerm{term, 0, dir, std::nullopt};
  return o;
}

Equipotential net(const std::string& driver, const std::string& receiver) {
  Equipotential e{true, {}, {}};
  e.occurrences.push_back(occ({driver}, "Y", Direction::Output));
  e.occurrences.push_back(occ({receiver}, "A", Direction::Input));
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

  ImU32 before = TraceStore::color(a);
  TraceStore::cycleStyle(a);
  EXPECT_NE(TraceStore::color(a), before);
}

TEST(TraceLabel, IsThePinTheTraceStartsFrom) {
  EXPECT_EQ(traceLabel({}, "clk"), "clk");
  EXPECT_EQ(traceLabel({"u1", "u2"}, "A[3]"), "u1/u2/A[3]");
}

// ---------------------------------------------------------------------------
// GUIData: which traces own which nets
// ---------------------------------------------------------------------------

TEST_F(Traces, ANetTwoTracesReachIsShownOnceAndOnBoth) {
  int a = TraceStore::begin("a");
  int b = TraceStore::begin("b");
  EXPECT_TRUE(data.addEquipotential(new Equipotential(net("u0", "u1")), a));
  EXPECT_FALSE(data.addEquipotential(new Equipotential(net("u0", "u1")), b));
  ASSERT_EQ(data.equipotentials_.size(), 1u);
  EXPECT_EQ(data.equipotentials_[0]->traceIds, (std::vector<int>{a, b}));
  EXPECT_FALSE(data.equipotentials_[0]->direct);
}

TEST_F(Traces, ANetShownOnItsOwnIsDirect) {
  data.addEquipotential(new Equipotential(net("u0", "u1")));
  ASSERT_EQ(data.equipotentials_.size(), 1u);
  EXPECT_TRUE(data.equipotentials_[0]->direct);
  EXPECT_TRUE(data.equipotentials_[0]->traceIds.empty());
}

TEST_F(Traces, RemovingATraceKeepsTheNetsSomethingElseShows) {
  int a = TraceStore::begin("a");
  int b = TraceStore::begin("b");
  data.addEquipotential(new Equipotential(net("u0", "u1")), a);   // a only
  data.addEquipotential(new Equipotential(net("u1", "u2")), a);   // a and b
  data.addEquipotential(new Equipotential(net("u1", "u2")), b);
  data.addEquipotential(new Equipotential(net("u2", "u3")), a);   // a, and shown directly
  data.addEquipotential(new Equipotential(net("u2", "u3")));

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
  data.addEquipotential(new Equipotential(net("u0", "u1")), a);
  data.clearEquipotentials();
  EXPECT_TRUE(data.equipotentials_.empty());
  EXPECT_TRUE(TraceStore::traces().empty());
}

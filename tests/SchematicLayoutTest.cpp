#include "SchematicLayout.h"

#include <cmath>

#include <gtest/gtest.h>

using namespace SchematicLayout;

namespace {

constexpr float kColStep = kInstW + kColGap;   // x distance between logic columns
constexpr float kRowStep = kInstH + kRowSpacing;

InstTermOccurrence occ(std::vector<std::string> path, const std::string& term, Direction dir,
                       std::vector<std::string> models = {}) {
  InstTermOccurrence o;
  o.path       = std::move(path);
  o.pathModels = std::move(models);
  o.term       = BitTerm{term, 0, dir, std::nullopt};
  return o;
}

BitTerm topTerm(const std::string& name, Direction dir) { return BitTerm{name, 0, dir, std::nullopt}; }

// driver.Q -> each receiver's A pin, all instances at the top level.
Equipotential net(const std::string& driver, std::vector<std::string> receivers) {
  Equipotential eq{true, {}, {}};
  eq.occurrences.push_back(occ({driver}, "Q", Direction::Output));
  for (auto& r : receivers) eq.occurrences.push_back(occ({r}, "A", Direction::Input));
  return eq;
}

ImVec2 pos(const IncrementalLayout& l, const InstancePath& path) {
  auto it = l.positions().find(path);
  EXPECT_NE(it, l.positions().end()) << displayPath(path) << " was not placed";
  return it == l.positions().end() ? ImVec2(NAN, NAN) : it->second;
}

InstanceShape box(int id, const InstancePath& path, float x, float y,
                  float w = kInstW, float h = kInstH) {
  InstanceShape s;
  s.id = id; s.path = path; s.name = displayPath(path); s.x = x; s.y = y; s.w = w; s.h = h;
  return s;
}

bool contains(const InstanceShape& outer, const InstanceShape& inner) {
  return inner.x >= outer.x && inner.y >= outer.y &&
         inner.x + inner.w <= outer.x + outer.w &&
         inner.y + inner.h <= outer.y + outer.h;
}

bool overlaps(const InstanceShape& a, const InstanceShape& b) {
  return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

const InstanceShape& byPath(const std::vector<InstanceShape>& v, const InstancePath& path) {
  for (const auto& s : v) if (s.path == path) return s;
  ADD_FAILURE() << "no shape for " << displayPath(path);
  static InstanceShape none;
  return none;
}

} // namespace

// ---------------------------------------------------------------------------
// buildItems
// ---------------------------------------------------------------------------

TEST(SchematicLayoutItems, DriversAreTopInputsAndInstanceOutputs) {
  Equipotential eq{true, {topTerm("clk", Direction::Input), topTerm("out", Direction::Output)}, {}};
  eq.occurrences.push_back(occ({"u1", "g"}, "Q", Direction::Output));
  eq.occurrences.push_back(occ({"u2"}, "A", Direction::Input));

  std::vector<Item> drivers, receivers;
  buildItems(&eq, drivers, receivers);

  ASSERT_EQ(drivers.size(), 2u);
  EXPECT_TRUE(drivers[0].isTerm);
  EXPECT_EQ(drivers[0].label, "clk");
  EXPECT_EQ(drivers[1].path, (InstancePath{"u1", "g"}));
  ASSERT_EQ(receivers.size(), 2u);
  EXPECT_EQ(receivers[0].label, "out");
  EXPECT_EQ(receivers[1].path, (InstancePath{"u2"}));
}

// ---------------------------------------------------------------------------
// IncrementalLayout::place
// ---------------------------------------------------------------------------

TEST(IncrementalLayout, FirstNetPutsDriversLeftAndReceiversRight) {
  IncrementalLayout layout;
  auto eq = net("u1", {"u2", "u3"});
  layout.place(&eq);

  EXPECT_EQ(pos(layout, {"u1"}).x, kLeftMargin);
  EXPECT_EQ(pos(layout, {"u1"}).y, 0.f);
  EXPECT_EQ(pos(layout, {"u2"}).x, kLeftMargin + kColStep);
  EXPECT_EQ(pos(layout, {"u2"}).y, 0.f);
  EXPECT_EQ(pos(layout, {"u3"}).x, kLeftMargin + kColStep);
  EXPECT_EQ(pos(layout, {"u3"}).y, kRowStep);
}

TEST(IncrementalLayout, TopLevelTermsAreNotPlaced) {
  IncrementalLayout layout;
  Equipotential eq{true, {topTerm("in", Direction::Input)}, {}};
  eq.occurrences.push_back(occ({"u1"}, "A", Direction::Input));
  layout.place(&eq);

  EXPECT_EQ(layout.positions().size(), 1u);
  EXPECT_EQ(layout.positions().count(InstancePath{"in"}), 0u);
  // The instance is a receiver: right column, even with no instance driver.
  EXPECT_EQ(pos(layout, {"u1"}).x, kLeftMargin + kColStep);
}

TEST(IncrementalLayout, PlacingTheSameNetTwiceIsANoOp) {
  IncrementalLayout layout;
  auto eq = net("u1", {"u2"});
  layout.place(&eq);
  auto before = layout.positions();
  layout.place(&eq);
  EXPECT_EQ(layout.positions().size(), before.size());

  // ...and it doesn't advance the "below existing content" cursor either.
  auto other = net("v1", {"v2"});
  layout.place(&other);
  EXPECT_EQ(pos(layout, {"v1"}).y, kRowStep + kNetVGap);
}

TEST(IncrementalLayout, UnrelatedNetGoesBelowTheTallestColumn) {
  IncrementalLayout layout;
  auto a = net("u1", {"u2", "u3"});   // right column is 2 rows tall
  auto b = net("v1", {"v2"});
  layout.place(&a);
  layout.place(&b);

  const float expectedY = 2 * kRowStep + kNetVGap;
  EXPECT_EQ(pos(layout, {"v1"}).x, kLeftMargin);
  EXPECT_EQ(pos(layout, {"v1"}).y, expectedY);
  EXPECT_EQ(pos(layout, {"v2"}).y, expectedY);
}

TEST(IncrementalLayout, DriverOfAPlacedReceiverGoesOneColumnLeft) {
  IncrementalLayout layout;
  auto a = net("g", {"sink"});
  auto b = net("src", {"g"});         // trace one step back from g's input
  layout.place(&a);
  layout.place(&b);

  EXPECT_EQ(pos(layout, {"src"}).x, pos(layout, {"g"}).x - kColStep);
  EXPECT_EQ(pos(layout, {"src"}).y, pos(layout, {"g"}).y);
  // The anchor itself never moves.
  EXPECT_EQ(pos(layout, {"g"}).x, kLeftMargin);
}

TEST(IncrementalLayout, ReceiversOfAPlacedDriverGoOneColumnRight) {
  IncrementalLayout layout;
  auto a = net("g", {"sink"});
  auto b = net("sink", {"r1", "r2"});
  layout.place(&a);
  layout.place(&b);

  const float x = pos(layout, {"sink"}).x + kColStep;
  EXPECT_EQ(pos(layout, {"r1"}).x, x);
  EXPECT_EQ(pos(layout, {"r2"}).x, x);
  EXPECT_EQ(pos(layout, {"r2"}).y, pos(layout, {"r1"}).y + kRowStep);
}

// A driver trace through a 2-input gate: each input net anchors on the gate
// and wants the same slot left of it. The second must slide down, not stack.
TEST(IncrementalLayout, FanInNetsAnchoredOnTheSameGateDoNotStack) {
  IncrementalLayout layout;
  auto out = net("g", {"sink"});
  auto inA = net("a", {"g"});
  auto inB = net("b", {"g"});
  layout.place(&out);
  layout.place(&inA);
  layout.place(&inB);

  EXPECT_EQ(pos(layout, {"a"}).x, pos(layout, {"b"}).x);
  EXPECT_EQ(pos(layout, {"a"}).y, pos(layout, {"g"}).y);
  EXPECT_EQ(pos(layout, {"b"}).y, pos(layout, {"a"}).y + kRowStep);
}

TEST(IncrementalLayout, NoTwoPlacedBoxesOverlapInADeeperTrace) {
  // g <- {a, b}, a <- {c, d}, b <- {e, f}: a small binary fan-in tree.
  IncrementalLayout layout;
  std::vector<Equipotential> nets = {
      net("g", {"sink"}), net("a", {"g"}), net("b", {"g"}),
      net("c", {"a"}),    net("d", {"a"}), net("e", {"b"}), net("f", {"b"})};
  for (auto& n : nets) layout.place(&n);

  std::vector<InstanceShape> boxes;
  int id = 1;
  for (const auto& [key, p] : layout.positions()) boxes.push_back(box(id++, key, p.x, p.y));
  for (size_t i = 0; i < boxes.size(); ++i)
    for (size_t j = i + 1; j < boxes.size(); ++j)
      EXPECT_FALSE(overlaps(boxes[i], boxes[j])) << boxes[i].name << " vs " << boxes[j].name;
}

TEST(IncrementalLayout, ClearForgetsPositionsAndTheVerticalCursor) {
  IncrementalLayout layout;
  auto a = net("u1", {"u2"});
  layout.place(&a);
  layout.clear();
  EXPECT_TRUE(layout.positions().empty());

  layout.place(&a);  // placeable again after clear, back at the top
  EXPECT_EQ(pos(layout, {"u1"}).y, 0.f);
}

// ---------------------------------------------------------------------------
// IncrementalLayout::resolveColumnOverlaps
// ---------------------------------------------------------------------------

TEST(IncrementalLayout, TallBoxPushesTheNextOneInItsColumnDown) {
  IncrementalLayout layout;
  std::vector<InstanceShape> shapes = {
      box(1, {"tall"}, 20.f, 0.f, kInstW, 200.f),
      box(2, {"below"}, 20.f, kRowStep),
      box(3, {"otherColumn"}, 320.f, kRowStep),
  };
  layout.resolveColumnOverlaps(shapes);

  EXPECT_EQ(shapes[0].y, 0.f);
  EXPECT_EQ(shapes[1].y, 200.f + kRowSpacing);
  EXPECT_EQ(shapes[2].y, kRowStep);  // different column: untouched
  // Persisted, so the next place() anchors on the drawn position.
  EXPECT_EQ(pos(layout, {"below"}).y, 200.f + kRowSpacing);
}

TEST(IncrementalLayout, ColumnOverlapsIgnoreZeroWidthTermStubs) {
  IncrementalLayout layout;
  std::vector<InstanceShape> shapes = {
      box(1, {"u1"}, 20.f, 0.f),
      box(2, {}, 20.f, 10.f, 0.f, 0.f),
  };
  layout.resolveColumnOverlaps(shapes);
  EXPECT_EQ(shapes[1].y, 10.f);
  EXPECT_EQ(layout.positions().count(InstancePath{}), 0u);
}

// ---------------------------------------------------------------------------
// layoutHierarchyGroups
// ---------------------------------------------------------------------------

namespace {

// Leaves at logic column `col` (as IncrementalLayout would have placed them).
struct LeafSpec { InstancePath path; std::vector<std::string> models; int col; float y; };

struct HierFixture {
  std::vector<InstanceShape>         shapes;
  std::map<InstancePath, LeafHier>   leafHier;
  std::map<InstancePath, int>        pathToInstId;
  int                                nextInstId = 1;

  explicit HierFixture(const std::vector<LeafSpec>& leaves) {
    for (const auto& l : leaves) {
      int id = nextInstId++;
      shapes.push_back(box(id, l.path, kLeftMargin + float(l.col) * kColStep, l.y));
      leafHier[l.path]     = {l.path, l.models};
      pathToInstId[l.path] = id;
    }
  }
  std::vector<HierFrame> run() {
    return layoutHierarchyGroups(leafHier, shapes, pathToInstId, nextInstId);
  }
};

const HierFrame& frame(const std::vector<HierFrame>& frames, const InstancePath& path) {
  for (const auto& f : frames) if (f.shape.path == path) return f;
  ADD_FAILURE() << "no frame for " << displayPath(path);
  static HierFrame none;
  return none;
}

} // namespace

TEST(HierarchyGroups, FlatDesignProducesNoFramesAndMovesNothing) {
  HierFixture fx({{{"u1"}, {}, 0, 0.f}, {{"u2"}, {}, 1, 0.f}});
  auto before = fx.shapes;
  auto frames = fx.run();

  EXPECT_TRUE(frames.empty());
  for (size_t i = 0; i < before.size(); ++i) {
    EXPECT_EQ(fx.shapes[i].x, before[i].x);
    EXPECT_EQ(fx.shapes[i].y, before[i].y);
    EXPECT_TRUE(fx.shapes[i].label.empty());
  }
}

TEST(HierarchyGroups, OneModuleFrameWrapsItsLeaves) {
  HierFixture fx({{{"core", "g1"}, {"Core", "AND2"}, 0, 0.f},
                  {{"core", "g2"}, {"Core", "OR2"}, 1, 0.f}});
  const int firstFreeId = fx.nextInstId;
  auto frames = fx.run();

  ASSERT_EQ(frames.size(), 1u);
  const auto& f = frames[0].shape;
  EXPECT_EQ(frames[0].shape.path, (InstancePath{"core"}));
  EXPECT_EQ(f.name, "core (Core)");
  EXPECT_TRUE(f.isHierGroup);
  EXPECT_EQ(f.hierDepth, 1);
  EXPECT_EQ(f.id, firstFreeId);
  EXPECT_EQ(fx.nextInstId, firstFreeId + 1);

  const auto& g1 = byPath(fx.shapes, {"core", "g1"});
  const auto& g2 = byPath(fx.shapes, {"core", "g2"});
  EXPECT_TRUE(contains(f, g1));
  EXPECT_TRUE(contains(f, g2));
  // Room for the frame's label above its contents.
  EXPECT_GE(g1.y - f.y, kGroupHeader);
  // Inside a frame the box shows just the leaf name.
  EXPECT_EQ(g1.label, "g1");
  EXPECT_EQ(g2.label, "g2");
}

TEST(HierarchyGroups, FrameLabelFallsBackToInstanceNameWithoutModel) {
  HierFixture fx({{{"core", "g1"}, {}, 0, 0.f}});
  auto frames = fx.run();
  ASSERT_EQ(frames.size(), 1u);
  EXPECT_EQ(frames[0].shape.name, "core");
}

TEST(HierarchyGroups, LogicColumnsStayLeftToRightInsideAFrame) {
  HierFixture fx({{{"core", "drv"}, {}, 0, 0.f},
                  {{"core", "mid"}, {}, 1, 0.f},
                  {{"core", "rcv"}, {}, 2, 0.f}});
  fx.run();

  const auto& drv = byPath(fx.shapes, {"core", "drv"});
  const auto& mid = byPath(fx.shapes, {"core", "mid"});
  const auto& rcv = byPath(fx.shapes, {"core", "rcv"});
  EXPECT_EQ(mid.x - (drv.x + drv.w), kGroupColGap);
  EXPECT_EQ(rcv.x - (mid.x + mid.w), kGroupColGap);
  EXPECT_EQ(drv.y, mid.y);
  EXPECT_EQ(mid.y, rcv.y);
}

TEST(HierarchyGroups, SameColumnLeavesKeepTheirVerticalOrder) {
  // Given in reverse y order on purpose; map order (by key) is also reversed.
  HierFixture fx({{{"core", "b"}, {}, 0, 0.f},
                  {{"core", "a"}, {}, 0, 3 * kRowStep}});
  fx.run();
  EXPECT_LT(byPath(fx.shapes, {"core", "b"}).y, byPath(fx.shapes, {"core", "a"}).y);
  EXPECT_FALSE(overlaps(byPath(fx.shapes, {"core", "a"}), byPath(fx.shapes, {"core", "b"})));
}

TEST(HierarchyGroups, NestedModulesNestTheirFramesParentFirst) {
  HierFixture fx({{{"top_a", "sub", "g"}, {"A", "S", "INV"}, 0, 0.f},
                  {{"top_a", "h"}, {"A", "BUF"}, 1, 0.f}});
  auto frames = fx.run();

  ASSERT_EQ(frames.size(), 2u);
  EXPECT_EQ(frames[0].shape.path, (InstancePath{"top_a"}));  // parent first: drawn underneath
  EXPECT_EQ(frames[1].shape.path, (InstancePath{"top_a", "sub"}));
  EXPECT_EQ(frames[0].shape.hierDepth, 1);
  EXPECT_EQ(frames[1].shape.hierDepth, 2);
  EXPECT_EQ(frames[1].shape.name, "sub (S)");

  const auto& outer = frames[0].shape;
  const auto& inner = frames[1].shape;
  EXPECT_TRUE(contains(outer, inner));
  EXPECT_TRUE(contains(inner, byPath(fx.shapes, {"top_a", "sub", "g"})));
  EXPECT_TRUE(contains(outer, byPath(fx.shapes, {"top_a", "h"})));
  EXPECT_FALSE(overlaps(inner, byPath(fx.shapes, {"top_a", "h"})));
}

TEST(HierarchyGroups, SiblingModulesDoNotOverlap) {
  // Two modules whose leaves sit in the same logic columns.
  HierFixture fx({{{"m1", "a"}, {}, 0, 0.f}, {{"m1", "b"}, {}, 1, 0.f},
                  {{"m2", "c"}, {}, 0, kRowStep}, {{"m2", "d"}, {}, 1, kRowStep}});
  auto frames = fx.run();

  ASSERT_EQ(frames.size(), 2u);
  const auto& m1 = frame(frames, {"m1"}).shape;
  const auto& m2 = frame(frames, {"m2"}).shape;
  EXPECT_FALSE(overlaps(m1, m2));
  for (const InstancePath& path : {InstancePath{"m1", "a"}, InstancePath{"m1", "b"}}) {
    EXPECT_TRUE(contains(m1, byPath(fx.shapes, path))) << displayPath(path);
    EXPECT_FALSE(overlaps(m2, byPath(fx.shapes, path))) << displayPath(path);
  }
  for (const InstancePath& path : {InstancePath{"m2", "c"}, InstancePath{"m2", "d"}}) {
    EXPECT_TRUE(contains(m2, byPath(fx.shapes, path))) << displayPath(path);
    EXPECT_FALSE(overlaps(m1, byPath(fx.shapes, path))) << displayPath(path);
  }
}

TEST(HierarchyGroups, TopLevelLeavesStayOutsideFramesAndKeepTheirLabel) {
  HierFixture fx({{{"src"}, {}, 0, 0.f},
                  {{"core", "g"}, {}, 1, 0.f},
                  {{"dst"}, {}, 2, 0.f}});
  auto frames = fx.run();

  ASSERT_EQ(frames.size(), 1u);
  const auto& core = frames[0].shape;
  const auto& src  = byPath(fx.shapes, {"src"});
  const auto& dst  = byPath(fx.shapes, {"dst"});
  EXPECT_FALSE(overlaps(core, src));
  EXPECT_FALSE(overlaps(core, dst));
  EXPECT_TRUE(src.label.empty());
  // Signal flow still reads left to right across the frame.
  EXPECT_LT(src.x + src.w, core.x);
  EXPECT_LT(core.x + core.w, dst.x);
}

TEST(HierarchyGroups, LeavesWithoutAShapeAreIgnored) {
  HierFixture fx({{{"core", "g"}, {}, 0, 0.f}});
  fx.leafHier[{"ghost", "x"}] = {{"ghost", "x"}, {}};   // no pathToInstId entry
  auto frames = fx.run();
  ASSERT_EQ(frames.size(), 1u);
  EXPECT_EQ(frames[0].shape.path, (InstancePath{"core"}));
}

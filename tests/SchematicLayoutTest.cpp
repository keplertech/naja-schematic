#include "SchematicLayout.h"

#include <cmath>
#include <map>

#include <gtest/gtest.h>

using namespace SchematicLayout;

namespace {

constexpr float kBoxW = 60.f, kBoxH = 40.f;
constexpr float kRowStep = kBoxH + kRowSpacing;

// A path of named instances. Each name gets a fixed id on first use, so the
// same names always make the same path; tests about anonymous (or
// same-named) instances spell their ids out instead.
InstancePath P(const std::vector<std::string>& names) {
  static std::map<std::string, unsigned> ids;
  InstancePath path;
  for (const auto& n : names) {
    auto it = ids.try_emplace(n, unsigned(ids.size()) + 100).first;
    path.push_back({it->second, n});
  }
  return path;
}

InstTermOccurrence occ(std::vector<std::string> path, const std::string& term, Direction dir,
                       std::vector<std::string> models = {}) {
  InstTermOccurrence o;
  o.path       = P(path);
  o.pathModels = std::move(models);
  o.term       = BitTerm{term, 0, dir, std::nullopt};
  return o;
}

BitTerm topTerm(const std::string& name, Direction dir) { return BitTerm{name, 0, dir, std::nullopt}; }

InstanceShape box(int id, const InstancePath& path, float x, float y,
                  float w = kBoxW, float h = kBoxH) {
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

const InstanceShape& byRef(const std::vector<InstanceShape>& v, const InstancePath& path);
const InstanceShape& byPath(const std::vector<InstanceShape>& v, const std::vector<std::string>& names) {
  return byRef(v, P(names));
}
const InstanceShape& byRef(const std::vector<InstanceShape>& v, const InstancePath& path) {
  for (const auto& s : v) if (s.path == path) return s;
  ADD_FAILURE() << "no shape for " << displayPath(path);
  static InstanceShape none;
  return none;
}

bool onGrid(float v) { return std::abs(v / kGrid - std::round(v / kGrid)) < 1e-4f; }

// A monospace stand-in for the view's font.
constexpr float kCharW = 7.f;
float charWidth(const std::string& s) { return kCharW * float(s.size()); }

} // namespace

// ---------------------------------------------------------------------------
// Instance names
// ---------------------------------------------------------------------------

TEST(InstanceNames, ANameThatFitsIsShownWhole) {
  EXPECT_EQ(shortenMiddle("u_alu", charWidth, 100.f), "u_alu");
}

TEST(InstanceNames, ALongNameKeepsItsHeadAndTail) {
  const std::string name = "buffer_1.Queue.value_1$_SDFFE_PP0P_";
  const float maxW = 20.f * kCharW;
  const std::string shown = shortenMiddle(name, charWidth, maxW);

  EXPECT_LE(charWidth(shown), maxW);
  EXPECT_EQ(shown.size(), 20u);   // as many characters as fit
  EXPECT_EQ(shown, "buffer_1...FFE_PP0P_");
  EXPECT_EQ(shown.substr(0, 8), name.substr(0, 8));
  EXPECT_EQ(shown.substr(shown.size() - 9), name.substr(name.size() - 9));
}

TEST(InstanceNames, NoRoomAtAllGivesNoName) {
  EXPECT_EQ(shortenMiddle("abcdef", charWidth, 2.f * kCharW), "");
}

TEST(InstanceNames, ALabelIsShownInsteadOfTheName) {
  InstanceShape s = box(1, P({"core", "g1"}), 0.f, 0.f);
  s.label = "g1";
  EXPECT_EQ(shownName(s, charWidth), "g1");
}

TEST(InstanceNames, FramesAndTopLevelPortsHaveNone) {
  InstanceShape frame = box(1, P({"core"}), 0.f, 0.f);
  frame.isHierGroup = true;
  InstanceShape port = box(2, {}, 0.f, 0.f, 0.f, 0.f);
  port.name = "clk"; port.modelName = "port";
  EXPECT_EQ(shownName(frame, charWidth), "");
  EXPECT_EQ(shownName(port, charWidth), "");
  EXPECT_FALSE(nameRect(frame, charWidth));
  EXPECT_EQ(footprintWidth(frame, charWidth), frame.w);
}

TEST(InstanceNames, TheFootprintIsTheWiderOfSymbolAndName) {
  InstanceShape shortName = box(1, P({"u1"}), 0.f, 0.f);
  InstanceShape longName  = box(2, P({"a_rather_long_instance_name"}), 30.f, 50.f);
  EXPECT_EQ(footprintWidth(shortName, charWidth), kBoxW);
  EXPECT_EQ(footprintWidth(longName, charWidth), charWidth(longName.name));

  auto r = nameRect(longName, charWidth);
  ASSERT_TRUE(r);
  EXPECT_EQ(r->x0, 30.f);
  EXPECT_EQ(r->x1, 30.f + charWidth(longName.name));
  EXPECT_EQ(r->y0, 50.f - kNameH);
  EXPECT_EQ(r->y1, 50.f);
}

TEST(InstanceNames, AVeryLongNameIsCappedInTheFootprint) {
  InstanceShape s = box(1, P({std::string(200, 'x')}), 0.f, 0.f);
  EXPECT_LE(footprintWidth(s, charWidth), kNameMaxW);
  EXPECT_GT(footprintWidth(s, charWidth), kNameMaxW - kCharW);
}

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
  EXPECT_EQ(drivers[1].path, P({"u1", "g"}));
  ASSERT_EQ(receivers.size(), 2u);
  EXPECT_EQ(receivers[0].label, "out");
  EXPECT_EQ(receivers[1].path, P({"u2"}));
}

// ---------------------------------------------------------------------------
// symbolGeometry
// ---------------------------------------------------------------------------

TEST(Symbols, GateInputsAreCenteredOnItsOutput) {
  auto g = symbolGeometry(PrimitiveType::And, {"A1", "A2"}, {"ZN"});
  EXPECT_EQ(g.h, 2.f * kPinPitch);
  EXPECT_EQ(g.leftY, (std::vector<float>{10.f, 30.f}));
  EXPECT_EQ(g.rightY, (std::vector<float>{20.f}));

  auto g3 = symbolGeometry(PrimitiveType::Nor, {"A", "B", "C"}, {"Y"});
  EXPECT_EQ(g3.h, 3.f * kPinPitch);
  EXPECT_EQ(g3.leftY, (std::vector<float>{10.f, 30.f, 50.f}));
  EXPECT_EQ(g3.rightY, (std::vector<float>{30.f}));
}

TEST(Symbols, EveryPinAndSizeIsOnTheGrid) {
  for (auto type : {PrimitiveType::And, PrimitiveType::Xnor, PrimitiveType::Inv,
                    PrimitiveType::Dff, PrimitiveType::Unknown})
    for (size_t n = 1; n <= 6; ++n) {
      std::vector<std::string> in(n, "A");
      auto g = symbolGeometry(type, in, {"Y"});
      EXPECT_TRUE(onGrid(g.w) && onGrid(g.h)) << int(type) << " x" << n;
      for (float y : g.leftY)  EXPECT_TRUE(onGrid(y)) << int(type) << " x" << n;
      for (float y : g.rightY) EXPECT_TRUE(onGrid(y)) << int(type) << " x" << n;
      for (float y : g.leftY)  EXPECT_TRUE(y > 0.f && y < g.h);
    }
}

TEST(Symbols, BufferIsASmallSquare) {
  auto g = symbolGeometry(PrimitiveType::Inv, {"A"}, {"ZN"});
  EXPECT_EQ(g.w, g.h);
  EXPECT_EQ(g.leftY, g.rightY);
}

TEST(Symbols, BoxPinsRunDownFromTheTopAndItsWidthFitsTheirNames) {
  auto narrow = symbolGeometry(PrimitiveType::Unknown, {"A", "B", "C"}, {"Y"});
  EXPECT_EQ(narrow.leftY, (std::vector<float>{20.f, 40.f, 60.f}));
  EXPECT_EQ(narrow.rightY, (std::vector<float>{20.f}));
  EXPECT_EQ(narrow.h, 4.f * kPinPitch);

  auto wide = symbolGeometry(PrimitiveType::Unknown, {"write_enable_n"}, {"read_data_out"});
  EXPECT_GT(wide.w, narrow.w);
}

TEST(Symbols, GateWithSeveralOutputsIsDrawnAsABox) {
  EXPECT_TRUE(isGateSymbol(PrimitiveType::Xor));
  EXPECT_FALSE(isGateSymbol(PrimitiveType::Dff));
  auto g = symbolGeometry(PrimitiveType::And, {"A", "B"}, {"Y", "YN"});
  EXPECT_EQ(g.leftY, (std::vector<float>{20.f, 40.f}));
  EXPECT_EQ(g.rightY, (std::vector<float>{20.f, 40.f}));
}

// ---------------------------------------------------------------------------
// layeredPlacement
// ---------------------------------------------------------------------------

namespace {

// A net from `from`'s output pin (at `dyOut`) to each `to` input pin (dyIn).
PlaceNet link(int from, std::vector<int> to, float dyOut = 20.f, float dyIn = 20.f) {
  PlaceNet n;
  n.drivers.push_back({from, dyOut});
  for (int t : to) n.receivers.push_back({t, dyIn});
  return n;
}

void expectNoOverlap(const std::vector<PlaceNode>& nodes, const Placement& p) {
  for (size_t i = 0; i < nodes.size(); ++i)
    for (size_t j = i + 1; j < nodes.size(); ++j) {
      InstanceShape a = box(1, {}, p.pos[i].x, p.pos[i].y, nodes[i].w, nodes[i].h);
      InstanceShape b = box(2, {}, p.pos[j].x, p.pos[j].y, nodes[j].w, nodes[j].h);
      EXPECT_FALSE(overlaps(a, b)) << i << " vs " << j;
    }
}

} // namespace

TEST(LayeredPlacement, AChainRunsLeftToRightOnOneStraightLine) {
  std::vector<PlaceNode> nodes(3, {kBoxW, kBoxH});
  auto p = layeredPlacement(nodes, {link(0, {1}), link(1, {2})});

  EXPECT_EQ(p.level, (std::vector<int>{0, 1, 2}));
  EXPECT_EQ(p.pos[0].x, kLeftMargin);
  EXPECT_LT(p.pos[0].x + kBoxW, p.pos[1].x);
  EXPECT_LT(p.pos[1].x + kBoxW, p.pos[2].x);
  EXPECT_EQ(p.pos[0].y, p.pos[1].y);
  EXPECT_EQ(p.pos[1].y, p.pos[2].y);
}

TEST(LayeredPlacement, AColumnIsAsWideAsItsWidestFootprint) {
  // Node 0's name makes its footprint 200 wide: node 1 starts after it.
  std::vector<PlaceNode> nodes{{200.f, kBoxH}, {kBoxW, kBoxH}};
  auto p = layeredPlacement(nodes, {link(0, {1})});
  EXPECT_GE(p.pos[1].x, p.pos[0].x + 200.f + kChannelMin);
}

TEST(LayeredPlacement, ReceiversPinIsLevelWithItsDriversPin) {
  // Driver output at 20 from its top, receiver input at 50 from its top.
  std::vector<PlaceNode> nodes = {{kBoxW, kBoxH}, {kBoxW, 80.f}};
  auto p = layeredPlacement(nodes, {link(0, {1}, 20.f, 50.f)});
  EXPECT_EQ(p.pos[0].y + 20.f, p.pos[1].y + 50.f);
}

TEST(LayeredPlacement, AFanInsDriversStackWithoutOverlapInPinOrder) {
  // a -> g.A (top pin), b -> g.B (bottom pin): a ends up above b.
  std::vector<PlaceNode> nodes(3, {kBoxW, kBoxH});
  PlaceNet toA = link(0, {2}, 20.f, 10.f), toB = link(1, {2}, 20.f, 30.f);
  auto p = layeredPlacement(nodes, {toB, toA});  // net order must not matter
  EXPECT_EQ(p.level[0], p.level[1]);
  EXPECT_LT(p.pos[0].y, p.pos[1].y);
  expectNoOverlap(nodes, p);
}

TEST(LayeredPlacement, ASourceSitsNextToTheNodeItFeeds) {
  // 0 -> 1 -> 2 -> 3, and a side input 4 -> 3.
  std::vector<PlaceNode> nodes(5, {kBoxW, kBoxH});
  auto p = layeredPlacement(nodes, {link(0, {1}), link(1, {2}), link(2, {3}), link(4, {3})});
  EXPECT_EQ(p.level[3], 3);
  EXPECT_EQ(p.level[4], 2);
}

TEST(LayeredPlacement, AFeedbackLoopStillFlowsLeftToRight) {
  // A flop and a gate feeding each other.
  std::vector<PlaceNode> nodes(2, {kBoxW, kBoxH});
  auto p = layeredPlacement(nodes, {link(0, {1}), link(1, {0})});
  EXPECT_NE(p.level[0], p.level[1]);
  expectNoOverlap(nodes, p);
}

TEST(LayeredPlacement, UnconnectedNodesArePlacedApart) {
  std::vector<PlaceNode> nodes(3, {kBoxW, kBoxH});
  auto p = layeredPlacement(nodes, {});
  EXPECT_EQ(p.level, (std::vector<int>{0, 0, 0}));
  expectNoOverlap(nodes, p);
}

TEST(LayeredPlacement, AChannelWidensWithTheNetsCrossingIt) {
  std::vector<PlaceNode> nodes(6, {kBoxW, kBoxH});
  auto one  = layeredPlacement(nodes, {link(0, {3})});
  auto many = layeredPlacement(nodes, {link(0, {3}), link(1, {4}), link(2, {5})});
  EXPECT_GT(many.pos[3].x, one.pos[3].x);
}

TEST(LayeredPlacement, ALargerRandomGraphHasNoOverlapsAndIsDeterministic) {
  std::vector<PlaceNode> nodes;
  std::vector<PlaceNet> nets;
  unsigned seed = 7;
  auto rnd = [&](unsigned n) { seed = seed * 1103515245u + 12345u; return int((seed >> 16) % n); };
  for (int i = 0; i < 60; ++i) {
    nodes.push_back({kBoxW + 10.f * float(rnd(4)), kBoxH + kPinPitch * float(rnd(3))});
    if (i > 0) nets.push_back(link(rnd(unsigned(i)), {i}, 20.f, 20.f));
    if (i > 2) nets.push_back(link(rnd(unsigned(i)), {i}, 20.f, 40.f));
  }
  auto p = layeredPlacement(nodes, nets);
  expectNoOverlap(nodes, p);
  for (const auto& net : nets)  // every edge points right
    EXPECT_LT(p.pos[net.drivers[0].node].x, p.pos[net.receivers[0].node].x);
  auto again = layeredPlacement(nodes, nets);
  for (size_t i = 0; i < nodes.size(); ++i) {
    EXPECT_EQ(p.pos[i].x, again.pos[i].x);
    EXPECT_EQ(p.pos[i].y, again.pos[i].y);
  }
}

TEST(StackPorts, PortsKeepTheirHeightUnlessCrowded) {
  EXPECT_EQ(stackPorts({0.f, 100.f}), (std::vector<float>{0.f, 100.f}));
  auto ys = stackPorts({50.f, 50.f, 7.f});
  EXPECT_EQ(ys[2], 7.f);  // not snapped: level with its pin
  EXPECT_GE(ys[0] - ys[2], kPinPitch - 1e-3f);
  EXPECT_GE(ys[1] - ys[0], kPinPitch - 1e-3f);
}

// ---------------------------------------------------------------------------
// routeNets
// ---------------------------------------------------------------------------

namespace {

bool isOrthogonal(const RouteSegment& s) {
  return std::abs(s.a.x - s.b.x) < 1e-3f || std::abs(s.a.y - s.b.y) < 1e-3f;
}

bool crosses(const RouteSegment& s, const RouteRect& r) {
  float x0 = std::min(s.a.x, s.b.x), x1 = std::max(s.a.x, s.b.x);
  float y0 = std::min(s.a.y, s.b.y), y1 = std::max(s.a.y, s.b.y);
  return x1 > r.x0 + 0.5f && x0 < r.x1 - 0.5f && y1 > r.y0 + 0.5f && y0 < r.y1 - 0.5f;
}

// The x of the vertical segment of `r` in (x0, x1), if any.
float trunkX(const RoutedNet& r, float x0, float x1) {
  for (const auto& s : r.segments)
    if (std::abs(s.a.x - s.b.x) < 1e-3f && std::abs(s.a.y - s.b.y) > 1e-3f && s.a.x > x0 && s.a.x < x1)
      return s.a.x;
  return NAN;
}

// Two columns of boxes: [0,40] and [100,140].
const std::vector<RouteRect> kTwoColumns = {{0.f, 0.f, 40.f, 200.f}, {100.f, 0.f, 140.f, 200.f}};

} // namespace

TEST(RouteNets, AlignedPinsGetAStraightWire) {
  auto r = routeNets({{{{40.f, 50.f}, true}, {{{100.f, 50.f}, false}}}}, kTwoColumns);
  ASSERT_EQ(r.size(), 1u);
  for (const auto& s : r[0].segments) {
    EXPECT_FLOAT_EQ(s.a.y, 50.f);
    EXPECT_FLOAT_EQ(s.b.y, 50.f);
  }
  EXPECT_TRUE(r[0].junctions.empty());
}

TEST(RouteNets, AFanOutIsOneTrunkWithAJunctionAtEachBranch) {
  RouteNet net{{{40.f, 50.f}, true},
               {{{100.f, 10.f}, false}, {{100.f, 50.f}, false}, {{100.f, 90.f}, false}, {{100.f, 130.f}, false}}};
  auto r = routeNets({net}, kTwoColumns);
  int verticals = 0;
  for (const auto& s : r[0].segments) {
    EXPECT_TRUE(isOrthogonal(s));
    if (std::abs(s.a.x - s.b.x) < 1e-3f) ++verticals;
  }
  EXPECT_EQ(verticals, 1);
  // Branches at 50 (where the driver joins too) and 90; 10 and 130 are corners.
  ASSERT_EQ(r[0].junctions.size(), 2u);
  EXPECT_FLOAT_EQ(r[0].junctions[0].y + r[0].junctions[1].y, 50.f + 90.f);
}

TEST(RouteNets, NetsSharingAChannelGetTheirOwnTrack) {
  RouteNet a{{{40.f, 20.f}, true}, {{{100.f, 120.f}, false}}};
  RouteNet b{{{40.f, 60.f}, true}, {{{100.f, 160.f}, false}}};
  auto r = routeNets({a, b}, kTwoColumns);
  float xa = trunkX(r[0], 40.f, 100.f), xb = trunkX(r[1], 40.f, 100.f);
  ASSERT_FALSE(std::isnan(xa) || std::isnan(xb));
  EXPECT_NE(xa, xb);
}

TEST(RouteNets, AStubNeverOverlapsAnotherNetsBranchAtTheSameHeight) {
  // Net a leaves its driver at y=50; net b enters a receiver at y=50 too. a's
  // stub runs from the left edge to a's trunk, b's branch from b's trunk to
  // the right edge: they only stay apart if a's trunk is left of b's.
  RouteNet a{{{40.f, 50.f}, true}, {{{100.f, 90.f}, false}}};
  RouteNet b{{{40.f, 10.f}, true}, {{{100.f, 50.f}, false}}};
  for (bool swapped : {false, true}) {
    auto r = swapped ? routeNets({b, a}, kTwoColumns) : routeNets({a, b}, kTwoColumns);
    float xa = trunkX(r[swapped ? 1 : 0], 40.f, 100.f), xb = trunkX(r[swapped ? 0 : 1], 40.f, 100.f);
    EXPECT_LT(xa, xb) << "swapped=" << swapped;
  }
}

TEST(RouteNets, ASpineDetoursAroundABoxInTheWay) {
  // Driver in column 0, receiver in column 2, a box in column 1 at the
  // driver's height.
  std::vector<RouteRect> boxes = {{0.f, 0.f, 40.f, 100.f}, {100.f, 30.f, 140.f, 70.f},
                                  {200.f, 0.f, 240.f, 100.f}};
  auto r = routeNets({{{{40.f, 50.f}, true}, {{{200.f, 50.f}, false}}}}, boxes);
  for (const auto& s : r[0].segments) {
    EXPECT_TRUE(isOrthogonal(s));
    for (const auto& b : boxes) EXPECT_FALSE(crosses(s, b));
  }
}

TEST(RouteNets, ATrunkStaysRightOfANameInItsChannel) {
  // A box with a name row reaching 100 past its right edge: the channel
  // (and so every trunk) starts after the name, not after the box.
  const RouteRect box{0.f, 40.f, 60.f, 80.f}, name{0.f, 20.f, 160.f, 40.f};
  const RouteRect rcv{250.f, 0.f, 310.f, 120.f};
  auto r = routeNets({{{{60.f, 60.f}, true}, {{{250.f, 100.f}, false}}}}, {box, name, rcv});
  ASSERT_EQ(r.size(), 1u);
  for (const auto& s : r[0].segments)
    if (std::abs(s.a.x - s.b.x) < 0.5f && std::abs(s.a.y - s.b.y) > 0.5f)
      EXPECT_GT(s.a.x, name.x1);
}

TEST(RouteNets, TwoSpinesDoNotShareAHeight) {
  // Both nets have to detour around the middle box, the same way.
  std::vector<RouteRect> boxes = {{0.f, 0.f, 40.f, 100.f}, {100.f, 0.f, 140.f, 100.f},
                                  {200.f, 0.f, 240.f, 100.f}};
  RouteNet a{{{40.f, 20.f}, true}, {{{200.f, 20.f}, false}}};
  RouteNet b{{{40.f, 40.f}, true}, {{{200.f, 40.f}, false}}};
  auto r = routeNets({a, b}, boxes);
  auto spineY = [](const RoutedNet& n) {
    for (const auto& s : n.segments)
      if (std::abs(s.a.y - s.b.y) < 1e-3f && std::min(s.a.x, s.b.x) < 100.f && std::max(s.a.x, s.b.x) > 140.f)
        return s.a.y;
    return NAN;
  };
  ASSERT_FALSE(std::isnan(spineY(r[0])) || std::isnan(spineY(r[1])));
  EXPECT_NE(spineY(r[0]), spineY(r[1]));
}

// ---------------------------------------------------------------------------
// layoutHierarchyGroups
// ---------------------------------------------------------------------------

namespace {

// Leaves at logic level `col` (as layeredPlacement would have placed them).
struct LeafSpec {
  InstancePath path; std::vector<std::string> models; int col; float y;
  LeafSpec(std::vector<std::string> names, std::vector<std::string> models, int col, float y)
      : path(P(names)), models(std::move(models)), col(col), y(y) {}
  struct ByRef {};
  LeafSpec(ByRef, InstancePath path, std::vector<std::string> models, int col, float y)
      : path(std::move(path)), models(std::move(models)), col(col), y(y) {}
};

struct HierFixture {
  std::vector<InstanceShape>         shapes;
  std::map<InstancePath, LeafHier>   leafHier;
  std::map<InstancePath, int>        pathToInstId;
  int                                nextInstId = 1;

  explicit HierFixture(const std::vector<LeafSpec>& leaves) {
    for (const auto& l : leaves) {
      int id = nextInstId++;
      shapes.push_back(box(id, l.path, kLeftMargin + float(l.col) * 200.f, l.y));
      leafHier[l.path]     = {l.path, l.models, l.col};
      pathToInstId[l.path] = id;
    }
  }
  std::vector<HierFrame> run() {
    return layoutHierarchyGroups(leafHier, shapes, pathToInstId, nextInstId, charWidth);
  }
};

const HierFrame& frameAt(const std::vector<HierFrame>& frames, const InstancePath& path) {
  for (const auto& f : frames) if (f.shape.path == path) return f;
  ADD_FAILURE() << "no frame for " << displayPath(path);
  static HierFrame none;
  return none;
}
const HierFrame& frame(const std::vector<HierFrame>& frames, const std::vector<std::string>& names) {
  return frameAt(frames, P(names));
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
  EXPECT_EQ(frames[0].shape.path, P({"core"}));
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
  // Map order (by id) is the reverse of the y order on purpose.
  const InstancePath a{{1, "core"}, {2, "a"}}, b{{1, "core"}, {3, "b"}};
  HierFixture fx({{LeafSpec::ByRef{}, b, {}, 0, 0.f}, {LeafSpec::ByRef{}, a, {}, 0, 3 * kRowStep}});
  fx.run();
  EXPECT_LT(byRef(fx.shapes, b).y, byRef(fx.shapes, a).y);
  EXPECT_FALSE(overlaps(byRef(fx.shapes, a), byRef(fx.shapes, b)));
}

// Anonymous instances all have the name "": two anonymous sibling modules
// are two frames (keyed by id), not one merged frame.
TEST(HierarchyGroups, AnonymousSiblingModulesGetTheirOwnFrames) {
  const InstancePath g0{{0, ""}, {5, "g"}}, g1{{1, ""}, {5, "g"}}, nested{{1, ""}, {0, ""}, {2, ""}};
  HierFixture fx({{LeafSpec::ByRef{}, g0, {"mid", "INV"}, 0, 0.f},
                  {LeafSpec::ByRef{}, g1, {"mid", "INV"}, 0, kRowStep},
                  {LeafSpec::ByRef{}, nested, {"mid", "sub", "BUF"}, 1, kRowStep}});
  auto frames = fx.run();

  ASSERT_EQ(frames.size(), 3u);
  const auto& m0  = frameAt(frames, InstancePath{{0, ""}}).shape;
  const auto& m1  = frameAt(frames, InstancePath{{1, ""}}).shape;
  const auto& sub = frameAt(frames, InstancePath{{1, ""}, {0, ""}}).shape;
  EXPECT_EQ(m0.name, "<#0> (mid)");
  EXPECT_EQ(m1.name, "<#1> (mid)");
  EXPECT_EQ(sub.name, "<#0> (sub)");
  EXPECT_FALSE(overlaps(m0, m1));
  EXPECT_TRUE(contains(m0, byRef(fx.shapes, g0)));
  EXPECT_TRUE(contains(m1, byRef(fx.shapes, g1)));
  EXPECT_TRUE(contains(sub, byRef(fx.shapes, nested)));
  EXPECT_EQ(byRef(fx.shapes, nested).label, "<#2>");
}

TEST(HierarchyGroups, NestedModulesNestTheirFramesParentFirst) {
  HierFixture fx({{{"top_a", "sub", "g"}, {"A", "S", "INV"}, 0, 0.f},
                  {{"top_a", "h"}, {"A", "BUF"}, 1, 0.f}});
  auto frames = fx.run();

  ASSERT_EQ(frames.size(), 2u);
  EXPECT_EQ(frames[0].shape.path, P({"top_a"}));  // parent first: drawn underneath
  EXPECT_EQ(frames[1].shape.path, P({"top_a", "sub"}));
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
  for (const InstancePath& path : {P({"m1", "a"}), P({"m1", "b"})}) {
    EXPECT_TRUE(contains(m1, byRef(fx.shapes, path))) << displayPath(path);
    EXPECT_FALSE(overlaps(m2, byRef(fx.shapes, path))) << displayPath(path);
  }
  for (const InstancePath& path : {P({"m2", "c"}), P({"m2", "d"})}) {
    EXPECT_TRUE(contains(m2, byRef(fx.shapes, path))) << displayPath(path);
    EXPECT_FALSE(overlaps(m1, byRef(fx.shapes, path))) << displayPath(path);
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

TEST(HierarchyGroups, ALongLeafNameWidensItsColumn) {
  HierFixture fx({{{"core", "a_long_register_name_q_reg"}, {}, 0, 0.f},
                  {{"core", "g"}, {}, 1, 0.f}});
  fx.run();
  const auto& reg = byPath(fx.shapes, {"core", "a_long_register_name_q_reg"});
  const auto& g   = byPath(fx.shapes, {"core", "g"});
  ASSERT_GT(charWidth(reg.label), reg.w);
  // The name (the leaf name, as the frame names the module) ends before
  // the next column, with the usual gap.
  EXPECT_GE(g.x, reg.x + charWidth(reg.label) + kGroupColGap);
}

TEST(HierarchyGroups, LeavesWithoutAShapeAreIgnored) {
  HierFixture fx({{{"core", "g"}, {}, 0, 0.f}});
  fx.leafHier[P({"ghost", "x"})] = {P({"ghost", "x"}), {}};   // no pathToInstId entry
  auto frames = fx.run();
  ASSERT_EQ(frames.size(), 1u);
  EXPECT_EQ(frames[0].shape.path, P({"core"}));
}

TEST(HierarchyGroups, LeavesKeepTheHeightsTheirPinsWereAlignedAt) {
  // g2's pin was aligned 20 below g1's by the layered placement.
  HierFixture fx({{{"core", "g1"}, {}, 0, 100.f}, {{"core", "g2"}, {}, 1, 120.f}});
  fx.run();
  EXPECT_EQ(byPath(fx.shapes, {"core", "g2"}).y - byPath(fx.shapes, {"core", "g1"}).y, 20.f);
}

TEST(HierarchyGroups, ANestedFrameKeepsItsLeafLevelWithAShallowerOne) {
  HierFixture fx({{{"m", "sub", "g"}, {}, 0, 100.f}, {{"m", "h"}, {}, 1, 100.f}});
  fx.run();
  EXPECT_EQ(byPath(fx.shapes, {"m", "sub", "g"}).y, byPath(fx.shapes, {"m", "h"}).y);
}

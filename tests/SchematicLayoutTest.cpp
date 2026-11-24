#include "SchematicLayout.h"

#include <algorithm>
#include <cmath>
#include <set>
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
// Net labels
// ---------------------------------------------------------------------------

namespace {
Port labeledPin(int id, float lx, float ly, const std::string& label) {
  Port p;
  p.id = id; p.name = "Q"; p.lx = lx; p.ly = ly;
  p.netLabel = label; p.netLabelW = charWidth(label);
  return p;
}
bool overlaps(const RouteRect& a, const RouteRect& b) {
  return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1;
}
} // namespace

TEST(NetLabels, ALabelSitsAboveItsWireRightPastThePin) {
  InstanceShape s = box(1, P({"u1"}), 100.f, 50.f);
  s.ports.push_back(labeledPin(7, 0.5f, 0.f, "en_reg -> 4 loads"));  // right side, mid-height
  const auto places = netLabelPlaces(s, charWidth);
  ASSERT_EQ(places.size(), 1u);
  const auto& l = places[0];
  const ImVec2 pin = portAnchor(s, s.ports[0]);
  EXPECT_EQ(l.portId, 7);
  EXPECT_EQ(l.at.x, pin.x + kNetLabelGap);
  EXPECT_EQ(l.at.y, pin.y - kNetLabelLift);   // bottom line just over the wire
  // Its rect starts at the box's edge (joining the box's span: the trunk
  // goes after it) and ends with the text.
  EXPECT_EQ(l.rect.x0, s.x + s.w);
  EXPECT_EQ(l.rect.x1, l.at.x + charWidth("en_reg -> 4 loads"));
  EXPECT_EQ(l.rect.y1, l.at.y);
  EXPECT_EQ(l.rect.y0, l.at.y - kNetLabelH);
  // Clear of the box, its name and its model, and in its footprint.
  s.showModel = true; s.model = "FDRE";
  const RouteRect boxRect{ s.x, s.y, s.x + s.w, s.y + s.h };
  EXPECT_FALSE(overlaps(l.rect, RouteRect{ boxRect.x0, boxRect.y0, boxRect.x1 - 0.01f, boxRect.y1 }));
  EXPECT_FALSE(overlaps(l.rect, *nameRect(s, charWidth)));
  EXPECT_FALSE(overlaps(l.rect, *modelRect(s, charWidth)));
  EXPECT_EQ(footprintWidth(s, charWidth), l.rect.x1 - s.x);
}

TEST(NetLabels, ALabelLevelWithTheNameRowGoesPastTheName) {
  // A small gate whose output is level with its name's row: a long name
  // overhangs the symbol, and the label goes after it.
  InstanceShape s = box(1, P({"a_long_gate_name"}), 0.f, 100.f, 30.f, 20.f);
  s.ports.push_back(labeledPin(3, 0.5f, 0.f, "net"));
  const auto name = nameRect(s, charWidth);
  ASSERT_TRUE(name);
  ASSERT_GT(name->x1, s.x + s.w);
  const auto places = netLabelPlaces(s, charWidth);
  ASSERT_EQ(places.size(), 1u);
  EXPECT_EQ(places[0].at.x, name->x1 + kNetLabelGap);
  EXPECT_EQ(footprintWidth(s, charWidth), places[0].rect.x1 - s.x);
}

TEST(NetLabels, OnlyLabeledRightSidePinsHaveOne) {
  InstanceShape s = box(1, P({"u1"}), 0.f, 0.f);
  s.ports.push_back(labeledPin(1, -0.5f, 0.f, "an input's net"));  // left side
  Port plain; plain.id = 2; plain.lx = 0.5f;                          // no label
  s.ports.push_back(plain);
  EXPECT_TRUE(netLabelPlaces(s, charWidth).empty());
  EXPECT_EQ(footprintWidth(s, charWidth), kBoxW);
}

TEST(NetLabels, AnItemCarriesItsPinsNetName) {
  Equipotential eq{true, {topTerm("en", Direction::Input)}, {}};
  eq.terms[0].net = "en";
  eq.occurrences.push_back(occ({"u1"}, "Q", Direction::Output));
  eq.occurrences.back().term.net = "[3]";   // an unnamed bus's bit
  eq.occurrences.back().term.netId = 9;
  eq.occurrences.push_back(occ({"u2"}, "A", Direction::Input));  // an older host: no net
  std::vector<Item> drivers, receivers;
  buildItems(&eq, drivers, receivers);
  ASSERT_EQ(drivers.size(), 2u);
  EXPECT_EQ(drivers[0].netName, "en");
  EXPECT_EQ(drivers[1].netName, "<net #9>[3]");
  EXPECT_EQ(receivers[0].netName, "");
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

TEST(Symbols, TieCellIsASmallSquareWithItsOutputCentered) {
  for (auto type : {PrimitiveType::Tie0, PrimitiveType::Tie1}) {
    EXPECT_TRUE(isGateSymbol(type));
    auto g = symbolGeometry(type, {}, {"Z"});
    EXPECT_EQ(g.w, g.h);
    EXPECT_TRUE(g.leftY.empty());
    EXPECT_EQ(g.rightY, (std::vector<float>{0.5f * g.h}));
  }
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

TEST(Symbols, MuxInputsRunDownFromTheTopAndItsOutputIsCentered) {
  auto g = symbolGeometry(PrimitiveType::Mux, {"A", "B", "S"}, {"Y"});
  EXPECT_FALSE(isGateSymbol(PrimitiveType::Mux));
  EXPECT_EQ(g.leftY, (std::vector<float>{20.f, 40.f, 60.f}));
  EXPECT_EQ(g.rightY, (std::vector<float>{0.5f * g.h}));
}

TEST(Symbols, MuxSlantStopsShortOfItsOutermostOutput) {
  for (size_t nL = 1; nL <= 6; ++nL)
    for (size_t nR = 1; nR <= nL; ++nR) {
      auto g = symbolGeometry(PrimitiveType::Mux, std::vector<std::string>(nL, "A"),
                              std::vector<std::string>(nR, "Y"));
      EXPECT_TRUE(onGrid(g.w) && onGrid(g.h)) << nL << "x" << nR;
      for (float y : g.rightY) EXPECT_TRUE(onGrid(y)) << nL << "x" << nR;
      const float slant = muxSlant(g.h, nR);
      EXPECT_GT(slant, 0.f) << nL << "x" << nR;
      EXPECT_LT(slant, g.rightY.front()) << nL << "x" << nR;
      EXPECT_GT(g.h - slant, g.rightY.back()) << nL << "x" << nR;
    }
}

TEST(Symbols, OperatorBoxHasRoomForItsGlyph) {
  for (auto type : {PrimitiveType::Add, PrimitiveType::Sub, PrimitiveType::Mul,
                    PrimitiveType::Shl, PrimitiveType::Shr, PrimitiveType::Ashr,
                    PrimitiveType::DivMod}) {
    EXPECT_TRUE(isOperatorSymbol(type));
    EXPECT_FALSE(isGateSymbol(type));
    auto op  = symbolGeometry(type, {"A", "B"}, {"Y"});
    auto box = symbolGeometry(PrimitiveType::Unknown, {"A", "B"}, {"Y"});
    EXPECT_GE(op.w, box.w + kOperatorGlyphW - kGrid) << int(type);
    EXPECT_TRUE(onGrid(op.w)) << int(type);
    EXPECT_EQ(op.leftY, box.leftY);
  }
  EXPECT_FALSE(isOperatorSymbol(PrimitiveType::Mux));
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

TEST(LayeredPlacement, ANodeOnlyFeedingOutputsSitsInTheLastColumn) {
  // Chain 0 -> 1 -> 2 -> 3 (3 drives an output). A tie cell 4 on an output,
  // a gate 5 fed by 0 on an output, and 1 feeding both 2 and an output.
  std::vector<PlaceNode> nodes(6, {kBoxW, kBoxH});
  nodes[1].drivesOutput = nodes[3].drivesOutput = true;
  nodes[4].drivesOutput = nodes[5].drivesOutput = true;
  auto p = layeredPlacement(nodes, {link(0, {1, 5}), link(1, {2}), link(2, {3})});
  EXPECT_EQ(p.level[3], 3);
  EXPECT_EQ(p.level[4], 3);  // not column 0, a sheet's width from its port
  EXPECT_EQ(p.level[5], 3);
  EXPECT_EQ(p.level[1], 1);  // also feeds 2: stays in its cone
  expectNoOverlap(nodes, p);
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

// --- Stability: hints from an earlier layout -------------------------------

namespace {
// Nodes [0, count) hinted where `was` put them.
std::vector<PlaceNode> hinted(std::vector<PlaceNode> nodes, const Placement& was, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    nodes[i].hinted    = true;
    nodes[i].hintLevel = was.logicLevel[i];
    nodes[i].hintY     = was.pos[i].y;
  }
  return nodes;
}
// Columns of hinted nodes keep their order and stay one column each: the
// level differences between them never shrink, and are unchanged when
// `exact`.
void expectColumnsKept(const Placement& was, const Placement& now, size_t count, bool exact) {
  for (size_t i = 0; i < count; ++i)
    for (size_t j = 0; j < count; ++j) {
      const int before = was.logicLevel[j] - was.logicLevel[i];
      const int after  = now.logicLevel[j] - now.logicLevel[i];
      if (before == 0) { EXPECT_EQ(after, 0) << i << "," << j; continue; }
      if (exact) { EXPECT_EQ(after, before) << i << "," << j; continue; }
      if (before > 0) EXPECT_GE(after, before) << i << "," << j;
    }
}
// The vertical offsets between hinted nodes are unchanged.
void expectHeightsKept(const Placement& was, const Placement& now, size_t count) {
  for (size_t i = 1; i < count; ++i)
    EXPECT_FLOAT_EQ(now.pos[i].y - now.pos[0].y, was.pos[i].y - was.pos[0].y) << i;
}

// The random graph above, with `n` nodes.
void randomGraph(int n, std::vector<PlaceNode>& nodes, std::vector<PlaceNet>& nets) {
  unsigned seed = 7;
  auto rnd = [&](unsigned k) { seed = seed * 1103515245u + 12345u; return int((seed >> 16) % k); };
  for (int i = 0; i < n; ++i) {
    nodes.push_back({kBoxW + 10.f * float(rnd(4)), kBoxH + kPinPitch * float(rnd(3))});
    if (i > 0) nets.push_back(link(rnd(unsigned(i)), {i}, 20.f, 20.f));
    if (i > 2) nets.push_back(link(rnd(unsigned(i)), {i}, 20.f, 40.f));
  }
}
} // namespace

TEST(StablePlacement, ALayoutHintedWithItselfStaysTheSame) {
  std::vector<PlaceNode> nodes;
  std::vector<PlaceNet> nets;
  randomGraph(40, nodes, nets);
  const Placement was = layeredPlacement(nodes, nets);
  const Placement now = layeredPlacement(hinted(nodes, was, nodes.size()), nets);
  EXPECT_EQ(now.logicLevel, was.logicLevel);
  for (size_t i = 0; i < nodes.size(); ++i) {
    EXPECT_EQ(now.pos[i].x, was.pos[i].x) << i;
    EXPECT_EQ(now.pos[i].y, was.pos[i].y) << i;
  }
}

TEST(StablePlacement, ANewDriverOfTheFirstNodeMovesTheOthersTogether) {
  // 0 -> 1 -> 2, then 3 -> 0 arrives: everything shown moves one column
  // right, as one, and keeps its heights.
  std::vector<PlaceNode> nodes(4, {kBoxW, kBoxH});
  const std::vector<PlaceNet> before{link(0, {1}), link(1, {2})};
  std::vector<PlaceNode> three(nodes.begin(), nodes.begin() + 3);
  const Placement was = layeredPlacement(three, before);

  std::vector<PlaceNet> after = before;
  after.push_back(link(3, {0}));
  const Placement now = layeredPlacement(hinted(nodes, was, 3), after);
  EXPECT_EQ(now.logicLevel, (std::vector<int>{1, 2, 3, 0}));
  expectColumnsKept(was, now, 3, true);
  expectHeightsKept(was, now, 3);
  EXPECT_EQ(now.pos[3].y + 20.f, now.pos[0].y + 20.f);  // the new one lines up
}

// What the issue's video showed: a net between two boxes already shown
// (here 2 -> 0, against their columns) used to move one of them, and
// everything after it, across the sheet. Hinted, it's a feedback wire and
// nothing moves.
TEST(StablePlacement, ANetAgainstTheShownColumnsMovesNothing) {
  std::vector<PlaceNode> nodes(4, {kBoxW, kBoxH});
  const std::vector<PlaceNet> before{link(0, {1}), link(2, {3})};
  const Placement was = layeredPlacement(nodes, before);
  ASSERT_LT(was.logicLevel[0], was.logicLevel[1]);
  ASSERT_LT(was.logicLevel[2], was.logicLevel[3]);

  std::vector<PlaceNet> after = before;
  after.push_back(link(3, {0}));
  // Unhinted, 0 and 1 go right of 3.
  const Placement fresh = layeredPlacement(nodes, after);
  ASSERT_GT(fresh.logicLevel[0], fresh.logicLevel[3]);

  const Placement now = layeredPlacement(hinted(nodes, was, 4), after);
  EXPECT_EQ(now.logicLevel, was.logicLevel);
  for (size_t i = 0; i < 4; ++i) {
    EXPECT_EQ(now.pos[i].y, was.pos[i].y) << i;
    // Only the channel the new wire crosses widens, by its track.
    EXPECT_GE(now.pos[i].x, was.pos[i].x) << i;
    EXPECT_LE(now.pos[i].x, was.pos[i].x + kTrackPitch) << i;
  }
}

TEST(StablePlacement, ShownNodesKeepTheirOrderInAColumn) {
  // 0, 1, 2 drive the top, middle and bottom pins of 3: by barycenter they
  // stack 0, 1, 2. Hinted the other way up, they stay that way.
  std::vector<PlaceNode> nodes{{kBoxW, kBoxH}, {kBoxW, kBoxH}, {kBoxW, kBoxH}, {kBoxW, 120.f}};
  const std::vector<PlaceNet> nets{link(0, {3}, 20.f, 20.f), link(1, {3}, 20.f, 60.f),
                                   link(2, {3}, 20.f, 100.f)};
  const Placement fresh = layeredPlacement(nodes, nets);
  ASSERT_LT(fresh.pos[0].y, fresh.pos[1].y);
  ASSERT_LT(fresh.pos[1].y, fresh.pos[2].y);

  Placement flipped = fresh;
  std::swap(flipped.pos[0].y, flipped.pos[2].y);
  const Placement now = layeredPlacement(hinted(nodes, flipped, 4), nets);
  EXPECT_GT(now.pos[0].y, now.pos[1].y);
  EXPECT_GT(now.pos[1].y, now.pos[2].y);
  expectNoOverlap(nodes, now);
}

TEST(StablePlacement, ANodeBetweenTwoShownColumnsWidensTheGap) {
  // 0 -> 1 shown; then 0 -> 2 -> 1 arrives: 2 needs a column between them.
  std::vector<PlaceNode> nodes(3, {kBoxW, kBoxH});
  std::vector<PlaceNode> two(nodes.begin(), nodes.begin() + 2);
  const Placement was = layeredPlacement(two, {link(0, {1})});
  const Placement now = layeredPlacement(hinted(nodes, was, 2), {link(0, {1}), link(0, {2}), link(2, {1})});
  EXPECT_EQ(now.logicLevel, (std::vector<int>{0, 2, 1}));
  expectColumnsKept(was, now, 2, false);
  expectHeightsKept(was, now, 2);
}

TEST(StablePlacement, ANodeSqueezedIntoAColumnPushesItsNeighborsApart) {
  // 0 and 1 drive 3's top and bottom pins, shown close together; 2 arrives
  // driving its middle pin and lands between them.
  std::vector<PlaceNode> nodes{{kBoxW, kBoxH}, {kBoxW, kBoxH}, {kBoxW, kBoxH}, {kBoxW, 120.f}};
  std::vector<PlaceNode> shown{nodes[0], nodes[1], nodes[3]};
  const Placement was3 = layeredPlacement(shown, {link(0, {2}, 20.f, 20.f), link(1, {2}, 20.f, 100.f)});
  // As nodes 0, 1, 3 of the new graph.
  Placement was = was3;
  was.pos.insert(was.pos.begin() + 2, ImVec2());
  was.logicLevel.insert(was.logicLevel.begin() + 2, 0);
  std::vector<PlaceNode> hint = nodes;
  for (size_t i : {size_t(0), size_t(1), size_t(3)}) {
    hint[i].hinted    = true;
    hint[i].hintLevel = was.logicLevel[i];
    hint[i].hintY     = was.pos[i].y;
  }
  const Placement now = layeredPlacement(hint, {link(0, {3}, 20.f, 20.f), link(1, {3}, 20.f, 100.f),
                                                link(2, {3}, 20.f, 60.f)});
  EXPECT_LT(now.pos[0].y, now.pos[2].y);
  EXPECT_LT(now.pos[2].y, now.pos[1].y);
  expectNoOverlap(nodes, now);
}

TEST(StablePlacement, GrowingARandomGraphKeepsWhatWasShown) {
  std::vector<PlaceNode> all;
  std::vector<PlaceNet> allNets;
  randomGraph(60, all, allNets);
  // Grow it ten nodes at a time, each step hinted with the one before.
  Placement was;
  size_t shown = 0;
  for (size_t count = 20; count <= all.size(); count += 10) {
    std::vector<PlaceNode> nodes(all.begin(), all.begin() + long(count));
    std::vector<PlaceNet> nets;
    for (const auto& net : allNets)
      if (size_t(net.drivers[0].node) < count && size_t(net.receivers[0].node) < count) nets.push_back(net);
    const Placement now = layeredPlacement(shown ? hinted(nodes, was, shown) : nodes, nets);
    expectNoOverlap(nodes, now);
    if (shown) {
      expectColumnsKept(was, now, shown, false);
      // Top-to-bottom order in a column is kept.
      for (size_t i = 0; i < shown; ++i)
        for (size_t j = 0; j < shown; ++j)
          if (was.logicLevel[i] == was.logicLevel[j] && was.pos[i].y < was.pos[j].y)
            EXPECT_LT(now.pos[i].y, now.pos[j].y) << count << ": " << i << " above " << j;
    }
    was = now;
    shown = count;
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

TEST(RouteNets, ATrunkLevelWithANameStaysRightOfIt) {
  // A box with a name row reaching 100 past its right edge, and a receiver
  // above: the trunk runs level with the name, so it starts after it.
  const RouteRect box{0.f, 40.f, 60.f, 80.f}, name{0.f, 20.f, 160.f, 40.f};
  const RouteRect rcv{250.f, 0.f, 310.f, 120.f};
  auto r = routeNets({{{{60.f, 60.f}, true}, {{{250.f, 10.f}, false}}}}, {box, name, rcv});
  ASSERT_EQ(r.size(), 1u);
  for (const auto& s : r[0].segments)
    if (std::abs(s.a.x - s.b.x) < 0.5f && std::abs(s.a.y - s.b.y) > 0.5f)
      EXPECT_GT(s.a.x, name.x1);
}

TEST(RouteNets, ATrunkBelowANameNeverCrossesIt) {
  // The same name, a receiver below: the trunk may run under the name's
  // span, never through the name.
  const RouteRect box{0.f, 40.f, 60.f, 80.f}, name{0.f, 20.f, 160.f, 40.f};
  const RouteRect rcv{250.f, 0.f, 310.f, 120.f};
  auto r = routeNets({{{{60.f, 60.f}, true}, {{{250.f, 100.f}, false}}}}, {box, name, rcv});
  ASSERT_EQ(r.size(), 1u);
  for (const auto& s : r[0].segments) EXPECT_FALSE(crosses(s, name));
}

TEST(RouteNets, ABoxAtAnotherHeightDoesNotSqueezeATrunk) {
  // A wide box far above makes the strip free of boxes over the whole sheet
  // a 3-wide sliver next to the receivers' column. The trunk, which runs
  // nowhere near that box, still keeps clear of the receivers' pin ticks.
  const RouteRect far{0.f, 0.f, 97.f, 40.f};
  const RouteRect drv{0.f, 200.f, 40.f, 240.f}, rcv{100.f, 200.f, 140.f, 300.f};
  auto r = routeNets({{{{40.f, 220.f}, true}, {{{100.f, 220.f}, false}, {{100.f, 280.f}, false}}}},
                     {far, drv, rcv});
  ASSERT_EQ(r.size(), 1u);
  const float x = trunkX(r[0], 40.f, 100.f);
  ASSERT_FALSE(std::isnan(x));
  EXPECT_GE(x, 40.f + kPinTick);
  EXPECT_LE(x, 100.f - kPinTick);
}

TEST(RouteNets, ACrowdedChannelKeepsItsTrunksClearOfThePinTicks) {
  // Twenty nets in one 60-wide channel, each with a receiver far below its
  // driver: twenty tracks. None may run under a pin's tick, which is drawn
  // kPinTick out of its box -- the wire would seem to meet the tick halfway.
  std::vector<RouteNet> nets;
  for (int i = 0; i < 20; ++i)
    nets.push_back({{{40.f, 10.f * float(i)}, true}, {{{100.f, 400.f + 10.f * float(i)}, false}}});
  const std::vector<RouteRect> boxes = {{0.f, 0.f, 40.f, 600.f}, {100.f, 0.f, 140.f, 600.f}};
  auto r = routeNets(nets, boxes);
  for (const auto& net : r) {
    const float x = trunkX(net, 40.f, 100.f);
    ASSERT_FALSE(std::isnan(x));
    EXPECT_GE(x, 40.f + kPinTick);
    EXPECT_LE(x, 100.f - kPinTick);
  }
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
  std::vector<HierFrame> run(const std::map<InstancePath, FrameRoom>& room = {},
                             const std::map<InstancePath, EmptyFrame>& empty = {}) {
    return layoutHierarchyGroups(leafHier, shapes, pathToInstId, nextInstId, charWidth, room, empty);
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

// ---------------------------------------------------------------------------
// Opened instances
// ---------------------------------------------------------------------------

namespace {

// A pin of `path`'s instance, with a term id (what pins are matched by).
InstTermOccurrence pinOf(std::vector<std::string> path, const std::string& term, unsigned termId,
                         Direction dir) {
  InstTermOccurrence o = occ(std::move(path), term, dir);
  o.term.child_id = termId;
  o.pathIds       = pathIds(o.path);
  return o;
}

Equipotential net(std::vector<BitTerm> terms, std::vector<InstTermOccurrence> occs) {
  Equipotential e;
  e.found       = true;
  e.terms       = std::move(terms);
  e.occurrences = std::move(occs);
  return e;
}

OpenedInstance opened(const std::vector<std::string>& path, std::vector<Equipotential> nets) {
  OpenedInstance oi;
  oi.path       = P(path);
  oi.pathModels = std::vector<std::string>(path.size(), "M");
  oi.nets       = std::move(nets);
  return oi;
}

bool hasPin(const Equipotential& e, const InstancePath& path, const std::string& term) {
  return std::any_of(e.occurrences.begin(), e.occurrences.end(), [&](const InstTermOccurrence& o) {
    return o.path == path && o.term.name == term;
  });
}

// The flags of `view` for `owner`'s pins, by pin name.
std::map<std::string, const BoundaryPin*> flagsOf(const OpenedView& view, const std::vector<std::string>& owner) {
  std::map<std::string, const BoundaryPin*> flags;
  for (const auto& [path, pin] : view.boundary)
    if (pin.owner == P(owner)) flags[pin.term.name] = &pin;
  return flags;
}

// u1.Q drives u2.A; u2 (opened) has g0: A -> g0.I, g0.O -> Y.
struct OpenedFixture {
  Equipotential shown = net({}, { pinOf({"u1"}, "Q", 1, Direction::Output),
                                  pinOf({"u2"}, "A", 20, Direction::Input) });
  std::map<InstancePath, OpenedInstance> open;
  OpenedFixture() {
    open[P({"u2"})] = opened({"u2"}, {
        net({ BitTerm{"A", 20, Direction::Input} }, { pinOf({"u2", "g0"}, "I", 0, Direction::Input) }),
        net({ BitTerm{"Y", 21, Direction::Output} }, { pinOf({"u2", "g0"}, "O", 1, Direction::Output) }) });
  }
};

} // namespace

TEST(OpenedInstances, AShownNetGoesOnInsideThroughTheOpenedPin) {
  OpenedFixture fx;
  const OpenedView view = openInstances({ &fx.shown }, fx.open);
  ASSERT_EQ(view.nets.size(), 2u);
  const Equipotential& joined = view.nets[0];
  EXPECT_TRUE(hasPin(joined, P({"u1"}), "Q"));
  EXPECT_TRUE(hasPin(joined, P({"u2", "g0"}), "I"));
  // The opened instance has no box: its own pin is gone.
  EXPECT_FALSE(hasPin(joined, P({"u2"}), "A"));
  EXPECT_EQ(joined.occurrences.size(), 2u);
  // Connected outside: no flag for A.
  EXPECT_EQ(flagsOf(view, {"u2"}).count("A"), 0u);
}

TEST(OpenedInstances, APinWithNothingOutsideInViewIsAFlag) {
  OpenedFixture fx;
  const OpenedView view = openInstances({ &fx.shown }, fx.open);
  const auto flags = flagsOf(view, {"u2"});
  ASSERT_EQ(flags.count("Y"), 1u);
  EXPECT_EQ(flags.at("Y")->term.direction, Direction::Output);
  // On the inside net, as an occurrence of a made-up path in u2 that
  // receives it (an output pin, seen from inside), addressed by u2's ids.
  const Equipotential& inside = view.nets[1];
  ASSERT_EQ(inside.occurrences.size(), 2u);
  const InstTermOccurrence& flag = inside.occurrences[1];
  ASSERT_EQ(flag.path.size(), 2u);
  EXPECT_EQ(InstancePath(flag.path.begin(), flag.path.end() - 1), P({"u2"}));
  EXPECT_GE(flag.path.back().id, kBoundaryIdBase);
  EXPECT_EQ(flag.pathIds, pathIds(P({"u2"})));
  EXPECT_EQ(flag.term.direction, Direction::Input);
  EXPECT_EQ(flag.term.child_id, 21u);
  EXPECT_TRUE(view.boundary.count(flag.path));
  EXPECT_TRUE(inside.direct);  // drawn plainly: nothing else shows it
}

TEST(OpenedInstances, AnOpenedInstanceAloneHasAFlagPerConnectedPin) {
  OpenedFixture fx;
  const OpenedView view = openInstances({}, fx.open);
  const auto flags = flagsOf(view, {"u2"});
  EXPECT_EQ(flags.size(), 2u);
  // An input pin drives the inside net.
  for (const auto& n : view.nets)
    for (const auto& o : n.occurrences)
      if (o.term.name == "A" && view.boundary.count(o.path))
        EXPECT_EQ(o.term.direction, Direction::Output);
}

TEST(OpenedInstances, ANetThroughAnInstanceKeepsBothFlags) {
  std::map<InstancePath, OpenedInstance> open;
  open[P({"ft"})] = opened({"ft"}, { net({ BitTerm{"a", 0, Direction::Input},
                                           BitTerm{"b", 1, Direction::Output} }, {}) });
  const OpenedView view = openInstances({}, open);
  ASSERT_EQ(view.nets.size(), 1u);
  EXPECT_EQ(view.nets[0].occurrences.size(), 2u);
  EXPECT_EQ(flagsOf(view, {"ft"}).size(), 2u);
}

TEST(OpenedInstances, AnInstanceOpenedInsideAnotherJoinsItsNets) {
  std::map<InstancePath, OpenedInstance> open;
  // x.a -> x/y.p; inside y: p -> x/y/z.A.
  open[P({"x"})] = opened({"x"}, { net({ BitTerm{"a", 0, Direction::Input} },
                                        { pinOf({"x", "y"}, "p", 5, Direction::Input) }) });
  open[P({"x", "y"})] = opened({"x", "y"}, { net({ BitTerm{"p", 5, Direction::Input} },
                                                  { pinOf({"x", "y", "z"}, "A", 0, Direction::Input) }) });
  const OpenedView view = openInstances({}, open);
  ASSERT_EQ(view.nets.size(), 1u);
  EXPECT_TRUE(hasPin(view.nets[0], P({"x", "y", "z"}), "A"));
  EXPECT_FALSE(hasPin(view.nets[0], P({"x", "y"}), "p"));
  // x's pin is where the net comes from; y's goes on outside it, in x.
  EXPECT_EQ(flagsOf(view, {"x"}).size(), 1u);
  EXPECT_TRUE(flagsOf(view, {"x", "y"}).empty());
}

TEST(OpenedInstances, ShownNetsStayApartAndKeepTheirTraces) {
  // Two traces' partial nets sharing a driver: drawn as they are.
  Equipotential a = net({}, { pinOf({"d"}, "Y", 1, Direction::Output), pinOf({"r1"}, "A", 0, Direction::Input) });
  Equipotential b = net({}, { pinOf({"d"}, "Y", 1, Direction::Output), pinOf({"r2"}, "A", 0, Direction::Input) });
  a.traceIds = {3};
  b.traceIds = {4};
  OpenedFixture fx;
  fx.shown.traceIds = {7};
  fx.shown.direct   = false;
  const OpenedView view = openInstances({ &a, &b, &fx.shown }, fx.open);
  ASSERT_EQ(view.nets.size(), 4u);
  EXPECT_EQ(view.nets[0].traceIds, std::vector<int>{3});
  EXPECT_EQ(view.nets[1].traceIds, std::vector<int>{4});
  // The trace through u2.A goes on inside u2, in its color.
  EXPECT_EQ(view.nets[2].traceIds, std::vector<int>{7});
  EXPECT_FALSE(view.nets[2].direct);
  EXPECT_TRUE(hasPin(view.nets[2], P({"u2", "g0"}), "I"));
}

TEST(OpenedInstances, ANetReachingManyPinsGetsAFlagForEach) {
  // A buffer driving 16 output pins: as many flags on one net, after its
  // pins (the list grows while flags are added).
  std::vector<BitTerm> outs;
  for (int b = 0; b < 16; ++b) outs.push_back(BitTerm{"GCLK", 2, Direction::Output, b});
  std::map<InstancePath, OpenedInstance> open;
  open[P({"cg"})] = opened({"cg"}, { net(outs, { pinOf({"cg", "buf"}, "O", 1, Direction::Output) }) });
  const OpenedView view = openInstances({}, open);
  ASSERT_EQ(view.nets.size(), 1u);
  ASSERT_EQ(view.nets[0].occurrences.size(), 17u);
  EXPECT_TRUE(hasPin(view.nets[0], P({"cg", "buf"}), "O"));
  EXPECT_EQ(view.boundary.size(), 16u);
  std::set<unsigned> ids;
  for (const auto& [path, pin] : view.boundary) ids.insert(path.back().id);
  EXPECT_EQ(ids.size(), 16u);  // one made-up id per bit
}

// ---------------------------------------------------------------------------
// Frame room and empty frames (frame pins)
// ---------------------------------------------------------------------------

TEST(HierarchyGroups, RoomForPinsWidensAFrameOnThatSide) {
  HierFixture plain({{{"core", "g1"}, {}, 0, 0.f}});
  const auto f0 = frame(plain.run(), {"core"}).shape;
  const float g0x = byPath(plain.shapes, {"core", "g1"}).x;

  HierFixture fx({{{"core", "g1"}, {}, 0, 0.f}});
  FrameRoom room;
  room.left  = 50.f;
  room.right = 30.f;
  room.minH  = 400.f;
  const auto f = frame(fx.run({{P({"core"}), room}}), {"core"}).shape;
  EXPECT_FLOAT_EQ(f.w, f0.w + 80.f);
  // The leaf moves right by the left room only.
  EXPECT_FLOAT_EQ(byPath(fx.shapes, {"core", "g1"}).x - f.x, g0x - f0.x + 50.f);
  EXPECT_FLOAT_EQ(f.h, 400.f);
}

TEST(HierarchyGroups, AnEmptyFrameGetsAColumnOfItsOwnBetweenTwoLevels) {
  HierFixture fx({{{"drv"}, {}, 0, 0.f}, {{"rcv"}, {}, 1, 0.f}});
  EmptyFrame e;
  e.pathModels = {"thru"};
  e.level      = 0.5f;
  e.y          = 20.f;
  auto frames = fx.run({}, {{P({"u_thru"}), e}});
  ASSERT_EQ(frames.size(), 1u);
  const auto& f = frames[0].shape;
  EXPECT_EQ(f.name, "u_thru (thru)");
  const auto& drv = byPath(fx.shapes, {"drv"});
  const auto& rcv = byPath(fx.shapes, {"rcv"});
  EXPECT_GT(f.x, drv.x + drv.w);
  EXPECT_LT(f.x + f.w, rcv.x);
}

// ---------------------------------------------------------------------------
// splitNet
// ---------------------------------------------------------------------------

namespace {

NetGraphEdge portEdge(size_t from, size_t to, const std::vector<std::string>& inst, const std::string& term,
                      unsigned termId = 0, Direction dir = Direction::Input) {
  NetGraphEdge e;
  e.port = true;
  e.from = from;
  e.to   = to;
  e.path = P(inst);
  e.pin  = BitTerm{term, termId, dir, std::nullopt};
  return e;
}
NetGraphEdge assignEdge(size_t from, size_t to) {
  NetGraphEdge e;
  e.port = false;
  e.from = from;
  e.to   = to;
  return e;
}
NetGraphNode node(const std::vector<std::string>& path, const std::string& net) {
  NetGraphNode n;
  n.path = path.empty() ? InstancePath{} : P(path);
  n.net  = net;
  return n;
}

// drv (top) drives net n, which goes into m (pin i) and on into m/s (pin
// i) to the leaf g; r reads n at the top.
NetGraph twoLevels() {
  NetGraph g;
  g.nodes = { node({}, "n"), node({"m"}, "i"), node({"m", "s"}, "i") };
  g.edges = { portEdge(0, 1, {"m"}, "i"), portEdge(1, 2, {"m", "s"}, "i") };
  return g;
}

bool hasEnd(const NetSegment& s, int end) {
  return std::find(s.receivers.begin(), s.receivers.end(), SegmentEnd{ end, -1, false }) != s.receivers.end();
}

} // namespace

TEST(NetSegments, ANetEnteringTwoFramesIsThreeSegmentsJoinedByTheirPins) {
  // Ends: 0 drv (node 0, drives), 1 g (node 2), 2 r (node 0).
  auto ns = splitNet(twoLevels(), {0, 2, 0}, {true, false, false}, {P({"m"}), P({"m", "s"})});
  ASSERT_TRUE(ns);
  ASSERT_EQ(ns->pins.size(), 2u);
  EXPECT_EQ(ns->pins[0].frame, P({"m"}));
  EXPECT_EQ(ns->pins[1].frame, P({"m", "s"}));
  EXPECT_TRUE(ns->pins[0].entering && ns->pins[1].entering);
  EXPECT_EQ(ns->pins[0].name, "i");
  ASSERT_EQ(ns->segments.size(), 3u);
  const auto& top = ns->segments[0];
  EXPECT_EQ(top.scope, InstancePath{});
  EXPECT_EQ(top.net, "n");
  EXPECT_EQ(top.source, (SegmentEnd{ 0, -1, false }));
  EXPECT_TRUE(hasEnd(top, 2));
  EXPECT_EQ(top.receivers.back(), (SegmentEnd{ 2, -1, false }));
  EXPECT_NE(std::find(top.receivers.begin(), top.receivers.end(), SegmentEnd{ -1, 0, false }), top.receivers.end());
  const auto& mid = ns->segments[1];
  EXPECT_EQ(mid.scope, P({"m"}));
  EXPECT_EQ(mid.source, (SegmentEnd{ -1, 0, true }));
  ASSERT_EQ(mid.receivers.size(), 1u);
  EXPECT_EQ(mid.receivers[0], (SegmentEnd{ -1, 1, false }));
  const auto& in = ns->segments[2];
  EXPECT_EQ(in.source, (SegmentEnd{ -1, 1, true }));
  EXPECT_TRUE(hasEnd(in, 1));
}

TEST(NetSegments, APinOfAModuleWithNoFrameIsNoCrossing) {
  auto ns = splitNet(twoLevels(), {0, 2, 0}, {true, false, false}, {P({"m"})});
  ASSERT_TRUE(ns);
  ASSERT_EQ(ns->pins.size(), 1u);
  ASSERT_EQ(ns->segments.size(), 2u);
  EXPECT_TRUE(hasEnd(ns->segments[1], 1));   // g, in m's segment (m/s isn't drawn)
  EXPECT_EQ(ns->segments[1].scope, P({"m"}));
}

TEST(NetSegments, ADriverInsideLeavesThroughTheRightEdge) {
  // g drives from inside m/s; r reads at the top.
  auto ns = splitNet(twoLevels(), {2, 0}, {true, false}, {P({"m"}), P({"m", "s"})});
  ASSERT_TRUE(ns);
  ASSERT_EQ(ns->pins.size(), 2u);
  EXPECT_FALSE(ns->pins[0].entering);
  EXPECT_FALSE(ns->pins[1].entering);
  EXPECT_EQ(ns->segments[0].scope, P({"m", "s"}));
  EXPECT_EQ(ns->segments[0].source, (SegmentEnd{ 0, -1, false }));
  EXPECT_EQ(ns->segments.back().scope, InstancePath{});
  EXPECT_TRUE(hasEnd(ns->segments.back(), 1));
}

TEST(NetSegments, AnAssignFeedThroughIsOneSegmentBetweenItsTwoPins) {
  // n enters t at i, i is assigned to o, o leaves t as m.
  NetGraph g;
  g.nodes = { node({}, "n"), node({"t"}, "i"), node({"t"}, "o"), node({}, "m") };
  g.edges = { portEdge(0, 1, {"t"}, "i", 0), assignEdge(1, 2), portEdge(3, 2, {"t"}, "o", 1, Direction::Output) };
  auto ns = splitNet(g, {0, 3}, {true, false}, {P({"t"})});
  ASSERT_TRUE(ns);
  ASSERT_EQ(ns->pins.size(), 2u);
  EXPECT_TRUE(ns->pins[0].entering);    // i, on the left
  EXPECT_FALSE(ns->pins[1].entering);   // o, on the right
  ASSERT_EQ(ns->segments.size(), 3u);
  const auto& inside = ns->segments[1];
  EXPECT_EQ(inside.source, (SegmentEnd{ -1, 0, true }));
  ASSERT_EQ(inside.receivers.size(), 1u);
  EXPECT_EQ(inside.receivers[0], (SegmentEnd{ -1, 1, true }));
  // Each named by the net at its source: inside, the one on i.
  EXPECT_EQ(ns->segments[0].net, "n");
  EXPECT_EQ(inside.net, "i");
  EXPECT_EQ(ns->segments[2].net, "m");
  EXPECT_EQ(ns->segments[2].source, (SegmentEnd{ -1, 1, false }));
  EXPECT_TRUE(hasEnd(ns->segments[2], 1));
}

TEST(NetSegments, TwoPinsIntoOneInnerNetMakeTheSecondAReceiverOnBothSides) {
  NetGraph g;
  g.nodes = { node({}, "n"), node({"m"}, "x") };
  g.edges = { portEdge(0, 1, {"m"}, "a", 0), portEdge(0, 1, {"m"}, "b", 1) };
  auto ns = splitNet(g, {0, 1}, {true, false}, {P({"m"})});
  ASSERT_TRUE(ns);
  ASSERT_EQ(ns->pins.size(), 2u);
  ASSERT_EQ(ns->segments.size(), 2u);
  const auto& top = ns->segments[0];
  const auto& in  = ns->segments[1];
  EXPECT_EQ(in.source, (SegmentEnd{ -1, 0, true }));
  EXPECT_NE(std::find(top.receivers.begin(), top.receivers.end(), SegmentEnd{ -1, 1, false }), top.receivers.end());
  EXPECT_NE(std::find(in.receivers.begin(), in.receivers.end(), SegmentEnd{ -1, 1, true }), in.receivers.end());
}

TEST(NetSegments, NothingWhenTheNetCantBeSplit) {
  const std::set<InstancePath> frames{ P({"m"}), P({"m", "s"}) };
  EXPECT_FALSE(splitNet(twoLevels(), {0, std::nullopt}, {true, false}, frames));  // an end off the graph
  EXPECT_FALSE(splitNet(twoLevels(), {0, 2}, {false, false}, frames));           // no driver
  EXPECT_FALSE(splitNet(twoLevels(), {0, 2}, {true, false}, {}));                // no frame crossed
  EXPECT_FALSE(splitNet(NetGraph{}, {}, {}, frames));
}

// ---------------------------------------------------------------------------
// Routing with frames
// ---------------------------------------------------------------------------

TEST(RouteNets, AFrameWithPinsKeepsOuterTrunksOutsideAndInnerOnesInside) {
  // A frame from x=100 to 300 with a pin on its left edge at y=50. Outside:
  // a driver at (40, 30) reaches the pin's outer face. Inside: the pin's
  // inner face reaches a box at x=200, at y=90.
  const std::vector<RouteRect> boxes = {{0.f, 0.f, 40.f, 200.f}, {200.f, 80.f, 240.f, 100.f}};
  const std::vector<RouteRegion> frames = {{{100.f, 0.f, 300.f, 200.f}, true}};
  RouteNet outer{{{40.f, 30.f}, true}, {{{100.f, 50.f}, false}}};
  RouteNet inner{{{100.f, 50.f}, true}, {{{200.f, 90.f}, false}}};
  auto r = routeNets({outer, inner}, boxes, {}, frames);
  ASSERT_EQ(r.size(), 2u);
  EXPECT_LT(trunkX(r[0], 40.f, 100.f), 100.f);
  EXPECT_GT(trunkX(r[1], 100.f, 200.f), 100.f);
  for (const auto& s : r[0].segments) EXPECT_LE(std::max(s.a.x, s.b.x), 100.f + 1e-3f);
  for (const auto& s : r[1].segments) EXPECT_GE(std::min(s.a.x, s.b.x), 100.f - 1e-3f);
}

TEST(RouteNets, AConfinedNetsSpineGoesAroundAFrameItIsNotIn) {
  // Two columns of boxes, a frame between them at the wire's height.
  const std::vector<RouteRect> boxes = {{0.f, 0.f, 40.f, 200.f}, {140.f, 150.f, 160.f, 200.f},
                                        {300.f, 0.f, 340.f, 200.f}};
  const std::vector<RouteRegion> frames = {{{100.f, 0.f, 200.f, 100.f}, false}};
  RouteNet net{{{40.f, 50.f}, true}, {{{300.f, 50.f}, false}}};
  auto spineYs = [&](const RoutedNet& n) {
    std::vector<float> ys;
    for (const auto& s : n.segments)
      if (std::abs(s.a.y - s.b.y) < 1e-3f && std::min(s.a.x, s.b.x) < 140.f && std::max(s.a.x, s.b.x) > 160.f)
        ys.push_back(s.a.y);
    return ys;
  };
  // Free to run through it...
  auto free = spineYs(routeNets({net}, boxes, {}, frames)[0]);
  ASSERT_EQ(free.size(), 1u);
  EXPECT_FLOAT_EQ(free[0], 50.f);
  // ...but not when confined outside it.
  net.confined = true;
  auto around = spineYs(routeNets({net}, boxes, {}, frames)[0]);
  ASSERT_EQ(around.size(), 1u);
  EXPECT_TRUE(around[0] < 0.f || around[0] > 100.f) << around[0];
  // Inside it, it may.
  net.inside = {0};
  auto inside = spineYs(routeNets({net}, boxes, {}, frames)[0]);
  ASSERT_EQ(inside.size(), 1u);
  EXPECT_FLOAT_EQ(inside[0], 50.f);
}

#include "GUIData.h"
#include "Types.h"

#include <gtest/gtest.h>

namespace {

InstTermOccurrence occ(InstancePath path, const std::string& term, Direction dir,
                       std::optional<int> bit = std::nullopt) {
  InstTermOccurrence o;
  o.path = std::move(path);
  o.term = BitTerm{term, 0, dir, bit};
  return o;
}

// top input `in` -> u1.A, u2.A
Equipotential fullNet() {
  Equipotential e{true, {BitTerm{"in", 0, Direction::Input, std::nullopt}}, {}};
  e.occurrences.push_back(occ({{1, "u1"}}, "A", Direction::Input));
  e.occurrences.push_back(occ({{3, "sub"}, {2, "u2"}}, "A", Direction::Input));
  return e;
}

} // namespace

// ---------------------------------------------------------------------------
// equipotentialCovers
// ---------------------------------------------------------------------------

TEST(EquipotentialCovers, SameNetIsCovered) {
  EXPECT_TRUE(equipotentialCovers(fullNet(), fullNet()));
}

// A driver trace lists only the receiver it entered each net through.
TEST(EquipotentialCovers, TracePartialNetIsCoveredByTheFullNet) {
  Equipotential partial{true, {BitTerm{"in", 0, Direction::Input, std::nullopt}}, {}};
  partial.occurrences.push_back(occ({{1, "u1"}}, "A", Direction::Input));
  EXPECT_TRUE(equipotentialCovers(fullNet(), partial));
  EXPECT_FALSE(equipotentialCovers(partial, fullNet()));  // but not the other way round
}

TEST(EquipotentialCovers, DifferentPinBitOrPathIsNotCovered) {
  auto otherPin = fullNet();
  otherPin.occurrences[0].term.name = "B";
  EXPECT_FALSE(equipotentialCovers(fullNet(), otherPin));

  auto otherBit = fullNet();
  otherBit.occurrences[0].term.bit = 1;
  EXPECT_FALSE(equipotentialCovers(fullNet(), otherBit));

  // Same leaf name, different hierarchy.
  auto otherPath = fullNet();
  otherPath.occurrences[1].path = {{2, "u2"}};
  EXPECT_FALSE(equipotentialCovers(fullNet(), otherPath));

  // Same (empty) name, different anonymous instance.
  Equipotential anon0{true, {}, {occ({{5, ""}}, "A", Direction::Input)}};
  Equipotential anon1{true, {}, {occ({{6, ""}}, "A", Direction::Input)}};
  EXPECT_FALSE(equipotentialCovers(anon0, anon1));
  EXPECT_TRUE(equipotentialCovers(anon0, anon0));

  auto otherTerm = fullNet();
  otherTerm.terms[0].name = "in2";
  EXPECT_FALSE(equipotentialCovers(fullNet(), otherTerm));
}

TEST(EquipotentialCovers, PathSegmentsAreNotConfusedWithSlashes) {
  Equipotential a{true, {}, {occ({{1, "a"}, {2, "b"}}, "A", Direction::Input)}};
  Equipotential b{true, {}, {occ({{3, "a/b"}}, "A", Direction::Input)}};
  // An escaped name can contain '/': instance b inside a is not the
  // top-level instance named "a/b", so a doesn't cover b.
  EXPECT_FALSE(equipotentialCovers(a, b));
}

// ---------------------------------------------------------------------------
// GUIData::addEquipotential
// ---------------------------------------------------------------------------

TEST(GUIData, AddsANewNet) {
  GUIData data;
  EXPECT_TRUE(data.addEquipotential(new Equipotential(fullNet())));
  EXPECT_EQ(data.equipotentials_.size(), 1u);
  data.clearEquipotentials();
}

// Clicking a pin whose net is already shown, or a trace re-listing it, used
// to add it again -- drawing each wire twice, which then got merged into a
// fake "bus" wire.
TEST(GUIData, RejectsANetAlreadyShown) {
  GUIData data;
  data.addEquipotential(new Equipotential(fullNet()));

  Equipotential partial{true, {}, {occ({{1, "u1"}}, "A", Direction::Input)}};
  EXPECT_FALSE(data.addEquipotential(new Equipotential(fullNet())));
  EXPECT_FALSE(data.addEquipotential(new Equipotential(partial)));
  EXPECT_EQ(data.equipotentials_.size(), 1u);
  data.clearEquipotentials();
}

// A trace's partial net folded into the full net shown records only the
// instances the trace listed: the net's other readers aren't on the trace.
TEST(GUIData, RecordsTheInstancesEachTraceReached) {
  GUIData data;
  data.addEquipotential(new Equipotential(fullNet()), 1);
  Equipotential partial{true, {}, {occ({{1, "u1"}}, "A", Direction::Input)}};
  EXPECT_FALSE(data.addEquipotential(new Equipotential(partial), 2));

  ASSERT_EQ(data.equipotentials_.size(), 1u);
  const auto& reached = data.equipotentials_[0]->traceInstances;
  EXPECT_EQ(reached.at(1), (std::set<InstancePath>{{{1, "u1"}}, {{3, "sub"}, {2, "u2"}}}));
  EXPECT_EQ(reached.at(2), (std::set<InstancePath>{{{1, "u1"}}}));

  data.removeTrace(2);
  EXPECT_EQ(data.equipotentials_[0]->traceInstances.count(2), 0u);
  data.clearEquipotentials();
}

TEST(GUIData, AcceptsANetThatAddsEndpoints) {
  GUIData data;
  Equipotential partial{true, {}, {occ({{1, "u1"}}, "A", Direction::Input)}};
  data.addEquipotential(new Equipotential(partial));
  // The full net has endpoints the partial one lacks: it's new information.
  EXPECT_TRUE(data.addEquipotential(new Equipotential(fullNet())));
  EXPECT_EQ(data.equipotentials_.size(), 2u);
  data.clearEquipotentials();
}

#include "GUIData.h"
#include "Types.h"

#include <gtest/gtest.h>

namespace {

InstTermOccurrence occ(std::vector<std::string> path, const std::string& term, Direction dir,
                       std::optional<int> bit = std::nullopt) {
  InstTermOccurrence o;
  o.path = std::move(path);
  o.term = BitTerm{term, 0, dir, bit};
  return o;
}

// top input `in` -> u1.A, u2.A
Equipotential fullNet() {
  Equipotential e{true, {BitTerm{"in", 0, Direction::Input, std::nullopt}}, {}};
  e.occurrences.push_back(occ({"u1"}, "A", Direction::Input));
  e.occurrences.push_back(occ({"sub", "u2"}, "A", Direction::Input));
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
  partial.occurrences.push_back(occ({"u1"}, "A", Direction::Input));
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
  otherPath.occurrences[1].path = {"u2"};
  EXPECT_FALSE(equipotentialCovers(fullNet(), otherPath));

  auto otherTerm = fullNet();
  otherTerm.terms[0].name = "in2";
  EXPECT_FALSE(equipotentialCovers(fullNet(), otherTerm));
}

TEST(EquipotentialCovers, PathSegmentsAreNotConfusedWithSlashes) {
  Equipotential a{true, {}, {occ({"a", "b"}, "A", Direction::Input)}};
  Equipotential b{true, {}, {occ({"a/b"}, "A", Direction::Input)}};
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

  Equipotential partial{true, {}, {occ({"u1"}, "A", Direction::Input)}};
  EXPECT_FALSE(data.addEquipotential(new Equipotential(fullNet())));
  EXPECT_FALSE(data.addEquipotential(new Equipotential(partial)));
  EXPECT_EQ(data.equipotentials_.size(), 1u);
  data.clearEquipotentials();
}

TEST(GUIData, AcceptsANetThatAddsEndpoints) {
  GUIData data;
  Equipotential partial{true, {}, {occ({"u1"}, "A", Direction::Input)}};
  data.addEquipotential(new Equipotential(partial));
  // The full net has endpoints the partial one lacks: it's new information.
  EXPECT_TRUE(data.addEquipotential(new Equipotential(fullNet())));
  EXPECT_EQ(data.equipotentials_.size(), 2u);
  data.clearEquipotentials();
}

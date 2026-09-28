#include "SchematicInteraction.h"

#include <gtest/gtest.h>

using namespace SchematicInteraction;

namespace {

// A 180x70 box at (0,0) with pins given as {id, lx, ly}.
InstanceShape box(int id, std::vector<std::tuple<int, float, float>> pins,
                  float x = 0.f, float y = 0.f) {
  InstanceShape s;
  s.id = id; s.name = "u" + std::to_string(id);
  s.x = x; s.y = y; s.w = 180.f; s.h = 70.f;
  for (auto [pid, lx, ly] : pins) {
    Port p;
    p.id = pid; p.lx = lx; p.ly = ly;
    s.ports.push_back(p);
  }
  return s;
}

// Top-level port flag: a zero-size shape whose single pin sits at (x, y).
InstanceShape flag(int id, int pid, float x, float y) {
  InstanceShape s;
  s.id = id; s.x = x; s.y = y; s.w = 0.f; s.h = 0.f; s.modelName = "port";
  Port p;
  p.id = pid; p.lx = 1.0f;
  s.ports.push_back(p);
  return s;
}

int pickedPort(const std::vector<InstanceShape>& shapes, ImVec2 world, float scale = 1.f) {
  auto hit = pickPin(shapes, world, scale);
  return hit ? hit->portId : -1;
}

} // namespace

// ---------------------------------------------------------------------------
// pickPin
// ---------------------------------------------------------------------------

TEST(PickPin, HitsThePinAnchorAndItsTick) {
  // Left pin anchored at (0, 35), its tick reaching out to (-10, 35).
  std::vector<InstanceShape> shapes = {box(1, {{7, -0.5f, 0.f}})};
  EXPECT_EQ(pickedPort(shapes, {0.f, 35.f}), 7);
  EXPECT_EQ(pickedPort(shapes, {-kPinTickWorld, 35.f}), 7);
  // Past the tick tip, still within the radius around it.
  EXPECT_EQ(pickedPort(shapes, {-kPinTickWorld - 15.f, 35.f}), 7);

  auto hit = pickPin(shapes, {-5.f, 35.f}, 1.f);
  ASSERT_TRUE(hit.has_value());
  EXPECT_EQ(hit->instanceId, 1);
}

TEST(PickPin, RightPinsReachOutToTheRight) {
  std::vector<InstanceShape> shapes = {box(1, {{7, 0.5f, 0.f}})};
  EXPECT_EQ(pickedPort(shapes, {180.f + kPinTickWorld + 10.f, 35.f}), 7);
  EXPECT_EQ(pickedPort(shapes, {-10.f, 35.f}), -1);  // the other side of the box
}

TEST(PickPin, MissesOutsideTheRadius) {
  std::vector<InstanceShape> shapes = {box(1, {{7, -0.5f, 0.f}})};
  EXPECT_EQ(pickedPort(shapes, {0.f, 35.f + kPinHitRadiusPx + 1.f}), -1);
  EXPECT_EQ(pickedPort(shapes, {-kPinTickWorld - kPinHitRadiusPx - 1.f, 35.f}), -1);
}

TEST(PickPin, BoxBodyAwayFromPinsIsNotAPin) {
  // A double-click on the box body must reach the box (full-interface
  // expansion), not be swallowed by a pin -- even zoomed far out, where the
  // pin's screen radius would otherwise cover most of the box.
  std::vector<InstanceShape> shapes = {box(1, {{7, -0.5f, 0.f}, {8, 0.5f, 0.f}})};
  EXPECT_EQ(pickedPort(shapes, {90.f, 35.f}, 1.f), -1);
  EXPECT_EQ(pickedPort(shapes, {90.f, 35.f}, 0.1f), -1);
  EXPECT_EQ(pickedPort(shapes, {kPinHitMaxWorld + 5.f, 35.f}, 0.1f), -1);
}

// The old hit test took the first pin within radius, so with pins closer
// together than the radius a click often landed on the neighbour above.
TEST(PickPin, NearestOfOverlappingPinsWins) {
  // Pins 14 world units apart (ly step 0.2 * h 70): closer than the radius.
  std::vector<InstanceShape> shapes = {box(1, {{1, -0.5f, -0.2f}, {2, -0.5f, 0.f}, {3, -0.5f, 0.2f}})};
  const float y1 = 21.f, y2 = 35.f, y3 = 49.f;
  EXPECT_EQ(pickedPort(shapes, {-3.f, y1 + 1.f}), 1);
  EXPECT_EQ(pickedPort(shapes, {-3.f, y2 + 6.f}), 2);  // within reach of 1 and 3 too
  EXPECT_EQ(pickedPort(shapes, {-3.f, y3 - 1.f}), 3);
  EXPECT_EQ(pickedPort(shapes, {-3.f, y2 - 6.f}), 2);
}

TEST(PickPin, NearestAcrossDifferentInstances) {
  // Two boxes stacked with a small gap; the pin nearest the cursor wins
  // regardless of which instance comes first.
  std::vector<InstanceShape> shapes = {box(1, {{1, -0.5f, 0.5f}}),             // anchor (0, 70)
                                       box(2, {{2, -0.5f, -0.5f}}, 0.f, 80.f)};  // anchor (0, 80)
  EXPECT_EQ(pickedPort(shapes, {-2.f, 72.f}), 1);
  EXPECT_EQ(pickedPort(shapes, {-2.f, 78.f}), 2);
}

TEST(PickPin, RadiusFollowsZoomOnScreenButIsCappedInWorld) {
  EXPECT_FLOAT_EQ(pinHitRadius(1.f), kPinHitRadiusPx);
  EXPECT_FLOAT_EQ(pinHitRadius(3.f), kPinHitRadiusPx / 3.f);  // zoomed in: same 18px on screen
  EXPECT_FLOAT_EQ(pinHitRadius(0.2f), kPinHitMaxWorld);       // zoomed out: capped

  std::vector<InstanceShape> shapes = {box(1, {{7, -0.5f, 0.f}})};
  // 15 world units below the anchor: in reach at 1x, not at 3x zoom.
  EXPECT_EQ(pickedPort(shapes, {0.f, 50.f}, 1.f), 7);
  EXPECT_EQ(pickedPort(shapes, {0.f, 50.f}, 3.f), -1);
}

TEST(PickPin, TopLevelPortFlagIsGrabbedAlongItsBody) {
  std::vector<InstanceShape> shapes = {flag(1, 9, 100.f, 100.f)};
  EXPECT_EQ(pickedPort(shapes, {100.f, 100.f}), 9);
  EXPECT_EQ(pickedPort(shapes, {100.f + kFlagHitLenWorld, 104.f}), 9);
  EXPECT_EQ(pickedPort(shapes, {100.f + kFlagHitLenWorld + kPinHitRadiusPx + 1.f, 100.f}), -1);
}

TEST(PickPin, HierarchyFramesAreSkipped) {
  auto frame = box(1, {{7, -0.5f, 0.f}});
  frame.isHierGroup = true;
  EXPECT_EQ(pickedPort({frame}, {0.f, 35.f}), -1);
}

TEST(PickPin, NoShapesNoHit) {
  EXPECT_FALSE(pickPin({}, {0.f, 0.f}, 1.f).has_value());
}

// ---------------------------------------------------------------------------
// pinRequestKey / PendingRequests
// ---------------------------------------------------------------------------

TEST(PinRequestKey, DistinguishesPathTermAndBit) {
  auto k = pinRequestKey({1, 2}, 3, std::nullopt);
  EXPECT_EQ(k, pinRequestKey({1, 2}, 3, std::nullopt));
  EXPECT_NE(k, pinRequestKey({1, 2}, 3, 0));
  EXPECT_NE(k, pinRequestKey({1}, 3, std::nullopt));
  EXPECT_NE(k, pinRequestKey({1, 2}, 4, std::nullopt));
  EXPECT_NE(pinRequestKey({12}, 3, std::nullopt), pinRequestKey({1, 2}, 3, std::nullopt));
  EXPECT_NE(pinRequestKey({1}, 23, std::nullopt), pinRequestKey({12}, 3, std::nullopt));
}

// Two distinct pins.
const PinRequestKey kA = pinRequestKey({1}, 0, std::nullopt);
const PinRequestKey kB = pinRequestKey({2}, 0, std::nullopt);

TEST(PendingRequests, SecondClickWhileLoadingIsNotResent) {
  PendingRequests pending(5.0);
  EXPECT_TRUE(pending.begin(kA, 10.0));
  EXPECT_TRUE(pending.isPending(kA, 10.1));
  EXPECT_FALSE(pending.begin(kA, 10.2));
  EXPECT_TRUE(pending.begin(kB, 10.2));  // other pins are independent
}

TEST(PendingRequests, ResolvedRequestIsNoLongerPending) {
  PendingRequests pending;
  pending.begin(kA, 0.0);
  pending.resolve(kA);
  EXPECT_FALSE(pending.isPending(kA, 0.1));
  EXPECT_TRUE(pending.begin(kA, 0.2));
}

TEST(PendingRequests, UnansweredRequestExpiresSoThePinCanBeRetried) {
  PendingRequests pending(5.0);
  pending.begin(kA, 0.0);
  EXPECT_TRUE(pending.isPending(kA, 4.9));
  EXPECT_FALSE(pending.isPending(kA, 5.0));
  EXPECT_TRUE(pending.begin(kA, 6.0));
}

TEST(PendingRequests, ClearDropsEverything) {
  PendingRequests pending;
  pending.begin(kA, 0.0);
  pending.begin(kB, 0.0);
  pending.clear();
  EXPECT_FALSE(pending.isPending(kA, 0.0));
  EXPECT_FALSE(pending.isPending(kB, 0.0));
}

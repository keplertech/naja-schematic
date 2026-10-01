// SchematicInteraction.h — pure hit-testing/bookkeeping behind the schematic's
// pin interactions (hover, click-to-extend, right-click). No ImGui frame, no
// provider: EquipotentialView feeds it world coordinates and the clock, which
// keeps it testable (see tests/SchematicInteractionTest.cpp).
#pragma once

#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <vector>
#include <imgui.h>

#include "Types.h"
#include "SchematicLayout.h"

namespace SchematicInteraction {

// Pin hit area: a capsule around the pin's tick (which reaches kPinTickWorld
// outward from the box edge, see SchematicView.cpp's drawPorts), with a
// radius of kPinHitRadiusPx on screen -- but never more than kPinHitMaxWorld
// in world units, so at low zoom a pin doesn't swallow clicks meant for the
// box body next to it. When several capsules overlap (dense pins at low
// zoom), the nearest pin wins, not whichever was built first.
inline constexpr float kPinHitRadiusPx = 18.0f;
inline constexpr float kPinHitMaxWorld = 20.0f;
inline constexpr float kPinTickWorld   = 10.0f;
// Zero-size shapes are top-level port flags: the flag body extends right of
// the anchor (see drawBoundaryPortInstance), so that's where they're grabbed.
inline constexpr float kFlagHitLenWorld = 30.0f;

struct PinHit {
    int instanceId = -1;
    int portId     = -1;
};

// World-space radius of a pin's hit capsule at the given zoom scale.
float pinHitRadius(float scale);

// The pin whose hit capsule contains `world`, nearest first; nullopt if none.
// Hierarchy frames have no pins and are skipped.
std::optional<PinHit> pickPin(const std::vector<InstanceShape>& instances,
                              ImVec2 world, float scale);

// Wire hit area: kWireHitRadiusPx on screen around each segment, capped at
// kWireHitMaxWorld world units -- narrower than a pin's, since wires run
// close to each other and to boxes. Pins are tested first (pickPin), so a
// wire never takes a click from the pin it ends on.
inline constexpr float kWireHitRadiusPx = 6.0f;
inline constexpr float kWireHitMaxWorld = 8.0f;

// World-space radius of a wire's hit area at the given zoom scale.
float wireHitRadius(float scale);

// The index in `wires` (each one routed wire tree, as its segments) of the
// tree with a segment nearest `world` within the hit radius; nullopt if none.
std::optional<size_t> pickWire(
    const std::vector<const std::vector<SchematicLayout::RouteSegment>*>& wires,
    ImVec2 world, float scale);

// Identity of a load_equipotential request for one pin, so a pin whose net
// is already on its way isn't requested again.
using PinRequestKey = std::tuple<std::vector<unsigned>, unsigned, std::optional<int>>;
PinRequestKey pinRequestKey(const std::vector<unsigned>& pathIds, unsigned termId,
                            std::optional<int> bit);

// Requests sent but not answered yet. An entry expires after `timeout`
// seconds, so a request whose reply never comes (server error, dropped
// socket) doesn't leave its pin stuck in the "loading" state forever.
class PendingRequests {
  public:
    explicit PendingRequests(double timeout = 5.0) : timeout_(timeout) {}

    // Registers `key` at time `now`. False (and no change) if it's already
    // pending, i.e. the caller shouldn't send it again.
    bool begin(const PinRequestKey& key, double now);
    bool isPending(const PinRequestKey& key, double now) const;
    // The reply arrived (the pin's net is now shown).
    void resolve(const PinRequestKey& key);
    void clear();

  private:
    double                        timeout_;
    std::map<PinRequestKey, double> started_;
};

// The canvas's view of the world: `offset` is the world point at its top-left
// corner, `scale` the pixels per world unit.
struct View {
    ImVec2 offset;
    float  scale = 1.0f;
};

// `view` changed as little as possible so the world rect [rMin, rMax] is on a
// `canvas`-pixel canvas with `margin` pixels around it: unchanged if it
// already is, else panned by the smallest amount, and zoomed out (about the
// canvas center, never below minScale) only when the rect is too big for the
// canvas at the current zoom. Used to show what a pin click just added
// without re-fitting the whole drawing. A rect too big even at minScale is
// shown from its top-left.
View revealRect(View view, ImVec2 rMin, ImVec2 rMax, ImVec2 canvas, float margin,
                float minScale);

} // namespace SchematicInteraction

// SchematicInteraction.h — pure hit-testing/bookkeeping behind the schematic's
// pin interactions (hover, click-to-extend, right-click). No ImGui frame, no
// provider: EquipotentialView feeds it world coordinates and the clock, which
// keeps it testable (see tests/SchematicInteractionTest.cpp).
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>
#include <imgui.h>

#include "Types.h"

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

// Identity of a load_equipotential request for one pin, so a pin whose net
// is already on its way isn't requested again.
std::string pinRequestKey(const std::vector<unsigned>& pathIds, unsigned termId,
                          std::optional<int> bit);

// Requests sent but not answered yet. An entry expires after `timeout`
// seconds, so a request whose reply never comes (server error, dropped
// socket) doesn't leave its pin stuck in the "loading" state forever.
class PendingRequests {
  public:
    explicit PendingRequests(double timeout = 5.0) : timeout_(timeout) {}

    // Registers `key` at time `now`. False (and no change) if it's already
    // pending, i.e. the caller shouldn't send it again.
    bool begin(const std::string& key, double now);
    bool isPending(const std::string& key, double now) const;
    // The reply arrived (the pin's net is now shown).
    void resolve(const std::string& key);
    void clear();

  private:
    double                        timeout_;
    std::map<std::string, double> started_;
};

} // namespace SchematicInteraction

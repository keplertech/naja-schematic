// SchematicInteraction.cpp — pure hit-testing/bookkeeping for schematic pins.
#include "SchematicInteraction.h"

#include <algorithm>

namespace SchematicInteraction {

float pinHitRadius(float scale) {
    return std::min(kPinHitRadiusPx / std::max(0.01f, scale), kPinHitMaxWorld);
}

// Squared distance from p to the segment [a, b].
static float distSqToSegment(ImVec2 p, ImVec2 a, ImVec2 b) {
    float dx = b.x - a.x, dy = b.y - a.y;
    float len2 = dx * dx + dy * dy;
    float t = len2 > 0.f ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / len2, 0.f, 1.f) : 0.f;
    float cx = a.x + t * dx - p.x, cy = a.y + t * dy - p.y;
    return cx * cx + cy * cy;
}

std::optional<PinHit> pickPin(const std::vector<InstanceShape>& instances,
                              ImVec2 world, float scale) {
    const float r = pinHitRadius(scale);
    float best = r * r;
    std::optional<PinHit> hit;
    for (const auto& inst : instances) {
        if (inst.isHierGroup) continue;
        const bool isFlag = inst.w <= 0.f && inst.h <= 0.f;
        for (const auto& port : inst.ports) {
            ImVec2 a = portAnchor(inst, port);
            ImVec2 b = isFlag ? ImVec2(a.x + kFlagHitLenWorld, a.y)
                              : ImVec2(a.x + (port.lx < 0.f ? -kPinTickWorld : kPinTickWorld), a.y);
            float d = distSqToSegment(world, a, b);
            if (d <= best) { best = d; hit = PinHit{inst.id, port.id}; }
        }
    }
    return hit;
}

PinRequestKey pinRequestKey(const std::vector<unsigned>& pathIds, unsigned termId,
                            std::optional<int> bit) {
    return {pathIds, termId, bit};
}

bool PendingRequests::begin(const PinRequestKey& key, double now) {
    if (isPending(key, now)) return false;
    started_[key] = now;
    return true;
}

bool PendingRequests::isPending(const PinRequestKey& key, double now) const {
    auto it = started_.find(key);
    return it != started_.end() && now - it->second < timeout_;
}

void PendingRequests::resolve(const PinRequestKey& key) { started_.erase(key); }

void PendingRequests::clear() { started_.clear(); }

} // namespace SchematicInteraction

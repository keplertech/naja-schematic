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

float wireHitRadius(float scale) {
    return std::min(kWireHitRadiusPx / std::max(0.01f, scale), kWireHitMaxWorld);
}

std::optional<size_t> pickWire(
    const std::vector<const std::vector<SchematicLayout::RouteSegment>*>& wires,
    ImVec2 world, float scale) {
    const float r = wireHitRadius(scale);
    float best = r * r;
    std::optional<size_t> hit;
    for (size_t i = 0; i < wires.size(); ++i) {
        if (!wires[i]) continue;
        for (const auto& seg : *wires[i]) {
            float d = distSqToSegment(world, seg.a, seg.b);
            if (d <= best) { best = d; hit = i; }
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

View revealRect(View view, ImVec2 rMin, ImVec2 rMax, ImVec2 canvas, float margin,
                float minScale) {
    const ImVec2 size(rMax.x - rMin.x, rMax.y - rMin.y);
    const ImVec2 room(canvas.x - 2.0f * margin, canvas.y - 2.0f * margin);
    if (room.x <= 0.0f || room.y <= 0.0f) return view;

    // Zoom out only as far as needed, about the canvas center.
    float scale = view.scale;
    if (size.x > 0.0f) scale = std::min(scale, room.x / size.x);
    if (size.y > 0.0f) scale = std::min(scale, room.y / size.y);
    scale = std::max(scale, std::min(minScale, view.scale));
    if (scale != view.scale) {
        const ImVec2 center(view.offset.x + 0.5f * canvas.x / view.scale,
                            view.offset.y + 0.5f * canvas.y / view.scale);
        view.offset = ImVec2(center.x - 0.5f * canvas.x / scale, center.y - 0.5f * canvas.y / scale);
        view.scale  = scale;
    }

    // Then the smallest pan that brings each side in.
    auto pan = [&](float& offset, float lo, float hi, float canvasLen) {
        const float m = margin / view.scale;
        const float visLo = offset + m, visHi = offset + canvasLen / view.scale - m;
        if (hi - lo > visHi - visLo) offset = lo - m;        // too big: its start
        else if (lo < visLo)         offset -= visLo - lo;
        else if (hi > visHi)         offset += hi - visHi;
    };
    pan(view.offset.x, rMin.x, rMax.x, canvas.x);
    pan(view.offset.y, rMin.y, rMax.y, canvas.y);
    return view;
}

} // namespace SchematicInteraction

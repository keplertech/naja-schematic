#include "SchematicView.h"

#include "RasterPainter.h"
#include "ViewOptions.h"

#include <imgui.h>
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace {
inline float clampf(float v, float lo, float hi) {
    return std::max(lo, std::min(hi, v));
}

// ---------------------------------------------------------------------------
// Label legibility helpers
//
// Names are drawn in world space (their pixel size follows zoom, like the
// boxes/ports they annotate) rather than at a fixed screen size. That keeps
// text proportional to the shrinking/growing box instead of overflowing it
// or fighting neighboring labels at any given zoom level. Below a minimum
// screen size a label is hidden outright (an unreadable smear is worse than
// no label); above a cap it stops growing so heavy zoom-in doesn't blow up
// glyphs into blurry blocks.
//
// Pin ticks and wires follow the same shrink-then-vanish policy instead of
// being held at an artificial minimum pixel size forever: at extreme
// zoom-out they fade out of existence just like their labels already do,
// rather than persisting as a fixed-size clutter of ticks/lines under
// illegible text.
// ---------------------------------------------------------------------------
constexpr float kInstanceLabelBaseSize = SchematicLayout::kNameFontSize; // world-space "1x zoom" size
constexpr float kPortLabelBaseSize     = 11.0f;
constexpr float kMinLabelFontSize      = 7.0f;
constexpr float kMaxLabelFontSize      = 30.0f;

// Schematic-style pin: a short tick line flush with the box edge rather than a
// filled dot -- direction is read from position/wire, not from a red/green
// fill, matching the "color is reserved for highlighting" convention below.
constexpr float kPortTickBaseLen  = 10.0f; // world-space "1x zoom" tick length
constexpr float kMinPortTickLen   = 2.5f;
constexpr float kWireBaseThickness = 2.0f; // world-space "1x zoom" wire thickness
constexpr float kMinWireThickness  = 0.75f;

// Junction dot: a filled circle marks a real electrical branch
// (one driver pin feeding more than one receiver) -- two wires that merely
// cross on screen without sharing a pin get no dot, so a dot always means
// "connected here" and a bare crossing always means "not connected."
// A wire shared by several traces is striped in their colors: dashes this
// long in world units, anchored to world coordinates so they line up across
// corners and branches. Below kMinStripePx on screen the stripes would read
// as noise, and the tree is drawn in its plain (convergence) color instead.
constexpr float kStripeWorldLen = 10.0f;
constexpr float kMinStripePx    = 5.0f;

constexpr float kJunctionDotBaseR = 3.0f;
constexpr float kMinJunctionDotR  = 1.25f;

// Top-level design port "flag" half-height (see drawBoundaryPortInstance).
// Unlike pin ticks, a flag never vanishes on zoom-out: it follows its
// neighboring instance boxes (which are always drawn) and is only floored at
// a minimum screen size, so the design boundary stays readable at any zoom.
constexpr float kBoundaryPortHalfHBase = 8.0f;
constexpr float kMinBoundaryPortHalfH  = 3.0f;

// Near-monochrome palette: every instance shares the same flat
// neutral "paper" fill rather than an arbitrary per-category color, so color
// stays reserved for diagnosis/selection highlighting rather than decorating
// every box. The canvas itself is the same convention taken one step
// further -- a plain white sheet rather than a dark viewport, so the
// near-black lines/fills above read as ink on paper instead of needing to
// fight a dark backdrop.
constexpr ImU32 kCanvasBgColor      = IM_COL32(255, 255, 255, 255);
constexpr ImU32 kInstanceFillColor  = IM_COL32(214, 216, 220, 255);
constexpr ImU32 kInstanceLineColor  = IM_COL32(35, 35, 38, 235);
constexpr ImU32 kPinLineColor       = IM_COL32(40, 40, 40, 235);
constexpr ImU32 kPinLabelColor      = IM_COL32(45, 45, 50, 235);
constexpr ImU32 kInstanceNameColor  = IM_COL32(30, 40, 90, 235);
constexpr ImU32 kInstanceModelColor = IM_COL32(95, 95, 110, 235);
// World offset of each box drawn behind a fanout summary (fits in kRowSpacing's
// room under it: kRowSpacing - kNameH = 10).
constexpr float kSummaryStackStep   = 3.0f;
// World-space radius of a gate's negation bubble (NAND/NOR/XNOR/INV).
constexpr float kBubbleR = 4.0f;
// Top-level design ports get their own tint so they can't be mistaken for an
// instance pin or a small instance box. Green on purpose: diagnosis severities
// use red/amber/blue (DiagnosisStore.cpp), so it doesn't read as a finding.
constexpr ImU32 kBoundaryPortFillColor = IM_COL32(196, 232, 204, 255);
constexpr ImU32 kBoundaryPortLineColor = IM_COL32(28, 110, 60, 255);
// Interaction feedback, not a finding: the pin under the cursor, and an open
// pin whose net is loading. Blue/violet stay clear of the diagnosis
// red/amber/blue-ish severities by being brighter and used only transiently.
constexpr ImU32 kPinHoverColor   = IM_COL32(20, 110, 235, 255);
constexpr ImU32 kPinPendingColor = IM_COL32(150, 90, 220, 255);
// The selected instance (SelectionStore) gets an outline just outside its
// box, the selected net a halo around its wires, in the same interaction
// blue as a hovered pin.
constexpr ImU32 kSelectionColor = IM_COL32(20, 110, 235, 255);
void addSelectionOutline(SchematicPainter* dl, ImVec2 rmin, ImVec2 rmax) {
    dl->AddRect(ImVec2(rmin.x - 4.0f, rmin.y - 4.0f), ImVec2(rmax.x + 4.0f, rmax.y + 4.0f),
                kSelectionColor, 2.0f, 0, 2.5f);
}
// Open-pin stub circle (see Port::open), world-space "1x zoom" radius.
constexpr float kOpenPinBaseR = 3.5f;
constexpr float kMinOpenPinR  = 2.0f;

// Returns 0.0f when the label would render too small to read -- callers
// should skip drawing (and any backing rect) in that case.
float labelFontSize(float baseSize, float scale) {
    float sz = baseSize * scale;
    if (sz < kMinLabelFontSize) return 0.0f;
    return std::min(sz, kMaxLabelFontSize);
}

// Same shrink-then-vanish policy as labelFontSize(), for elements (pin dots,
// wires) with no upper cap on how large they should grow when zoomed in.
float zoomedSizeOrHidden(float baseSize, float scale, float minPx) {
    float sz = baseSize * scale;
    return sz < minPx ? 0.0f : sz;
}

// Picks black or white text (by relative luminance) so a label stays legible
// against an arbitrary fill color, including diagnosis-tinted boxes.
ImU32 contrastingTextColor(ImU32 bg, unsigned char alpha = 235) {
    float r = ((bg >> IM_COL32_R_SHIFT) & 0xFF) / 255.0f;
    float g = ((bg >> IM_COL32_G_SHIFT) & 0xFF) / 255.0f;
    float b = ((bg >> IM_COL32_B_SHIFT) & 0xFF) / 255.0f;
    float luminance = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    return luminance > 0.55f ? IM_COL32(20, 20, 20, alpha) : IM_COL32(245, 245, 245, alpha);
}

// `over` painted over the opaque `under` (alpha compositing): opaque.
ImU32 blendOver(ImU32 over, ImU32 under) {
    const float a = ((over >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f;
    auto ch = [&](int shift) {
        float o = float((over >> shift) & 0xFF), u = float((under >> shift) & 0xFF);
        return unsigned(o * a + u * (1.0f - a) + 0.5f);
    };
    return IM_COL32(ch(IM_COL32_R_SHIFT), ch(IM_COL32_G_SHIFT), ch(IM_COL32_B_SHIFT), 255);
}

// A shape's body fill: the host style's (StyleStore) when it has one -- a
// translucent one tints `base` -- else `base`.
ImU32 fillOf(const InstanceShape& inst, ImU32 base) {
    return inst.fill == 0 ? base : blendOver(inst.fill, base);
}

// The highlight outline's thickness: `def` screen pixels, or the host
// style's width (world units) at the current zoom.
float outlineThickness(const InstanceShape& inst, float scale, float def) {
    return inst.outlineWidth > 0.0f ? std::max(1.0f, inst.outlineWidth * scale) : def;
}

// Truncates `text` to fit within maxWidth pixels at fontSize, appending "..."
// when it doesn't fit whole ("" if there isn't even room for the ellipsis).
std::string truncateToWidth(ImFont* font, float fontSize, const std::string& text, float maxWidth) {
    if (maxWidth <= 0.0f) return "";
    if (font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text.c_str()).x <= maxWidth) return text;
    const char* kEllipsis = "...";
    float ellipsisWidth = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, kEllipsis).x;
    if (ellipsisWidth > maxWidth) return "";
    size_t lo = 0, hi = text.size();
    while (lo < hi) {
        size_t mid = (lo + hi + 1) / 2;
        float w = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text.data(), text.data() + mid).x;
        if (w + ellipsisWidth <= maxWidth) lo = mid; else hi = mid - 1;
    }
    return text.substr(0, lo) + kEllipsis;
}

// Screen-space point sampling for gate body outlines (§ standard shapes
// library, below): a plain vector of points rather than ImDrawList's own
// Path*() buffer, since a shape needs its points twice (once filled, once
// stroked as the outline) and Path*() consumes its buffer on Fill/Stroke.
void appendArcPoints(std::vector<ImVec2>& pts, ImVec2 center, float radius,
                     float a0, float a1, int segments) {
    for (int i = 0; i <= segments; ++i) {
        float t = a0 + (a1 - a0) * (float(i) / float(segments));
        pts.push_back(ImVec2(center.x + cosf(t) * radius, center.y + sinf(t) * radius));
    }
}

void appendQuadBezierPoints(std::vector<ImVec2>& pts, ImVec2 p0, ImVec2 c, ImVec2 p1, int segments) {
    for (int i = 0; i <= segments; ++i) {
        float t = float(i) / float(segments);
        float u = 1.0f - t;
        pts.push_back(ImVec2(u * u * p0.x + 2.0f * u * t * c.x + t * t * p1.x,
                              u * u * p0.y + 2.0f * u * t * c.y + t * t * p1.y));
    }
}

// Draws a sampled body outline both filled and stroked (plus a thicker
// diagnosis-severity stroke on top, when flagged) -- shared tail end of every
// standard gate shape below.
void fillAndStrokeBody(SchematicPainter* dl, std::vector<ImVec2> pts,
                       ImU32 fillColor, ImU32 diagOutline, float diagThickness = 3.0f) {
    // Curves appended back to back repeat their joint point, which breaks
    // the concave fill's triangulation.
    auto same = [](ImVec2 a, ImVec2 b) { return std::abs(a.x - b.x) < 0.01f && std::abs(a.y - b.y) < 0.01f; };
    pts.erase(std::unique(pts.begin(), pts.end(), same), pts.end());
    while (pts.size() > 1 && same(pts.front(), pts.back())) pts.pop_back();
    dl->AddConcavePolyFilled(pts.data(), int(pts.size()), fillColor);
    dl->AddPolyline(pts.data(), int(pts.size()), kInstanceLineColor, 1.25f, ImDrawFlags_Closed);
    if (diagOutline != 0)
        dl->AddPolyline(pts.data(), int(pts.size()), diagOutline, diagThickness, ImDrawFlags_Closed);
}

// Draws a bubble (small negation circle), touching the tip of a gate body's
// output side -- shared by NAND/NOR/XNOR/INV.
void drawNegationBubble(SchematicPainter* dl, ImVec2 tip, float radius, ImU32 fillColor) {
    ImVec2 center(tip.x + radius, tip.y);
    dl->AddCircleFilled(center, radius, fillColor);
    dl->AddCircle(center, radius, kInstanceLineColor, 16, 1.25f);
}

// Draws a dashed closed polygon in screen space.
// dashPx / gapPx are in screen pixels so they stay legible at any zoom.
void addDashedPolygon(SchematicPainter* dl, const std::vector<ImVec2>& pts,
                      ImU32 col, float thickness,
                      float dashPx = 6.0f, float gapPx = 4.0f) {
    auto seg = [&](float ax, float ay, float bx, float by) {
        float dx = bx - ax, dy = by - ay;
        float len = sqrtf(dx * dx + dy * dy);
        if (len < 0.001f) return;
        dx /= len; dy /= len;
        bool on = true;
        for (float t = 0.0f; t < len; ) {
            float step = on ? dashPx : gapPx;
            float t2 = std::min(t + step, len);
            if (on)
                dl->AddLine({ax + dx * t, ay + dy * t},
                            {ax + dx * t2, ay + dy * t2}, col, thickness);
            t = t2;
            on = !on;
        }
    };
    for (size_t i = 0; i < pts.size(); ++i) {
        const ImVec2 a = pts[i], b = pts[(i + 1) % pts.size()];
        seg(a.x, a.y, b.x, b.y);
    }
}

// Draws a dashed rectangle in screen space (no corner rounding).
void addDashedRect(SchematicPainter* dl, ImVec2 rmin, ImVec2 rmax,
                   ImU32 col, float thickness) {
    addDashedPolygon(dl, { rmin, ImVec2(rmax.x, rmin.y), rmax, ImVec2(rmin.x, rmax.y) },
                     col, thickness);
}
} // namespace

// Convert a point in world coordinates to screen coordinates using the renderer transform.
ImVec2 SchematicView::worldToScreen(const ImVec2& world, const ImVec2& canvasPos, const ImVec2& /*canvasSize*/) const {
    // world -> scaled screen offset
    float sx = (world.x - transform.offset.x + transform.screenOrigin.x / transform.scale) * transform.scale;
    float sy = (world.y - transform.offset.y + transform.screenOrigin.y / transform.scale) * transform.scale;
    return ImVec2(canvasPos.x + sx, canvasPos.y + sy);
}

// Convert a rectangle in world coordinates (x,y,w,h) to screen rect (min,max)
void SchematicView::worldRectToScreen(float x, float y, float w, float h,
                                      const ImVec2& canvasPos, const ImVec2& canvasSize,
                                      ImVec2& outMin, ImVec2& outMax) const {
    ImVec2 topLeft = worldToScreen(ImVec2(x, y), canvasPos, canvasSize);
    ImVec2 bottomRight = worldToScreen(ImVec2(x + w, y + h), canvasPos, canvasSize);
    outMin = ImVec2(std::min(topLeft.x, bottomRight.x), std::min(topLeft.y, bottomRight.y));
    outMax = ImVec2(std::max(topLeft.x, bottomRight.x), std::max(topLeft.y, bottomRight.y));
}

// Find instance by id (returns pointer or nullptr) -- const
InstanceShape* SchematicView::findInstanceById(int id) const {
    for (auto &inst : instances) {
        if (inst.id == id) return const_cast<InstanceShape*>(&inst);
    }
    return nullptr;
}

// Find port by id within an instance (returns pointer or nullptr) -- const
Port* SchematicView::findPortById(InstanceShape& inst, int portId) const {
    for (auto &p : inst.ports) {
        if (p.id == portId) return const_cast<Port*>(&p);
    }
    return nullptr;
}

// Compute absolute world position of a port given its instance and normalized local coords (lx,ly).
ImVec2 SchematicView::portWorldPos(const InstanceShape& inst, const Port& port) const {
    return portAnchor(inst, port);
}

// ---------------------------------------------------------------------------
// Shared port drawing (used by all gate renderers)
// ---------------------------------------------------------------------------
// Pin labels: inside a box, next to the pin (the box is sized
// to hold them); a gate symbol's pins are unlabeled, as on a printed
// schematic -- except a merged bus pin, whose range is labeled outside.
static void drawPorts(SchematicPainter* dl, const InstanceShape& inst,
                      const SchematicView& sv,
                      const ImVec2& canvasPos, const ImVec2& canvasSize,
                      bool namesInside) {
    for (const auto& p : inst.ports) {
        float tickLen = zoomedSizeOrHidden(kPortTickBaseLen, sv.transform.scale, kMinPortTickLen);
        if (tickLen <= 0.0f) continue; // too small to matter at this zoom -- same fade policy as labels

        ImVec2 worldP  = sv.portWorldPos(inst, p);
        ImVec2 screenP = sv.worldToScreen(worldP, canvasPos, canvasSize);
        bool   isLeft  = p.lx < 0.0f;

        // No direction color by default -- direction reads from the pin's
        // side of the box, not a red/green fill. p.color (diagnosis/
        // selection override) is the one case color is still used here.
        const bool hovered = !sv.exporting && p.id == sv.hoveredPortId;
        const bool pending = !sv.exporting && p.pending;
        // A diagnosis color wins over the host style's (StyleStore).
        ImU32 portColor = hovered ? kPinHoverColor
                        : pending ? kPinPendingColor
                        : p.color != 0 ? p.color
                        : p.styleColor != 0 ? p.styleColor : kPinLineColor;
        float tickThickness = p.styleWidth > 0.0f ? std::max(1.0f, p.styleWidth * sv.transform.scale)
                                                  : std::max(1.0f, 1.5f * sv.transform.scale);
        if (hovered) tickThickness *= 2.0f;

        ImVec2 tickEnd = ImVec2(screenP.x + (isLeft ? -tickLen : tickLen), screenP.y);
        if (hovered)
            dl->AddCircleFilled(tickEnd, std::max(6.0f, tickLen * 0.9f), IM_COL32(20, 110, 235, 45));
        dl->AddLine(screenP, tickEnd, portColor, tickThickness);
        // An open pin (net not in the view yet) ends in a hollow circle, a
        // loading one in a filled circle: "click here to see more" without
        // needing color for the resting state.
        if (p.open && !sv.hideUiGlyphs) {
            float r = std::max(kMinOpenPinR, kOpenPinBaseR * sv.transform.scale);
            ImVec2 c(tickEnd.x + (isLeft ? -r : r), tickEnd.y);
            dl->AddCircleFilled(c, r, pending ? portColor : kCanvasBgColor);
            dl->AddCircle(c, r, portColor, 0, std::max(1.0f, tickThickness * 0.8f));
        }
        // Not connected: the schematic no-connect cross on the stub's end.
        if (p.unconnected) {
            float r = std::max(kMinOpenPinR, kOpenPinBaseR * sv.transform.scale);
            ImVec2 c(tickEnd.x + (isLeft ? -r : r), tickEnd.y);
            dl->AddLine(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), portColor, tickThickness);
            dl->AddLine(ImVec2(c.x - r, c.y + r), ImVec2(c.x + r, c.y - r), portColor, tickThickness);
        }
        // A merged bus pin gets the classic diagonal bus slash across its
        // tick instead of a plain line, in addition to its "[hi:lo]" label.
        if (p.isBus) {
            ImVec2 mid = ImVec2((screenP.x + tickEnd.x) * 0.5f, screenP.y);
            float slashLen = std::max(4.0f, tickLen * 0.6f);
            dl->AddLine(ImVec2(mid.x, mid.y + slashLen * 0.5f),
                        ImVec2(mid.x + slashLen * 0.35f, mid.y - slashLen * 0.5f),
                        portColor, tickThickness);
        }

        // A host style's label replaces the name (display only), and is
        // shown on a gate's pin too.
        const std::string& pinName = p.label.empty() ? p.name : p.label;
        if (pinName.empty() || (!namesInside && !p.isBus && p.label.empty())) continue;
        float fontSize = labelFontSize(kPortLabelBaseSize, sv.transform.scale);
        if (fontSize <= 0.0f) continue;
        ImFont* font = ImGui::GetFont();
        const float pad = 4.0f * sv.transform.scale;
        // Inside: at most half the box, so left and right names never meet.
        const float maxWidth = namesInside ? inst.w * 0.5f * sv.transform.scale - pad * 1.5f
                                           : 90.0f * sv.transform.scale;
        // A gate's pins are unnamed on the canvas: its bus pin shows only its
        // range, "[7:0]" (the full name is in the pin's tooltip).
        const size_t range = pinName.rfind('[');
        const std::string shown = namesInside || !p.label.empty() || range == std::string::npos
                                      ? pinName : pinName.substr(range);
        std::string label = truncateToWidth(font, fontSize, shown, maxWidth);
        if (label.empty()) continue;
        ImVec2 textSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, label.c_str());
        ImVec2 lblPos;
        // A clock pin's name starts past drawDffInstance()'s notch.
        const float notch = p.clock ? 7.0f * sv.transform.scale : 0.0f;
        if (namesInside)
            lblPos = isLeft ? ImVec2(screenP.x + pad + notch, screenP.y - textSize.y * 0.5f)
                            : ImVec2(screenP.x - pad - textSize.x, screenP.y - textSize.y * 0.5f);
        else  // above the tick, outside the symbol
            lblPos = isLeft ? ImVec2(tickEnd.x - textSize.x, screenP.y - textSize.y - 1.0f)
                            : ImVec2(tickEnd.x, screenP.y - textSize.y - 1.0f);
        // On a styled fill, inside a box, in whichever ink reads on it.
        const ImU32 labelColor = hovered ? kPinHoverColor
                               : namesInside && inst.fill != 0
                                   ? contrastingTextColor(fillOf(inst, kInstanceFillColor))
                                   : kPinLabelColor;
        dl->AddText(font, fontSize, lblPos, labelColor, label.c_str());
    }
}

float instanceNameWidth(const std::string& name) {
    return ImGui::GetFont()->CalcTextSizeA(kInstanceLabelBaseSize, FLT_MAX, 0.0f, name.c_str()).x;
}

// Review badges: a disc with the count of open threads, in the worst open
// severity's color -- on an instance box's (or module frame's) top-right
// corner, and next to a pin with threads on its term: above the end of its
// tick, or on the top of a top-level port's flag. Drawn through the painter,
// so they reach SVG exports; they hide with the labels when zoomed too far
// out to read. reviewBadges() hit-tests the same geometry.
namespace {
struct BadgeGeom {
    ImVec2      center;
    float       radius = 0.0f;
    float       fontSize = 0.0f;
    std::string count;
    ImVec2      textSize;
};

std::optional<BadgeGeom> badgeText(int open, float fontSize) {
    if (open <= 0 || fontSize <= 0.0f) return std::nullopt;
    BadgeGeom g;
    g.fontSize = fontSize;
    g.count    = std::to_string(open);
    g.textSize = ImGui::GetFont()->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, g.count.c_str());
    g.radius   = std::max(g.textSize.x, g.textSize.y) * 0.5f + 2.0f;
    return g;
}

std::optional<BadgeGeom> instanceBadge(const InstanceShape& inst, const SchematicView& sv,
                                       const ImVec2& canvasPos, const ImVec2& canvasSize) {
    if (inst.w <= 0.0f || inst.h <= 0.0f) return std::nullopt;
    auto g = badgeText(inst.reviewOpen, labelFontSize(kInstanceLabelBaseSize, sv.transform.scale));
    if (!g) return std::nullopt;
    ImVec2 rmin, rmax;
    sv.worldRectToScreen(inst.x, inst.y, inst.w, inst.h, canvasPos, canvasSize, rmin, rmax);
    g->center = ImVec2(rmax.x, rmin.y);
    return g;
}

std::optional<BadgeGeom> pinBadge(const InstanceShape& inst, const Port& p, const SchematicView& sv,
                                  const ImVec2& canvasPos, const ImVec2& canvasSize) {
    auto g = badgeText(p.reviewOpen, labelFontSize(kPortLabelBaseSize, sv.transform.scale));
    if (!g) return std::nullopt;
    if (inst.modelName == "port") {
        // The flag's top edge, at its wire end (see drawBoundaryPortInstance).
        const float halfH = std::max(kBoundaryPortHalfHBase * sv.transform.scale, kMinBoundaryPortHalfH);
        const ImVec2 anchor = sv.worldToScreen(ImVec2(inst.x, inst.y), canvasPos, canvasSize);
        g->center = ImVec2(anchor.x, anchor.y - halfH);
        return g;
    }
    // Above the outer end of the tick (see drawPorts), clear of the wire.
    const float tickLen = zoomedSizeOrHidden(kPortTickBaseLen, sv.transform.scale, kMinPortTickLen);
    if (tickLen <= 0.0f) return std::nullopt;
    const ImVec2 screenP = sv.worldToScreen(sv.portWorldPos(inst, p), canvasPos, canvasSize);
    const float side = p.lx < 0.0f ? -1.0f : 1.0f;
    g->center = ImVec2(screenP.x + side * (tickLen + g->radius), screenP.y - g->radius - 1.0f);
    return g;
}

void paintBadge(SchematicPainter* dl, const BadgeGeom& g, ImU32 color) {
    dl->AddCircleFilled(g.center, g.radius, color);
    dl->AddText(ImGui::GetFont(), g.fontSize,
                ImVec2(g.center.x - g.textSize.x * 0.5f, g.center.y - g.textSize.y * 0.5f),
                contrastingTextColor(color), g.count.c_str());
}
}  // namespace

static void drawReviewBadge(SchematicPainter* dl, const InstanceShape& inst,
                            const SchematicView& sv,
                            const ImVec2& canvasPos, const ImVec2& canvasSize) {
    if (auto g = instanceBadge(inst, sv, canvasPos, canvasSize)) paintBadge(dl, *g, inst.reviewColor);
    for (const auto& p : inst.ports)
        if (auto g = pinBadge(inst, p, sv, canvasPos, canvasSize)) paintBadge(dl, *g, p.reviewColor);
}

std::vector<SchematicView::ReviewBadge> SchematicView::reviewBadges(const ImVec2& canvasPos,
                                                                    const ImVec2& canvasSize) const {
    std::vector<ReviewBadge> badges;
    for (const auto& inst : instances) {
        if (auto g = instanceBadge(inst, *this, canvasPos, canvasSize))
            badges.push_back({inst.id, -1, g->center, g->radius});
        for (const auto& p : inst.ports)
            if (auto g = pinBadge(inst, p, *this, canvasPos, canvasSize))
                badges.push_back({inst.id, p.id, g->center, g->radius});
    }
    return badges;
}

// Instance name above its symbol: a box's inside holds its pin
// names, and a gate symbol has no room for text. The layout reserved this
// exact (possibly shortened) text's row, so it never overlaps anything.
static void drawInstanceName(SchematicPainter* dl, const InstanceShape& inst,
                             const SchematicView& sv, ImVec2 rmin) {
    float fontSize = labelFontSize(kInstanceLabelBaseSize, sv.transform.scale);
    if (fontSize <= 0.0f) return;
    ImFont* font = ImGui::GetFont();
    const std::string shown = SchematicLayout::shownName(inst, instanceNameWidth);
    if (!shown.empty()) {
        ImVec2 ts = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, shown.c_str());
        dl->AddText(font, fontSize, ImVec2(rmin.x, rmin.y - ts.y - 2.0f * sv.transform.scale),
                    kInstanceNameColor, shown.c_str());
    }
    // The cell model, in the row the layout reserved under the symbol.
    const std::string model = SchematicLayout::shownModel(inst, instanceNameWidth);
    if (!model.empty())
        dl->AddText(font, fontSize, ImVec2(rmin.x, rmin.y + (inst.h + 2.0f) * sv.transform.scale),
                    kInstanceModelColor, model.c_str());
}

// ---------------------------------------------------------------------------
// Generic box renderer (fallback for unknown gate types)
// ---------------------------------------------------------------------------
static void drawGenericInstance(SchematicPainter* dl, const InstanceShape& inst,
                                const SchematicView& sv,
                                const ImVec2& canvasPos, const ImVec2& canvasSize) {
    ImVec2 rmin, rmax;
    sv.worldRectToScreen(inst.x, inst.y, inst.w, inst.h, canvasPos, canvasSize, rmin, rmax);

    if (inst.w > 0.0f && inst.h > 0.0f) {
        // A fanout summary stands for many boxes: two more peek out behind
        // it, down and to the right.
        if (inst.isSummary) {
            for (float k : { 2.0f, 1.0f }) {
                const ImVec2 d(kSummaryStackStep * k * sv.transform.scale, kSummaryStackStep * k * sv.transform.scale);
                const ImVec2 a(rmin.x + d.x, rmin.y + d.y), b(rmax.x + d.x, rmax.y + d.y);
                dl->AddRectFilled(a, b, fillOf(inst, kInstanceFillColor), 0.0f);
                dl->AddRect(a, b, kInstanceLineColor, 0.0f, 0, 1.0f);
            }
        }
        // Flat fill: a neutral "paper" tone shared by every
        // box, not a per-category color -- color stays reserved for
        // diagnosis/selection highlighting rather than decorating every
        // instance. Sharp corners (no rounding).
        dl->AddRectFilled(rmin, rmax, fillOf(inst, kInstanceFillColor), 0.0f);

        if (inst.partialInterface)
            // Dashing alone (not a color) signals "only a
            // subset of the interface is shown" -- same ink as a fully
            // expanded box's solid border, since color stays reserved for
            // diagnosis/selection highlighting.
            addDashedRect(dl, rmin, rmax, kInstanceLineColor, 1.25f);
        else
            dl->AddRect(rmin, rmax, kInstanceLineColor, 0.0f, 0, 1.25f);

        // Diagnosis outline drawn on top so it stays visible regardless of
        // the partialInterface dashed border above.
        if (inst.diagOutline != 0)
            dl->AddRect(rmin, rmax, inst.diagOutline, 0.0f, 0, outlineThickness(inst, sv.transform.scale, 3.5f));
        if (inst.selected) addSelectionOutline(dl, rmin, rmax);

        drawInstanceName(dl, inst, sv, rmin);

        if (inst.partialInterface && !sv.hideUiGlyphs) {
            float boxH = rmax.y - rmin.y;
            if (boxH > 28.0f) {
                const float dotR    = 2.5f;
                const float spacing = 7.0f;
                const float dotY    = rmax.y - dotR - 5.0f;
                const float dotX    = (rmin.x + rmax.x) * 0.5f;
                dl->AddCircleFilled({dotX - spacing, dotY}, dotR, kInstanceLineColor);
                dl->AddCircleFilled({dotX,           dotY}, dotR, kInstanceLineColor);
                dl->AddCircleFilled({dotX + spacing, dotY}, dotR, kInstanceLineColor);
            }
        }

        // Hierarchy expand/collapse glyph: a small "+"/"-" square straddling
        // the top border. Its world-space rect is shared with
        // EquipotentialView's click hit-test via hierToggleGlyphRect().
        if (canShowHierToggle(inst) && !sv.exporting) {
            float gx0, gy0, gx1, gy1;
            hierToggleGlyphRect(inst, gx0, gy0, gx1, gy1);
            ImVec2 gmin, gmax;
            sv.worldRectToScreen(gx0, gy0, gx1 - gx0, gy1 - gy0, canvasPos, canvasSize, gmin, gmax);
            dl->AddRectFilled(gmin, gmax, IM_COL32(40, 40, 40, 230), 3.0f);
            dl->AddRect(gmin, gmax, IM_COL32(200, 200, 200, 200), 3.0f, 0, 1.5f);
            const char* glyph = inst.hierExpanded ? "-" : "+";
            ImVec2 ts = ImGui::CalcTextSize(glyph);
            ImVec2 c((gmin.x + gmax.x) * 0.5f, (gmin.y + gmax.y) * 0.5f);
            dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f),
                        IM_COL32(255, 255, 255, 255), glyph);
        }
    }

    drawPorts(dl, inst, sv, canvasPos, canvasSize, /*namesInside=*/true);
}

// ---------------------------------------------------------------------------
// Standard shapes library
//
// Traditional (non-IEC) gate symbols for PrimitiveType::{And,Nand,Or,Nor,
// Xor,Xnor,Inv,Buf,Assign,Tie0,Tie1}. Each body fills its instance's box exactly --
// flat back on the left edge where the input pins attach, output tip (or
// negation bubble) on the right edge where the output pin attaches -- so the
// pin ticks drawn by drawPorts() always touch the body. Box size and pin
// positions come from SchematicLayout::symbolGeometry().
// ---------------------------------------------------------------------------

// Right end of a gate body: the box's right edge, less a negation bubble.
static float bodyRight(const ImVec2& rmax, bool negated, float scale) {
    return rmax.x - (negated ? 2.0f * kBubbleR * scale : 0.0f);
}

// Shared tail of every gate: selection, name, bubble, pins.
static void finishGate(SchematicPainter* dl, const InstanceShape& inst, const SchematicView& sv,
                       const ImVec2& canvasPos, const ImVec2& canvasSize,
                       ImVec2 rmin, ImVec2 rmax, bool negated) {
    if (negated) {
        float r = kBubbleR * sv.transform.scale;
        drawNegationBubble(dl, ImVec2(bodyRight(rmax, true, sv.transform.scale), (rmin.y + rmax.y) * 0.5f),
                           r, fillOf(inst, kInstanceFillColor));
    }
    if (inst.selected) addSelectionOutline(dl, rmin, rmax);
    drawInstanceName(dl, inst, sv, rmin);
    drawPorts(dl, inst, sv, canvasPos, canvasSize, /*namesInside=*/false);
}

// AND / NAND — flat back, semicircular (bulging right) front. NAND adds a
// negation bubble at the tip.
static void drawAndLikeInstance(SchematicPainter* dl, const InstanceShape& inst,
                                const SchematicView& sv,
                                const ImVec2& canvasPos, const ImVec2& canvasSize,
                                bool negated) {
    ImVec2 rmin, rmax;
    sv.worldRectToScreen(inst.x, inst.y, inst.w, inst.h, canvasPos, canvasSize, rmin, rmax);
    float cy = (rmin.y + rmax.y) * 0.5f;
    float r  = (rmax.y - rmin.y) * 0.5f;
    float xm = std::max(rmin.x, bodyRight(rmax, negated, sv.transform.scale) - r);

    std::vector<ImVec2> pts;
    pts.push_back(ImVec2(rmin.x, rmin.y));
    appendArcPoints(pts, ImVec2(xm, cy), r, -1.5707963f, 1.5707963f, 24);
    pts.push_back(ImVec2(rmin.x, rmax.y));
    fillAndStrokeBody(dl, pts, fillOf(inst, kInstanceFillColor), inst.diagOutline,
                      outlineThickness(inst, sv.transform.scale, 3.0f));
    finishGate(dl, inst, sv, canvasPos, canvasSize, rmin, rmax, negated);
}

// OR / NOR / XOR / XNOR — concave back, curved sides meeting in a point on
// the right; NOR/XNOR add a negation bubble at the tip; XOR/XNOR add the
// extra curved line behind the back. Input pins are extended from the box
// edge to the concave back so they visibly reach the body.
static void drawOrLikeInstance(SchematicPainter* dl, const InstanceShape& inst,
                               const SchematicView& sv,
                               const ImVec2& canvasPos, const ImVec2& canvasSize,
                               bool negated, bool exclusive) {
    ImVec2 rmin, rmax;
    sv.worldRectToScreen(inst.x, inst.y, inst.w, inst.h, canvasPos, canvasSize, rmin, rmax);
    const float s  = sv.transform.scale;
    const float cy = (rmin.y + rmax.y) * 0.5f;
    const float sh = rmax.y - rmin.y;
    const float x0 = rmin.x + (exclusive ? 8.0f * s : 0.0f);   // body back
    const float x1 = bodyRight(rmax, negated, s);              // body tip
    const float sw = x1 - x0;
    const float depth = std::min(sw * 0.25f, sh * 0.2f);       // concavity of the back
    ImVec2 tip(x1, cy);

    std::vector<ImVec2> pts;
    appendQuadBezierPoints(pts, ImVec2(x0, rmin.y), ImVec2(x0 + sw * 0.6f, rmin.y), tip, 16);
    appendQuadBezierPoints(pts, tip, ImVec2(x0 + sw * 0.6f, rmax.y), ImVec2(x0, rmax.y), 16);
    appendQuadBezierPoints(pts, ImVec2(x0, rmax.y), ImVec2(x0 + 2.0f * depth, cy), ImVec2(x0, rmin.y), 12);
    pts.pop_back(); // closes back onto the first point
    fillAndStrokeBody(dl, pts, fillOf(inst, kInstanceFillColor), inst.diagOutline,
                      outlineThickness(inst, sv.transform.scale, 3.0f));

    // The back curve at height y (a quadratic whose control point sits at
    // mid-height, so y is linear in t): x = x0 + 2t(1-t) * 2*depth.
    auto backX = [&](float xBase, float y) {
        float t = sh > 0.0f ? (y - rmin.y) / sh : 0.5f;
        return xBase + 4.0f * depth * t * (1.0f - t);
    };
    const float thickness = std::max(1.0f, 1.5f * s);
    for (const auto& p : inst.ports) {
        if (p.lx >= 0.0f) continue;
        ImVec2 at = sv.worldToScreen(sv.portWorldPos(inst, p), canvasPos, canvasSize);
        dl->AddLine(at, ImVec2(backX(x0, at.y), at.y), kPinLineColor, thickness);
    }
    if (exclusive) {
        std::vector<ImVec2> arc;
        appendQuadBezierPoints(arc, ImVec2(rmin.x, rmax.y), ImVec2(rmin.x + 2.0f * depth, cy),
                               ImVec2(rmin.x, rmin.y), 12);
        dl->AddPolyline(arc.data(), int(arc.size()), kInstanceLineColor, 1.25f, 0);
    }
    finishGate(dl, inst, sv, canvasPos, canvasSize, rmin, rmax, negated);
}

// INV / BUF / assign — triangle pointing right; INV adds a negation bubble at
// the tip, an assign is marked with "=".
static void drawBufLikeInstance(SchematicPainter* dl, const InstanceShape& inst,
                                const SchematicView& sv,
                                const ImVec2& canvasPos, const ImVec2& canvasSize,
                                bool negated, bool isAssign = false) {
    ImVec2 rmin, rmax;
    sv.worldRectToScreen(inst.x, inst.y, inst.w, inst.h, canvasPos, canvasSize, rmin, rmax);
    ImVec2 tl = rmin, bl(rmin.x, rmax.y);
    ImVec2 mr(bodyRight(rmax, negated, sv.transform.scale), (rmin.y + rmax.y) * 0.5f);

    std::vector<ImVec2> pts{ tl, mr, bl };
    fillAndStrokeBody(dl, pts, fillOf(inst, kInstanceFillColor), inst.diagOutline,
                      outlineThickness(inst, sv.transform.scale, 3.0f));
    if (isAssign) {
        float fs = labelFontSize(kPortLabelBaseSize, sv.transform.scale);
        if (fs > 0.0f) {
            ImFont* font = ImGui::GetFont();
            ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, "=");
            dl->AddText(font, fs, ImVec2(tl.x + (mr.x - tl.x) * 0.3f - ts.x * 0.5f, mr.y - ts.y * 0.5f),
                        contrastingTextColor(fillOf(inst, kInstanceFillColor), 200), "=");
        }
    }
    finishGate(dl, inst, sv, canvasPos, canvasSize, rmin, rmax, negated);
}

// TIE1 / TIE0 (constant cells: Liberty function "1" / "0") -- the supply
// symbol their output is tied to: a stem rising to a bar (VDD) or dropping
// to the three-line ground symbol (GND), with a lead from the stem to the
// output pin on the right edge. No body to fill: the symbol is the lines.
static void drawTieInstance(SchematicPainter* dl, const InstanceShape& inst,
                            const SchematicView& sv,
                            const ImVec2& canvasPos, const ImVec2& canvasSize,
                            bool high) {
    ImVec2 rmin, rmax;
    sv.worldRectToScreen(inst.x, inst.y, inst.w, inst.h, canvasPos, canvasSize, rmin, rmax);
    const float w  = rmax.x - rmin.x, h = rmax.y - rmin.y;
    const float cy = (rmin.y + rmax.y) * 0.5f;
    const float xs = rmin.x + w * 0.4f;                         // stem
    const float ye = high ? rmin.y + h * 0.2f : rmax.y - h * 0.35f; // stem end

    auto stroke = [&](ImU32 color, float thickness) {
        dl->AddLine(ImVec2(rmax.x, cy), ImVec2(xs, cy), color, thickness);
        dl->AddLine(ImVec2(xs, cy), ImVec2(xs, ye), color, thickness);
        if (high) {
            dl->AddLine(ImVec2(xs - w * 0.3f, ye), ImVec2(xs + w * 0.3f, ye), color, thickness);
        } else {
            for (int i = 0; i < 3; ++i) {
                float half = w * 0.3f * (1.0f - i / 3.0f), y = ye + i * h * 0.1f;
                dl->AddLine(ImVec2(xs - half, y), ImVec2(xs + half, y), color, thickness);
            }
        }
    };
    if (inst.diagOutline != 0) stroke(inst.diagOutline, outlineThickness(inst, sv.transform.scale, 3.0f));
    stroke(kInstanceLineColor, std::max(1.25f, 1.5f * sv.transform.scale));
    finishGate(dl, inst, sv, canvasPos, canvasSize, rmin, rmax, /*negated=*/false);
}

// DFF (and other clocked sequential cells) — the generic box, plus the
// standard clock-triangle notch on the left edge at the clock pin's
// position, so a flop reads differently from a plain unclassified
// hierarchical/blackbox instance even though its outline is the same.
static void drawDffInstance(SchematicPainter* dl, const InstanceShape& inst,
                            const SchematicView& sv,
                            const ImVec2& canvasPos, const ImVec2& canvasSize) {
    drawGenericInstance(dl, inst, sv, canvasPos, canvasSize);

    for (const auto& p : inst.ports) {
        if (!p.clock) continue;
        ImVec2 world  = sv.portWorldPos(inst, p);
        float  notchH = std::min(inst.h * 0.3f, 10.0f);
        ImVec2 top  = sv.worldToScreen(ImVec2(inst.x,               world.y - notchH * 0.5f), canvasPos, canvasSize);
        ImVec2 bot  = sv.worldToScreen(ImVec2(inst.x,               world.y + notchH * 0.5f), canvasPos, canvasSize);
        ImVec2 apex = sv.worldToScreen(ImVec2(inst.x + notchH * 0.6f, world.y),               canvasPos, canvasSize);
        dl->AddLine(top, apex, kInstanceLineColor, 1.25f);
        dl->AddLine(apex, bot, kInstanceLineColor, 1.25f);
        break; // one clock pin is enough to draw the notch once
    }
}

// ---------------------------------------------------------------------------
// Mux — a trapezoid: full height on the input side, its right edge pulled
// in by SchematicLayout::muxSlant() top and bottom, short of the centered
// outputs (see symbolGeometry()). Pin names inside, as in a box.
// ---------------------------------------------------------------------------
static void drawMuxInstance(SchematicPainter* dl, const InstanceShape& inst,
                            const SchematicView& sv,
                            const ImVec2& canvasPos, const ImVec2& canvasSize) {
    ImVec2 rmin, rmax;
    sv.worldRectToScreen(inst.x, inst.y, inst.w, inst.h, canvasPos, canvasSize, rmin, rmax);
    size_t rightPins = 0;
    for (const auto& p : inst.ports) if (p.lx >= 0.0f) ++rightPins;
    const float slant = SchematicLayout::muxSlant(inst.h, rightPins) * sv.transform.scale;
    std::vector<ImVec2> pts = {
        rmin, ImVec2(rmax.x, rmin.y + slant), ImVec2(rmax.x, rmax.y - slant), ImVec2(rmin.x, rmax.y),
    };
    dl->AddConvexPolyFilled(pts.data(), int(pts.size()), fillOf(inst, kInstanceFillColor));
    if (inst.partialInterface)
        addDashedPolygon(dl, pts, kInstanceLineColor, 1.25f);
    else
        dl->AddPolyline(pts.data(), int(pts.size()), kInstanceLineColor, 1.25f, ImDrawFlags_Closed);
    if (inst.diagOutline != 0)
        dl->AddPolyline(pts.data(), int(pts.size()), inst.diagOutline,
                        outlineThickness(inst, sv.transform.scale, 3.5f), ImDrawFlags_Closed);
    if (inst.selected) addSelectionOutline(dl, rmin, rmax);
    drawInstanceName(dl, inst, sv, rmin);
    drawPorts(dl, inst, sv, canvasPos, canvasSize, /*namesInside=*/true);
}

// ---------------------------------------------------------------------------
// Word-level operator (add, sub, mul, shifts, divmod) — a box, with its
// operator in a circle between the two columns of pin names, in the room
// symbolGeometry() left for it (kOperatorGlyphW).
// ---------------------------------------------------------------------------
static const char* operatorGlyph(PrimitiveType type) {
    switch (type) {
        case PrimitiveType::Add:    return "+";
        case PrimitiveType::Sub:    return "-";
        case PrimitiveType::Mul:    return "*";
        case PrimitiveType::Shl:    return "<<";
        case PrimitiveType::Shr:    return ">>";
        case PrimitiveType::Ashr:   return ">>>";
        case PrimitiveType::DivMod: return "/%";
        default:                    return "";
    }
}

static void drawOperatorInstance(SchematicPainter* dl, const InstanceShape& inst,
                                 const SchematicView& sv,
                                 const ImVec2& canvasPos, const ImVec2& canvasSize) {
    drawGenericInstance(dl, inst, sv, canvasPos, canvasSize);
    float fontSize = labelFontSize(kInstanceLabelBaseSize, sv.transform.scale);
    if (fontSize <= 0.0f) return;
    ImVec2 rmin, rmax;
    sv.worldRectToScreen(inst.x, inst.y, inst.w, inst.h, canvasPos, canvasSize, rmin, rmax);
    const ImVec2 c((rmin.x + rmax.x) * 0.5f, (rmin.y + rmax.y) * 0.5f);
    const float r = 0.5f * SchematicLayout::kOperatorGlyphW * 0.8f * sv.transform.scale;
    dl->AddCircle(c, r, kInstanceLineColor, 24, 1.25f);
    const char* glyph = operatorGlyph(inst.primitiveType);
    ImFont* font = ImGui::GetFont();
    ImVec2 ts = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, glyph);
    dl->AddText(font, fontSize, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), kPinLabelColor, glyph);
}

// ---------------------------------------------------------------------------
// Top-level design port — a "flag": a small pentagon sized to its
// name, pointing the way the signal flows. The wire attaches at the
// pseudo-instance's own x/y (the zero-size term box built in
// EquipotentialView.cpp). Inputs sit on the sheet's left edge: the flag lies
// left of that point with its tip on it, pointing into the design. Outputs
// sit on the right edge: the flag's flat base is on the point and its tip
// points away, off the sheet. Replaces the generic tick-line pin drawn by
// drawPorts() for every other kind of port: a boundary port isn't a pin on a
// box, it *is* the box.
// ---------------------------------------------------------------------------
static void drawBoundaryPortInstance(SchematicPainter* dl, const InstanceShape& inst,
                                     const SchematicView& sv,
                                     const ImVec2& canvasPos, const ImVec2& canvasSize) {
    if (inst.ports.empty()) return;
    const Port& p = inst.ports[0];

    // Floored, not hidden: top-level ports stay visible as long as instances
    // do (see kBoundaryPortHalfHBase).
    float halfH = std::max(kBoundaryPortHalfHBase * sv.transform.scale, kMinBoundaryPortHalfH);

    ImVec2 anchor = sv.worldToScreen(ImVec2(inst.x, inst.y), canvasPos, canvasSize);

    const bool hovered = !sv.exporting && p.id == sv.hoveredPortId;
    ImU32 outline = hovered ? kPinHoverColor : p.color != 0 ? p.color
                  : p.styleColor != 0 ? p.styleColor : kBoundaryPortLineColor;
    float lineThickness = p.styleWidth > 0.0f ? std::max(1.0f, p.styleWidth * sv.transform.scale)
                                              : std::max(1.0f, 1.25f * sv.transform.scale);
    const std::string& shownName = p.label.empty() ? p.name : p.label;
    const ImU32 flagFill = fillOf(inst, kBoundaryPortFillColor);
    if (hovered) lineThickness *= 2.0f;

    // Name shown whenever instance names are (same visibility threshold),
    // drawn at pin-label size but never below the legible minimum.
    float fontSize = labelFontSize(kInstanceLabelBaseSize, sv.transform.scale) > 0.0f
        ? std::max(kMinLabelFontSize, labelFontSize(kPortLabelBaseSize, sv.transform.scale))
        : 0.0f;
    ImFont* font = ImGui::GetFont();
    ImVec2 textSize = (fontSize > 0.0f && !shownName.empty())
        ? font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, shownName.c_str())
        : ImVec2(0.0f, 0.0f);

    float bodyLen = std::max(halfH * 2.0f, textSize.x + 10.0f);
    float tipLen  = halfH;

    // Input: [base .. body] tip on the anchor. Output: base on the anchor.
    float baseX = p.isInput ? anchor.x - tipLen - bodyLen : anchor.x;
    float bodyX = baseX + bodyLen;
    float tipX  = bodyX + tipLen;

    ImVec2 pts[5] = {
        { baseX, anchor.y - halfH },
        { bodyX, anchor.y - halfH },
        { tipX,  anchor.y },
        { bodyX, anchor.y + halfH },
        { baseX, anchor.y + halfH },
    };
    dl->AddConvexPolyFilled(pts, 5, flagFill);
    dl->AddPolyline(pts, 5, outline, lineThickness, ImDrawFlags_Closed);
    // Not connected: a no-connect cross where its wire would start.
    if (p.unconnected) {
        const float r = halfH * 0.6f;
        const ImVec2 c(anchor.x + (p.isInput ? r : -r) * 1.6f, anchor.y);
        dl->AddLine(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), outline, lineThickness);
        dl->AddLine(ImVec2(c.x - r, c.y + r), ImVec2(c.x + r, c.y - r), outline, lineThickness);
    }
    if (inst.diagOutline != 0)
        dl->AddPolyline(pts, 5, inst.diagOutline, outlineThickness(inst, sv.transform.scale, 3.0f),
                        ImDrawFlags_Closed);

    if (fontSize > 0.0f && !shownName.empty()) {
        ImVec2 textPos((baseX + bodyX) * 0.5f - textSize.x * 0.5f, anchor.y - textSize.y * 0.5f);
        dl->AddText(font, fontSize, textPos, contrastingTextColor(flagFill), shownName.c_str());
    }
}

// ---------------------------------------------------------------------------
// Hierarchy group frame — a hierarchical module enclosing some traced leaf
// instances (see EquipotentialView's hierarchy grouping). Drawn under the
// nets with a translucent fill, so wires crossing the frame stay visible and
// nested frames read as progressively darker layers. Label at the top-left
// corner, clear of the leaf boxes laid out below it.
// ---------------------------------------------------------------------------
static void drawHierGroupInstance(SchematicPainter* dl, const InstanceShape& inst,
                                  const SchematicView& sv,
                                  const ImVec2& canvasPos, const ImVec2& canvasSize) {
    ImVec2 rmin, rmax;
    sv.worldRectToScreen(inst.x, inst.y, inst.w, inst.h, canvasPos, canvasSize, rmin, rmax);
    int alpha = std::min(90, 22 + 14 * std::max(0, inst.hierDepth - 1));
    // A styled frame (StyleStore fill) keeps its translucency: wires and
    // nested frames still show through it.
    const ImU32 tint = inst.fill != 0 ? (inst.fill & ~IM_COL32_A_MASK) : IM_COL32(110, 125, 175, 0);
    dl->AddRectFilled(rmin, rmax, tint | (ImU32(alpha) << IM_COL32_A_SHIFT), 0.0f);
    dl->AddRect(rmin, rmax, IM_COL32(70, 80, 115, 220), 0.0f, 0, 1.25f);
    if (inst.diagOutline != 0)
        dl->AddRect(rmin, rmax, inst.diagOutline, 0.0f, 0, outlineThickness(inst, sv.transform.scale, 3.5f));
    if (inst.selected) addSelectionOutline(dl, rmin, rmax);

    float fontSize = labelFontSize(kInstanceLabelBaseSize, sv.transform.scale);
    if (!inst.name.empty() && fontSize > 0.0f) {
        ImFont* font = ImGui::GetFont();
        float padding = std::max(2.0f, 5.0f * sv.transform.scale);
        std::string label = truncateToWidth(font, fontSize, inst.label.empty() ? inst.name : inst.label,
                                            (rmax.x - rmin.x) - 2.0f * padding);
        if (!label.empty())
            dl->AddText(font, fontSize, ImVec2(rmin.x + padding, rmin.y + padding),
                        IM_COL32(35, 40, 60, 235), label.c_str());
    }
}

// ---------------------------------------------------------------------------
// Gate dispatcher — add new PrimitiveType values here as the library grows
// ---------------------------------------------------------------------------
// To add a new gate type:
//   1. Write a static drawXxxInstance() function above with the same signature
//   2. Add a case below matching its PrimitiveType
// ---------------------------------------------------------------------------
void SchematicView::drawInstance(SchematicPainter* dl, const InstanceShape& inst,
                                 const ImVec2& canvasPos, const ImVec2& canvasSize) const {
    if (inst.isHierGroup) {
        drawHierGroupInstance(dl, inst, *this, canvasPos, canvasSize);
        return;
    }
    if (inst.modelName == "port") {
        drawBoundaryPortInstance(dl, inst, *this, canvasPos, canvasSize);
        return;
    }
    // A gate with several outputs has no symbol (symbolGeometry sized it as
    // a box): draw the box.
    int rightPins = 0;
    for (const auto& p : inst.ports) if (p.lx >= 0.0f) ++rightPins;
    if (rightPins > 1 && SchematicLayout::isGateSymbol(inst.primitiveType)) {
        drawGenericInstance(dl, inst, *this, canvasPos, canvasSize);
        return;
    }
    switch (inst.primitiveType) {
        case PrimitiveType::Assign:
            drawBufLikeInstance(dl, inst, *this, canvasPos, canvasSize, /*negated=*/false, /*isAssign=*/true);
            break;
        case PrimitiveType::And:
            drawAndLikeInstance(dl, inst, *this, canvasPos, canvasSize, /*negated=*/false);
            break;
        case PrimitiveType::Nand:
            drawAndLikeInstance(dl, inst, *this, canvasPos, canvasSize, /*negated=*/true);
            break;
        case PrimitiveType::Or:
            drawOrLikeInstance(dl, inst, *this, canvasPos, canvasSize, /*negated=*/false, /*exclusive=*/false);
            break;
        case PrimitiveType::Nor:
            drawOrLikeInstance(dl, inst, *this, canvasPos, canvasSize, /*negated=*/true, /*exclusive=*/false);
            break;
        case PrimitiveType::Xor:
            drawOrLikeInstance(dl, inst, *this, canvasPos, canvasSize, /*negated=*/false, /*exclusive=*/true);
            break;
        case PrimitiveType::Xnor:
            drawOrLikeInstance(dl, inst, *this, canvasPos, canvasSize, /*negated=*/true, /*exclusive=*/true);
            break;
        case PrimitiveType::Buf:
            drawBufLikeInstance(dl, inst, *this, canvasPos, canvasSize, /*negated=*/false);
            break;
        case PrimitiveType::Inv:
            drawBufLikeInstance(dl, inst, *this, canvasPos, canvasSize, /*negated=*/true);
            break;
        case PrimitiveType::Tie0:
            drawTieInstance(dl, inst, *this, canvasPos, canvasSize, /*high=*/false);
            break;
        case PrimitiveType::Tie1:
            drawTieInstance(dl, inst, *this, canvasPos, canvasSize, /*high=*/true);
            break;
        case PrimitiveType::Dff:
            drawDffInstance(dl, inst, *this, canvasPos, canvasSize);
            break;
        case PrimitiveType::Mux:
            drawMuxInstance(dl, inst, *this, canvasPos, canvasSize);
            break;
        case PrimitiveType::Add:  case PrimitiveType::Sub:
        case PrimitiveType::Mul:  case PrimitiveType::DivMod:
        case PrimitiveType::Shl:  case PrimitiveType::Shr:
        case PrimitiveType::Ashr:
            drawOperatorInstance(dl, inst, *this, canvasPos, canvasSize);
            break;
        default:
            drawGenericInstance(dl, inst, *this, canvasPos, canvasSize);
            break;
    }
}

void SchematicView::drawNet(SchematicPainter* dl, const NetWire& net, const ImVec2& canvasPos, const ImVec2& canvasSize) const {
    InstanceShape* srcInst = findInstanceById(net.srcInstance);
    InstanceShape* dstInst = findInstanceById(net.dstInstance);
    if (!srcInst || !dstInst) return;

    Port* srcPort = findPortById(*srcInst, net.srcPortId);
    Port* dstPort = findPortById(*dstInst, net.dstPortId);
    if (!srcPort || !dstPort) return;

    ImVec2 srcWorld = portWorldPos(*srcInst, *srcPort);
    ImVec2 dstWorld = portWorldPos(*dstInst, *dstPort);
    ImVec2 srcScreen = worldToScreen(srcWorld, canvasPos, canvasSize);
    ImVec2 dstScreen = worldToScreen(dstWorld, canvasPos, canvasSize);
    // Wire connects directly at the port world position (no triangle offset).
    float srcSide = (srcPort->lx >= 0.0f) ? 1.0f : -1.0f;
    float dstSide = (dstPort->lx >= 0.0f) ? 1.0f : -1.0f;

    // Too thin to matter at this zoom -- same fade policy as labels/pin dots.
    float thickness = zoomedSizeOrHidden(net.width > 0.0f ? net.width : kWireBaseThickness,
                                         transform.scale, kMinWireThickness);
    if (thickness <= 0.0f) return;
    if (net.isBus) thickness *= 2.0f;

    const float stub = std::max(12.0f, 18.0f * transform.scale);
    ImU32 col = net.color;

    // Depart/arrive with a short stub so the wire leaves the box orthogonally.
    ImVec2 p0 = srcScreen;
    ImVec2 p1 = ImVec2(srcScreen.x + stub * srcSide, srcScreen.y);
    ImVec2 p4 = ImVec2(dstScreen.x + stub * dstSide, dstScreen.y);
    float  midX = (p1.x + p4.x) * 0.5f;
    ImVec2 p2 = ImVec2(midX, p1.y);
    ImVec2 p3 = ImVec2(midX, p4.y);
    ImVec2 p5 = dstScreen;

    std::array<ImVec2, 6> points = {p0, p1, p2, p3, p4, p5};

    // Draw segments individually to keep thickness consistent at joints.
    for (size_t i = 0; i + 1 < points.size(); ++i) {
        dl->AddLine(points[i], points[i + 1], IM_COL32(0,0,0,80), thickness + 2.0f);
        dl->AddLine(points[i], points[i + 1], col, thickness);
    }

    // Junction dot at the source pin when more than one NetWire departs from
    // it (one driver, several receivers) -- see kJunctionDotBaseR above.
    // Every receiver's NetWire recomputes and redraws the same dot at the
    // same point, which is harmless (identical draws just overlap).
    {
        int fanoutFromSrc = 0;
        for (const auto& other : nets)
            if (other.srcInstance == net.srcInstance && other.srcPortId == net.srcPortId)
                ++fanoutFromSrc;
        if (fanoutFromSrc > 1) {
            float dotR = zoomedSizeOrHidden(kJunctionDotBaseR, transform.scale, kMinJunctionDotR);
            if (dotR > 0.0f) dl->AddCircleFilled(p0, dotR, col);
        }
    }

    // Bus slash mark across the horizontal run, plus the net
    // name if we have one.
    if (!net.label.empty() && !net.isBus) {
        float fontSize = labelFontSize(kPortLabelBaseSize, transform.scale);
        if (fontSize > 0.0f) {
            ImFont* font = ImGui::GetFont();
            ImVec2 ts = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, net.label.c_str());
            dl->AddText(font, fontSize, ImVec2(midX + 4.0f, (p1.y + p4.y) * 0.5f - ts.y * 0.5f),
                        col, net.label.c_str());
        }
    }
    if (net.isBus) {
        ImVec2 mid = ImVec2(midX, (p1.y + p4.y) * 0.5f);
        float slashLen = std::max(6.0f, 9.0f * transform.scale);
        dl->AddLine(ImVec2(mid.x - slashLen * 0.35f, mid.y + slashLen * 0.5f),
                    ImVec2(mid.x + slashLen * 0.35f, mid.y - slashLen * 0.5f),
                    col, thickness);
        const std::string& busText = net.label.empty() ? net.netName : net.label;
        if (!busText.empty()) {
            float fontSize = labelFontSize(kPortLabelBaseSize, transform.scale);
            if (fontSize > 0.0f) {
                ImFont* font = ImGui::GetFont();
                ImVec2 ts = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, busText.c_str());
                ImVec2 lp = ImVec2(mid.x + 6.0f, mid.y - ts.y - 4.0f);
                dl->AddRectFilled(ImVec2(lp.x - 2.0f, lp.y - 1.0f),
                                  ImVec2(lp.x + ts.x + 2.0f, lp.y + ts.y + 1.0f),
                                  IM_COL32(30, 30, 30, 190));
                dl->AddText(font, fontSize, lp, col, busText.c_str());
            }
        }
    }
}

void SchematicView::drawRoute(SchematicPainter* dl, const NetRoute& route,
                              const ImVec2& canvasPos, const ImVec2& canvasSize) const {
    float thickness = zoomedSizeOrHidden(route.width > 0.0f ? route.width : kWireBaseThickness,
                                         transform.scale, kMinWireThickness);
    if (thickness <= 0.0f) return;
    if (route.isBus) thickness *= 2.0f;
    // The selected net: a halo in the selection color under its wires and
    // junction dots, so its own (trace, diagnosis) colors still show.
    if (route.selected) {
        const float halo = thickness + 5.0f;
        for (const auto& seg : route.segments)
            dl->AddLine(worldToScreen(seg.a, canvasPos, canvasSize),
                        worldToScreen(seg.b, canvasPos, canvasSize), kSelectionColor, halo);
        for (const auto& seg : route.segments)
            for (ImVec2 w : { seg.a, seg.b })  // square corners and ends
                dl->AddCircleFilled(worldToScreen(w, canvasPos, canvasSize), halo * 0.5f, kSelectionColor);
    }
    const bool striped = route.stripeColors.size() >= 2 &&
                         kStripeWorldLen * transform.scale >= kMinStripePx;
    // The stripe color at world coordinate `t` along a segment's axis.
    auto stripeAt = [&](float t) {
        const long n = long(route.stripeColors.size());
        long k = long(std::floor(t / kStripeWorldLen)) % n;
        return route.stripeColors[size_t(k < 0 ? k + n : k)];
    };
    // A point's color: the stripe of the horizontal axis (corner caps and
    // junction dots, where segments of both orientations meet).
    auto colorAt = [&](ImVec2 w) { return striped ? stripeAt(w.x) : route.color; };
    for (const auto& seg : route.segments) {
        if (!striped) {
            dl->AddLine(worldToScreen(seg.a, canvasPos, canvasSize),
                        worldToScreen(seg.b, canvasPos, canvasSize), route.color, thickness);
            continue;
        }
        const bool horiz = std::abs(seg.a.y - seg.b.y) < 0.5f;
        float t0 = horiz ? seg.a.x : seg.a.y, t1 = horiz ? seg.b.x : seg.b.y;
        if (t0 > t1) std::swap(t0, t1);
        auto at = [&](float t) {
            return worldToScreen(horiz ? ImVec2(t, seg.a.y) : ImVec2(seg.a.x, t), canvasPos, canvasSize);
        };
        for (float t = t0; t < t1; ) {
            float next = std::min(t1, (std::floor(t / kStripeWorldLen) + 1.0f) * kStripeWorldLen);
            dl->AddLine(at(t), at(next), stripeAt(0.5f * (t + next)), thickness);
            t = next;
        }
    }
    // A square cap where a horizontal and a vertical segment end at the same
    // point, so the corner isn't notched (butt-ended thick lines).
    auto same = [](ImVec2 a, ImVec2 b) { return std::abs(a.x - b.x) < 0.5f && std::abs(a.y - b.y) < 0.5f; };
    for (const auto& h : route.segments) {
        if (std::abs(h.a.y - h.b.y) > 0.5f) continue;
        for (ImVec2 w : { h.a, h.b }) {
            bool corner = false;
            for (const auto& v : route.segments)
                if (std::abs(v.a.x - v.b.x) < 0.5f && (same(v.a, w) || same(v.b, w))) { corner = true; break; }
            if (!corner) continue;
            ImVec2 p = worldToScreen(w, canvasPos, canvasSize);
            float r = thickness * 0.5f;
            dl->AddRectFilled(ImVec2(p.x - r, p.y - r), ImVec2(p.x + r, p.y + r), colorAt(w));
        }
    }
    float dotR = zoomedSizeOrHidden(kJunctionDotBaseR, transform.scale, kMinJunctionDotR);
    if (dotR > 0.0f)
        for (const auto& j : route.junctions)
            dl->AddCircleFilled(worldToScreen(j, canvasPos, canvasSize), route.isBus ? dotR * 1.5f : dotR, colorAt(j));

    // Bus slash mark on the longest horizontal run, plus the
    // net name if we have one -- or the host style's label, on any net.
    if (!route.isBus && route.label.empty()) return;
    const SchematicLayout::RouteSegment* best = nullptr;
    for (const auto& seg : route.segments)
        if (std::abs(seg.a.y - seg.b.y) < 0.5f &&
            (!best || std::abs(seg.b.x - seg.a.x) > std::abs(best->b.x - best->a.x)))
            best = &seg;
    if (!best) return;
    ImVec2 mid = worldToScreen(ImVec2((best->a.x + best->b.x) * 0.5f, best->a.y), canvasPos, canvasSize);
    if (route.isBus) {
        float slashLen = std::max(6.0f, 9.0f * transform.scale);
        dl->AddLine(ImVec2(mid.x - slashLen * 0.35f, mid.y + slashLen * 0.5f),
                    ImVec2(mid.x + slashLen * 0.35f, mid.y - slashLen * 0.5f),
                    route.color, thickness * 0.5f);
    }
    const std::string& text = route.label.empty() ? route.netName : route.label;
    float fontSize = labelFontSize(kPortLabelBaseSize, transform.scale);
    if (!text.empty() && fontSize > 0.0f) {
        ImFont* font = ImGui::GetFont();
        ImVec2 ts = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text.c_str());
        // A style label in the net's color, centered on the run.
        const bool styled = !route.label.empty();
        ImVec2 at = styled ? ImVec2(mid.x - ts.x * 0.5f, mid.y - ts.y - thickness * 0.5f - 2.0f)
                           : ImVec2(mid.x + 6.0f, mid.y - ts.y - 3.0f);
        dl->AddText(font, fontSize, at, styled ? route.color : kPinLabelColor, text.c_str());
    }
}

// Main render entry
// What a shape covers when drawn: the shape itself, plus a port's flag (as
// long as drawBoundaryPortInstance makes it at 1x) or an instance's name
// above it.
void shapeWorldExtent(const InstanceShape& inst, ImVec2& outMin, ImVec2& outMax) {
    const bool port = inst.modelName == "port";
    float flag = 0.0f;
    if (port && !inst.ports.empty()) {
        const float textW = ImGui::GetFont()->CalcTextSizeA(kPortLabelBaseSize, FLT_MAX, 0.0f,
                                                            inst.ports[0].name.c_str()).x;
        flag = std::max(2.0f * kBoundaryPortHalfHBase, textW + 10.0f) + kBoundaryPortHalfHBase;
    }
    const bool named = !SchematicLayout::shownName(inst, instanceNameWidth).empty();
    const float stack = inst.isSummary ? 2.0f * kSummaryStackStep : 0.0f;
    outMin = ImVec2(inst.x - flag, inst.y - (named ? SchematicLayout::kNameH : 0.0f));
    outMax = ImVec2(inst.x + SchematicLayout::footprintWidth(inst, instanceNameWidth) + flag + stack,
                    inst.y + std::max(SchematicLayout::footprintHeight(inst, instanceNameWidth), inst.h + stack));
}

bool SchematicView::computeWorldBounds(ImVec2& outMin, ImVec2& outMax) const {
    if (instances.empty()) return false;
    float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
    for (const auto& inst : instances) {
        ImVec2 lo, hi;
        shapeWorldExtent(inst, lo, hi);
        minX = std::min(minX, lo.x); minY = std::min(minY, lo.y);
        maxX = std::max(maxX, hi.x); maxY = std::max(maxY, hi.y);
    }
    for (const auto& route : routes)
        for (const auto& seg : route.segments)
            for (ImVec2 p : { seg.a, seg.b }) {
                minX = std::min(minX, p.x); maxX = std::max(maxX, p.x);
                minY = std::min(minY, p.y); maxY = std::max(maxY, p.y);
            }
    outMin = ImVec2(minX, minY);
    outMax = ImVec2(maxX, maxY);
    return true;
}

void SchematicView::fitToContents(const ImVec2& /*canvasPos*/, const ImVec2& canvasSize, float padding) {
    ImVec2 boundsMin, boundsMax;
    if (!computeWorldBounds(boundsMin, boundsMax)) return;
    fitToRect(boundsMin, boundsMax, canvasSize, padding);
}

void SchematicView::fitToRect(ImVec2 boundsMin, ImVec2 boundsMax, const ImVec2& canvasSize, float padding) {
    float width = std::max(1.0f, boundsMax.x - boundsMin.x);
    float height = std::max(1.0f, boundsMax.y - boundsMin.y);

    float scaleX = (canvasSize.x - 2.0f * padding) / width;
    float scaleY = (canvasSize.y - 2.0f * padding) / height;
    float targetScale = std::min(scaleX, scaleY);
    targetScale = clampf(targetScale, minScale, maxScale);
    transform.scale = targetScale;

    float usedWidth = width * transform.scale;
    float usedHeight = height * transform.scale;
    float padX = std::max(padding, (canvasSize.x - usedWidth) * 0.5f);
    float padY = std::max(padding, (canvasSize.y - usedHeight) * 0.5f);

    // With screenOrigin at (0,0), offset is the world-space top-left of the screen.
    transform.offset.x = boundsMin.x - padX / transform.scale;
    transform.offset.y = boundsMin.y - padY / transform.scale;
}

void SchematicView::zoomBy(float factor, const ImVec2& anchor) {
    const float oldScale = transform.scale;
    transform.scale = clampf(transform.scale * factor, minScale, maxScale);
    if (transform.scale == oldScale) return;
    // world = (anchor - screenOrigin) / scale + offset, kept constant.
    const ImVec2 a(anchor.x - transform.screenOrigin.x, anchor.y - transform.screenOrigin.y);
    transform.offset.x += a.x / oldScale - a.x / transform.scale;
    transform.offset.y += a.y / oldScale - a.y / transform.scale;
    hasUserInteraction_ = true;
}

void SchematicView::requestFit(bool resetInteraction) {
    needsFit_ = true;
    if (resetInteraction) {
        hasUserInteraction_ = false;
    }
}

void SchematicView::requestFitRect(const ImVec2& worldMin, const ImVec2& worldMax) {
    requestFit(true);
    hasFitRect_ = true;
    fitRectMin_ = worldMin;
    fitRectMax_ = worldMax;
}

void SchematicView::updateFitIfNeeded(const ImVec2& canvasPos, const ImVec2& canvasSize, float padding) {
    if (!needsFit_ || hasUserInteraction_) return;
    if (hasFitRect_) fitToRect(fitRectMin_, fitRectMax_, canvasSize, padding);
    else             fitToContents(canvasPos, canvasSize, padding);
    hasFitRect_ = false;
    needsFit_ = false;
}

void SchematicView::handleInteraction(const ImVec2& canvasPos, const ImVec2& canvasSize) {
    ImGuiIO& io = ImGui::GetIO();
    const bool allowKeyboard = ImGui::IsItemHovered() || ImGui::IsItemActive();

    // Middle mouse drag to pan (also allow right mouse drag)
    if (ImGui::IsItemActive() && (ImGui::IsMouseDragging(ImGuiMouseButton_Middle) || ImGui::IsMouseDragging(ImGuiMouseButton_Right))) {
        ImVec2 delta = io.MouseDelta;
        transform.offset.x -= delta.x / transform.scale;
        transform.offset.y -= delta.y / transform.scale;
        hasUserInteraction_ = true;
    }

    // Zoom with wheel when hovered
    if (ImGui::IsItemHovered()) {
        float wheel = io.MouseWheel;
        if (wheel != 0.0f) {
            hasUserInteraction_ = true;
            float oldScale = transform.scale;
            float zoomFactor = (wheel > 0.0f) ? 1.1f : 0.9f;
            transform.scale = clampf(transform.scale * zoomFactor, minScale, maxScale);

            // Zoom to mouse position (keeps mouse world point stable)
            ImVec2 mousePos = io.MousePos;
            ImVec2 mouseWorldBefore = ImVec2(
                (mousePos.x - canvasPos.x) / oldScale + transform.offset.x - transform.screenOrigin.x / oldScale,
                (mousePos.y - canvasPos.y) / oldScale + transform.offset.y - transform.screenOrigin.y / oldScale
            );
            ImVec2 mouseWorldAfter = ImVec2(
                (mousePos.x - canvasPos.x) / transform.scale + transform.offset.x - transform.screenOrigin.x / transform.scale,
                (mousePos.y - canvasPos.y) / transform.scale + transform.offset.y - transform.screenOrigin.y / transform.scale
            );
            transform.offset.x += (mouseWorldBefore.x - mouseWorldAfter.x);
            transform.offset.y += (mouseWorldBefore.y - mouseWorldAfter.y);
        }
    }

    // Bare keys only: with Ctrl/Cmd/Alt held they belong to the app's
    // shortcuts (View menu, AppLogic.cpp), which would otherwise act twice.
    if (allowKeyboard && !io.KeyCtrl && !io.KeySuper && !io.KeyAlt) {
        float panSpeed = 420.0f * io.DeltaTime / std::max(0.001f, transform.scale);
        if (ImGui::IsKeyDown(ImGuiKey_A)) {
            transform.offset.x -= panSpeed;
            hasUserInteraction_ = true;
        }
        if (ImGui::IsKeyDown(ImGuiKey_D)) {
            transform.offset.x += panSpeed;
            hasUserInteraction_ = true;
        }
        if (ImGui::IsKeyDown(ImGuiKey_W)) {
            transform.offset.y -= panSpeed;
            hasUserInteraction_ = true;
        }
        if (ImGui::IsKeyDown(ImGuiKey_S)) {
            transform.offset.y += panSpeed;
            hasUserInteraction_ = true;
        }

        // Keyboard zoom is about the canvas center (the wheel's is about the
        // cursor).
        const ImVec2 center(canvasSize.x * 0.5f, canvasSize.y * 0.5f);
        if (ImGui::IsKeyDown(ImGuiKey_Equal) || ImGui::IsKeyDown(ImGuiKey_KeypadAdd)) zoomBy(1.02f, center);
        if (ImGui::IsKeyDown(ImGuiKey_Minus) || ImGui::IsKeyDown(ImGuiKey_KeypadSubtract)) zoomBy(0.98f, center);

        if (ImGui::IsKeyPressed(ImGuiKey_0) || ImGui::IsKeyPressed(ImGuiKey_Home)) {
            requestFit(true);
        }
    }
}

// ---------------------------------------------------------------------------
// Export structure: the data-* attributes of each drawn object's <g>
// (SchematicPainter::BeginGroup; only the SVG keeps them).
// ---------------------------------------------------------------------------
namespace {

std::string idPathText(const InstancePath& path) {
    std::string out;
    for (size_t i = 0; i < path.size(); ++i) out += (i ? "," : "") + std::to_string(path[i].id);
    return out;
}

// True when the "u1/u2" (or "u1/u2:Q") text can't be read back: a name
// that is empty (anonymous) or holds a separator, as escaped Verilog names
// can. The group then also carries the path as JSON.
bool ambiguous(const InstancePath& path, const std::string& term = "") {
    for (const auto& ref : path)
        if (ref.name.empty() || ref.name.find_first_of("/:") != std::string::npos) return true;
    return term.find_first_of("/:") != std::string::npos;
}

std::string jsonText(const nlohmann::json& j) {
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

nlohmann::json namesJson(const InstancePath& path) {
    nlohmann::json names = nlohmann::json::array();
    for (const auto& ref : path) names.push_back(ref.name);
    return names;
}

} // namespace

SchematicPainter::GroupAttrs SchematicView::instanceGroup(const InstanceShape& inst) {
    if (inst.modelName == "port") {
        const Port* p = inst.ports.empty() ? nullptr : &inst.ports[0];
        const char* dir = !p ? "" : p->direction == Direction::Input ? "input"
                                  : p->direction == Direction::Output ? "output" : "inout";
        return { { "class", "port" }, { "data-name", p ? p->name : inst.name }, { "data-direction", dir } };
    }
    if (inst.isSummary)
        return { { "class", "summary" }, { "data-group", inst.summaryGroup },
                 { "data-count", std::to_string(inst.summaryCount) } };
    SchematicPainter::GroupAttrs attrs{ { "class", inst.isHierGroup ? "module" : "instance" },
                                        { "data-path", displayPath(inst.path) },
                                        { "data-id-path", idPathText(inst.path) } };
    if (ambiguous(inst.path)) attrs.push_back({ "data-path-json", jsonText(namesJson(inst.path)) });
    if (!inst.model.empty()) attrs.push_back({ "data-model", inst.model });
    return attrs;
}

SchematicPainter::GroupAttrs SchematicView::netGroup(const std::string& netName, const SchematicNetRef& driver,
                                                     const std::string& label) {
    SchematicPainter::GroupAttrs attrs{ { "class", "net" }, { "data-net", netName } };
    if (driver.port) {
        attrs.push_back({ "data-driver", driver.pin });
    } else {
        attrs.push_back({ "data-driver", displayPath(driver.path) + ":" + driver.pin });
        attrs.push_back({ "data-driver-id-path", idPathText(driver.path) });
        if (ambiguous(driver.path, driver.pin))
            attrs.push_back({ "data-driver-json",
                              jsonText({ { "path", namesJson(driver.path) }, { "terminal", driver.pin } }) });
    }
    if (!label.empty()) attrs.push_back({ "data-label", label });
    return attrs;
}

void SchematicView::render(ImDrawList* drawList, const ImVec2& canvasPos, const ImVec2& canvasSize) {
    ImDrawListPainter painter(drawList);
    render(painter, canvasPos, canvasSize);
}

void SchematicView::render(SchematicPainter& painter, const ImVec2& canvasPos, const ImVec2& canvasSize) {
    SchematicPainter* dl = &painter;
    dl->PushClipRect(canvasPos, ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y));

    // White "paper" canvas -- see kCanvasBgColor.
    dl->AddRectFilled(canvasPos, ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y), kCanvasBgColor);

    // Hierarchy group frames go under everything else -- they only outline
    // which module the traced leaves belong to, and are listed parent-first
    // so nested frames paint over their enclosing one.
    for (const auto& inst : instances) {
        if (inst.isHierGroup) {
            dl->BeginGroup(instanceGroup(inst));
            drawInstance(dl, inst, canvasPos, canvasSize);
            drawReviewBadge(dl, inst, *this, canvasPos, canvasSize);
            dl->EndGroup();
        }
    }

    // Top-level nets draw next (so top-level instances render on top), as
    // before. A net nested inside an expanded instance (containerShapeId set)
    // is drawn later instead -- see drawSubtree below -- so that instance's
    // opaque box fill doesn't get painted over it afterward.
    for (const auto& route : routes) {
        dl->BeginGroup(netGroup(route.netName, route.ref, route.label));
        drawRoute(dl, route, canvasPos, canvasSize);
        dl->EndGroup();
    }
    // Unrouted wires, one group per run of wires from the same driver pin.
    auto drawNets = [&](int containerShapeId) {
        std::optional<std::pair<int, int>> open;
        for (const auto& net : nets) {
            if (net.containerShapeId != containerShapeId || net.routed) continue;
            InstanceShape* src = findInstanceById(net.srcInstance);
            const Port* srcPort = src ? findPortById(*src, net.srcPortId) : nullptr;
            const std::pair<int, int> key{ net.srcInstance, net.srcPortId };
            if (open != key) {
                if (open) dl->EndGroup();
                open.reset();
                if (src && srcPort) {
                    dl->BeginGroup(netGroup(net.netName,
                                            src->modelName == "port"
                                                ? SchematicNetRef{ {}, srcPort->name, true }
                                                : SchematicNetRef{ src->path, srcPort->name, false },
                                            net.label));
                    open = key;
                }
            }
            drawNet(dl, net, canvasPos, canvasSize);
        }
        if (open) dl->EndGroup();
    };
    drawNets(-1);

    // Recursive draw: a box, then the nets internal to it, then its nested
    // children on top of those nets -- so hierarchy embedding never hides a
    // wire under a box's fill or a child under its own internal wiring.
    std::function<void(const InstanceShape&)> drawSubtree = [&](const InstanceShape& inst) {
        dl->BeginGroup(instanceGroup(inst));
        drawInstance(dl, inst, canvasPos, canvasSize);
        drawReviewBadge(dl, inst, *this, canvasPos, canvasSize);
        dl->EndGroup();
        drawNets(inst.id);
        for (const auto& child : instances)
            if (child.parentShapeId == inst.id) drawSubtree(child);
    };
    for (const auto& inst : instances) {
        if (inst.parentShapeId < 0 && !inst.isHierGroup) drawSubtree(inst);
    }

    dl->PopClipRect();
}

// ---------------------------------------------------------------------------
// Export page: the drawing, plus ViewOptions' decorations around it -- a
// title band above, the legend and a caption below. With none of them the
// page is the drawing alone, drawn exactly as before they existed.
// ---------------------------------------------------------------------------
namespace {

constexpr float kTitleFontSize   = 18.0f;
constexpr float kCaptionFontSize = 12.0f;
constexpr float kLegendFontSize  = 12.0f;
constexpr float kLegendSwatchW   = 24.0f;
constexpr float kLegendGap       = 6.0f;   // swatch to label
constexpr float kLegendSpacing   = 18.0f;  // between entries
constexpr ImU32 kTitleColor      = IM_COL32(20, 20, 25, 255);
constexpr ImU32 kCaptionColor    = IM_COL32(70, 70, 80, 255);

struct TextBlock {
    std::vector<std::string> lines;
    float size = 0.0f, lineH = 0.0f, w = 0.0f, h = 0.0f;
};

TextBlock measureText(const std::string& text, float size) {
    TextBlock b;
    if (text.empty()) return b;
    ImFont* font = ImGui::GetFont();
    b.size  = size;
    b.lineH = font->CalcTextSizeA(size, FLT_MAX, 0.0f, "Xg").y;
    size_t from = 0;
    for (;;) {
        const size_t nl = text.find('\n', from);
        b.lines.push_back(text.substr(from, nl == std::string::npos ? std::string::npos : nl - from));
        if (nl == std::string::npos) break;
        from = nl + 1;
    }
    for (const auto& line : b.lines)
        b.w = std::max(b.w, font->CalcTextSizeA(size, FLT_MAX, 0.0f, line.c_str()).x);
    b.h = b.lineH * float(b.lines.size());
    return b;
}

struct ExportPage {
    ImVec2    size, drawingAt, drawing;
    bool      decorated = false;
    TextBlock title, caption;
    ImVec2    titleAt, captionAt;
    struct LegendItem { ImVec2 at; float textW = 0.0f; const SchematicView::LegendEntry* entry = nullptr; };
    std::vector<LegendItem> legend;
    float legendLineH = 0.0f;
};

ExportPage layoutPage(ImVec2 drawing, float margin, const ViewOptions& options,
                      const std::vector<SchematicView::LegendEntry>& legend) {
    ExportPage page;
    page.drawing = drawing;
    page.title   = measureText(options.title, kTitleFontSize);
    page.caption = measureText(options.caption, kCaptionFontSize);
    const bool withLegend = options.legend && !legend.empty();
    page.decorated = !page.title.lines.empty() || !page.caption.lines.empty() || withLegend;
    if (!page.decorated) {
        page.size = drawing;
        return page;
    }
    ImFont* font = ImGui::GetFont();
    // Width: the drawing's, or more for a long title, caption or legend entry.
    float width = std::max({ drawing.x, page.title.w + 2.0f * margin, page.caption.w + 2.0f * margin });
    std::vector<float> textW;
    if (withLegend) {
        page.legendLineH = font->CalcTextSizeA(kLegendFontSize, FLT_MAX, 0.0f, "Xg").y;
        for (const auto& e : legend) {
            textW.push_back(font->CalcTextSizeA(kLegendFontSize, FLT_MAX, 0.0f, e.label.c_str()).x);
            width = std::max(width, kLegendSwatchW + kLegendGap + textW.back() + 2.0f * margin);
        }
    }
    float y = 0.0f;
    if (!page.title.lines.empty()) {
        page.titleAt = ImVec2(margin, margin);
        y = margin + page.title.h;  // the drawing's own margin follows
    }
    page.drawingAt = ImVec2(std::floor((width - drawing.x) * 0.5f), y);
    y += drawing.y;
    if (withLegend) {
        // Entries left to right, wrapped at the page's inner width.
        const float rowH = page.legendLineH + 6.0f;
        float x = margin;
        for (size_t i = 0; i < legend.size(); ++i) {
            const float w = kLegendSwatchW + kLegendGap + textW[i];
            if (x > margin && x + w > width - margin) { x = margin; y += rowH; }
            page.legend.push_back({ ImVec2(x, y), textW[i], &legend[i] });
            x += w + kLegendSpacing;
        }
        y += rowH;
    }
    if (!page.caption.lines.empty()) {
        page.captionAt = ImVec2(margin, y + (withLegend ? 6.0f : 0.0f));
        y = page.captionAt.y + page.caption.h;
    }
    page.size = ImVec2(width, y + margin);
    return page;
}

void paintText(SchematicPainter& p, const TextBlock& b, ImVec2 at, ImU32 color) {
    for (size_t i = 0; i < b.lines.size(); ++i)
        p.AddText(ImGui::GetFont(), b.size, ImVec2(at.x, at.y + b.lineH * float(i)), color, b.lines[i].c_str());
}

void paintPage(SchematicPainter& p, SchematicView& sheet, const ExportPage& page) {
    if (!page.decorated) {
        sheet.render(p, ImVec2(0.0f, 0.0f), page.drawing);
        return;
    }
    p.AddRectFilled(ImVec2(0.0f, 0.0f), page.size, IM_COL32(255, 255, 255, 255));
    sheet.render(p, page.drawingAt, page.drawing);
    if (!page.title.lines.empty()) {
        p.BeginGroup({ { "class", "title" } });
        paintText(p, page.title, page.titleAt, kTitleColor);
        p.EndGroup();
    }
    if (!page.legend.empty()) {
        p.BeginGroup({ { "class", "legend" } });
        for (const auto& item : page.legend) {
            p.BeginGroup({ { "class", "legend-entry" }, { "data-label", item.entry->label } });
            const float midY = item.at.y + page.legendLineH * 0.5f;
            p.AddLine(ImVec2(item.at.x, midY), ImVec2(item.at.x + kLegendSwatchW, midY), item.entry->color, 3.0f);
            p.AddText(ImGui::GetFont(), kLegendFontSize, ImVec2(item.at.x + kLegendSwatchW + kLegendGap, item.at.y),
                      kCaptionColor, item.entry->label.c_str());
            p.EndGroup();
        }
        p.EndGroup();
    }
    if (!page.caption.lines.empty()) {
        p.BeginGroup({ { "class", "caption" } });
        paintText(p, page.caption, page.captionAt, kCaptionColor);
        p.EndGroup();
    }
}

} // namespace

// The sheet an export draws: this view at 1x over the whole drawing, with
// `margin` around it, with no hover or pending feedback -- but the
// selection is kept, so an export can point at something. False if empty.
bool SchematicView::exportSheet(float margin, SchematicView& sheet, ImVec2& size) const {
    ImVec2 lo, hi;
    if (!computeWorldBounds(lo, hi)) return false;
    sheet = *this;
    sheet.exporting = true;
    sheet.hideUiGlyphs = ViewOptions::current().hideUiGlyphs;
    sheet.hoveredPortId = -1;
    sheet.transform = Transform{};
    sheet.transform.offset = ImVec2(lo.x - margin, lo.y - margin);
    size = ImVec2(hi.x - lo.x + 2.0f * margin, hi.y - lo.y + 2.0f * margin);
    return true;
}

std::string SchematicView::exportSvg(float margin) const {
    // At 1x nothing is small enough to fade out (see the label legibility
    // helpers above).
    SchematicView sheet;
    ImVec2 drawing;
    if (!exportSheet(margin, sheet, drawing)) return "";
    const ExportPage page = layoutPage(drawing, margin, ViewOptions::current(), sheet.legend);
    SvgPainter svg(page.size);
    paintPage(svg, sheet, page);
    return svg.finish();
}

std::string SchematicView::exportPng(float scale, float margin) const {
    SchematicView sheet;
    ImVec2 drawing;
    if (!(scale > 0.0f) || !exportSheet(margin, sheet, drawing)) return "";
    const ExportPage page = layoutPage(drawing, margin, ViewOptions::current(), sheet.legend);
    if (double(page.size.x) * scale * double(page.size.y) * scale > kMaxPngPixels) return "";
    // Twice: the first drawing bakes the font sizes it needs into the atlas,
    // which may grow it and so move glyphs drawn before; the second finds
    // every glyph in place.
    {
        RasterPainter warmup(page.size, scale);
        paintPage(warmup, sheet, page);
    }
    RasterPainter raster(page.size, scale);
    paintPage(raster, sheet, page);
    return raster.png();
}

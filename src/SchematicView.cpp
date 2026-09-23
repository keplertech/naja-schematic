#include "SchematicView.h"

#include <imgui.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <functional>
#include <string>

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
constexpr float kInstanceLabelBaseSize = 13.0f; // world-space "1x zoom" size
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

// Schematic-style junction dot: a filled circle marks a real electrical branch
// (one driver pin feeding more than one receiver) -- two wires that merely
// cross on screen without sharing a pin get no dot, so a dot always means
// "connected here" and a bare crossing always means "not connected."
constexpr float kJunctionDotBaseR = 3.0f;
constexpr float kMinJunctionDotR  = 1.25f;

// Top-level design port "flag" half-height (see drawBoundaryPortInstance).
// Unlike pin ticks, a flag never vanishes on zoom-out: it follows its
// neighboring instance boxes (which are always drawn) and is only floored at
// a minimum screen size, so the design boundary stays readable at any zoom.
constexpr float kBoundaryPortHalfHBase = 8.0f;
constexpr float kMinBoundaryPortHalfH  = 3.0f;

// Schematic-style near-monochrome palette: every instance shares the same flat
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
// Top-level design ports get their own tint so they can't be mistaken for an
// instance pin or a small instance box. Green on purpose: diagnosis severities
// use red/amber/blue (DiagnosisStore.cpp), so it doesn't read as a finding.
constexpr ImU32 kBoundaryPortFillColor = IM_COL32(196, 232, 204, 255);
constexpr ImU32 kBoundaryPortLineColor = IM_COL32(28, 110, 60, 255);

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

// Draws a dashed rectangle in screen space (no corner rounding).
// dashPx / gapPx are in screen pixels so they stay legible at any zoom.
void addDashedRect(ImDrawList* dl, ImVec2 rmin, ImVec2 rmax,
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
    seg(rmin.x, rmin.y, rmax.x, rmin.y); // top
    seg(rmax.x, rmin.y, rmax.x, rmax.y); // right
    seg(rmax.x, rmax.y, rmin.x, rmax.y); // bottom
    seg(rmin.x, rmax.y, rmin.x, rmin.y); // left
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
    // port.lx, port.ly are normalized: -0.5..0.5 horizontally/vertically where 0 is center
    float px = inst.x + (inst.w * (0.5f + port.lx)); // convert -0.5..0.5 to 0..1 then * width
    float py = inst.y + (inst.h * (0.5f + port.ly));
    return ImVec2(px, py);
}

// ---------------------------------------------------------------------------
// Shared port drawing (used by all gate renderers)
// ---------------------------------------------------------------------------
static void drawPorts(ImDrawList* dl, const InstanceShape& inst,
                      const SchematicView& sv,
                      const ImVec2& canvasPos, const ImVec2& canvasSize) {
    for (const auto& p : inst.ports) {
        float tickLen = zoomedSizeOrHidden(kPortTickBaseLen, sv.transform.scale, kMinPortTickLen);
        if (tickLen <= 0.0f) continue; // too small to matter at this zoom -- same fade policy as labels

        ImVec2 worldP  = sv.portWorldPos(inst, p);
        ImVec2 screenP = sv.worldToScreen(worldP, canvasPos, canvasSize);
        bool   isLeft  = p.lx < 0.0f;

        // No direction color by default -- direction reads from the pin's
        // side of the box, not a red/green fill. p.color (diagnosis/
        // selection override) is the one case color is still used here.
        ImU32 portColor = p.color != 0 ? p.color : kPinLineColor;
        float tickThickness = std::max(1.0f, 1.5f * sv.transform.scale);

        ImVec2 tickEnd = ImVec2(screenP.x + (isLeft ? -tickLen : tickLen), screenP.y);
        dl->AddLine(screenP, tickEnd, portColor, tickThickness);
        // A merged bus pin gets the classic diagonal bus slash across its
        // tick instead of a plain line, in addition to its "[hi:lo]" label.
        if (p.isBus) {
            ImVec2 mid = ImVec2((screenP.x + tickEnd.x) * 0.5f, screenP.y);
            float slashLen = std::max(4.0f, tickLen * 0.6f);
            dl->AddLine(ImVec2(mid.x, mid.y + slashLen * 0.5f),
                        ImVec2(mid.x + slashLen * 0.35f, mid.y - slashLen * 0.5f),
                        portColor, tickThickness);
        }

        float fontSize = labelFontSize(kPortLabelBaseSize, sv.transform.scale);
        if (!p.name.empty() && fontSize > 0.0f) {
            ImFont* font = ImGui::GetFont();
            // Cap label width (in screen px, scaled with zoom like everything
            // else) so a long bus name can't sprawl across neighboring pins.
            const float kPortLabelMaxWorldWidth = 90.0f;
            std::string label = truncateToWidth(font, fontSize, p.name,
                                                 kPortLabelMaxWorldWidth * sv.transform.scale);
            if (!label.empty()) {
                ImVec2 textSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, label.c_str());
                // Anchored past the tick's tip (not the box edge) so the
                // label doesn't sit on top of the pin's own tick line.
                ImVec2 lblPos   = isLeft
                    ? ImVec2(tickEnd.x - 3.0f - textSize.x, tickEnd.y - textSize.y * 0.5f)
                    : ImVec2(tickEnd.x + 3.0f,               tickEnd.y - textSize.y * 0.5f);
                dl->AddRectFilled(
                    ImVec2(lblPos.x - 2.0f, lblPos.y - 1.0f),
                    ImVec2(lblPos.x + textSize.x + 2.0f, lblPos.y + textSize.y + 1.0f),
                    IM_COL32(30, 30, 30, 190));
                dl->AddText(font, fontSize, lblPos, IM_COL32(225, 225, 225, 235), label.c_str());
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Generic box renderer (fallback for unknown gate types)
// ---------------------------------------------------------------------------
static void drawGenericInstance(ImDrawList* dl, const InstanceShape& inst,
                                const SchematicView& sv,
                                const ImVec2& canvasPos, const ImVec2& canvasSize) {
    ImVec2 rmin, rmax;
    sv.worldRectToScreen(inst.x, inst.y, inst.w, inst.h, canvasPos, canvasSize, rmin, rmax);

    if (inst.w > 0.0f && inst.h > 0.0f) {
        // Schematic-style flat fill: a neutral "paper" tone shared by every
        // box, not a per-category color -- color stays reserved for
        // diagnosis/selection highlighting rather than decorating every
        // instance. Sharp corners (no rounding).
        dl->AddRectFilled(rmin, rmax, kInstanceFillColor, 0.0f);

        if (inst.partialInterface)
            // Schematic-style: dashing alone (not a color) signals "only a
            // subset of the interface is shown" -- same ink as a fully
            // expanded box's solid border, since color stays reserved for
            // diagnosis/selection highlighting.
            addDashedRect(dl, rmin, rmax, kInstanceLineColor, 1.25f);
        else
            dl->AddRect(rmin, rmax, kInstanceLineColor, 0.0f, 0, 1.25f);

        // Diagnosis outline drawn on top so it stays visible regardless of
        // the partialInterface dashed border above.
        if (inst.diagOutline != 0)
            dl->AddRect(rmin, rmax, inst.diagOutline, 0.0f, 0, 3.5f);

        float instFontSize = labelFontSize(kInstanceLabelBaseSize, sv.transform.scale);
        if (!inst.name.empty() && instFontSize > 0.0f) {
            ImFont* font = ImGui::GetFont();
            float padding = std::max(2.0f, 6.0f * sv.transform.scale);
            float maxWidth = (rmax.x - rmin.x) - 2.0f * padding;
            std::string label = truncateToWidth(font, instFontSize, inst.name, maxWidth);
            if (!label.empty()) {
                ImVec2 textSize = font->CalcTextSizeA(instFontSize, FLT_MAX, 0.0f, label.c_str());
                ImVec2 textPos  = ImVec2((rmin.x + rmax.x) * 0.5f - textSize.x * 0.5f,
                                         rmin.y + padding);
                dl->AddText(font, instFontSize, textPos, contrastingTextColor(kInstanceFillColor), label.c_str());
            }
        }

        if (inst.partialInterface) {
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
        // the top-center of the box. Its world-space rect is shared with
        // EquipotentialView's click hit-test via hierToggleGlyphRect().
        if (canShowHierToggle(inst)) {
            float gx0, gy0, gx1, gy1;
            hierToggleGlyphRect(inst, gx0, gy0, gx1, gy1);
            ImVec2 gmin, gmax;
            sv.worldRectToScreen(gx0, gy0, gx1 - gx0, gy1 - gy0, canvasPos, canvasSize, gmin, gmax);
            dl->AddRectFilled(gmin, gmax, IM_COL32(40, 40, 40, 230), 3.0f);
            dl->AddRect(gmin, gmax, IM_COL32(200, 200, 200, 200), 3.0f, 0, 1.5f);
            const char* glyph = inst.hierExpanded ? "-" : "+";
            ImVec2 ts = ImGui::CalcTextSize(glyph);
            ImVec2 c((gmin.x + gmax.x) * 0.5f, (gmin.y + gmax.y) * 0.5f);
            dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), IM_COL32(255, 255, 255, 255), glyph);
        }
    }

    drawPorts(dl, inst, sv, canvasPos, canvasSize);
}

// ---------------------------------------------------------------------------
// assign / buffer — triangle pointing right (input left, output apex right)
// ---------------------------------------------------------------------------
static void drawAssignInstance(ImDrawList* dl, const InstanceShape& inst,
                               const SchematicView& sv,
                               const ImVec2& canvasPos, const ImVec2& canvasSize) {
    ImVec2 tl = sv.worldToScreen(ImVec2(inst.x,          inst.y),          canvasPos, canvasSize);
    ImVec2 bl = sv.worldToScreen(ImVec2(inst.x,          inst.y + inst.h), canvasPos, canvasSize);
    ImVec2 mr = sv.worldToScreen(ImVec2(inst.x + inst.w, inst.y + inst.h * 0.5f), canvasPos, canvasSize);

    // Symbol shape (not fill color) carries the gate identity, schematic-style:
    // same flat neutral fill as every other box.
    dl->AddTriangleFilled(tl, bl, mr, kInstanceFillColor);
    dl->AddTriangle(tl, bl, mr, kInstanceLineColor, 1.25f);
    if (inst.diagOutline != 0)
        dl->AddTriangle(tl, bl, mr, inst.diagOutline, 3.0f);

    // Label ("assign") near top-left of the bounding box, small and subtle
    ImVec2 lblPos = ImVec2(tl.x + 4.0f, tl.y + 4.0f);
    dl->AddText(lblPos, contrastingTextColor(kInstanceFillColor, 180), "=");

    drawPorts(dl, inst, sv, canvasPos, canvasSize);
}

// ---------------------------------------------------------------------------
// Top-level design port — schematic-style "flag": a small pentagon sized to its
// name, flat base toward the design (where the net wire attaches, at the
// pseudo-instance's own x/y -- see the zero-size term box built in
// EquipotentialView.cpp), pointed tip facing outward, off the edge of the
// design. Replaces the generic tick-line pin drawn by drawPorts() for every
// other kind of port: a boundary port isn't a pin on a box, it *is* the box.
// ---------------------------------------------------------------------------
static void drawBoundaryPortInstance(ImDrawList* dl, const InstanceShape& inst,
                                     const SchematicView& sv,
                                     const ImVec2& canvasPos, const ImVec2& canvasSize) {
    if (inst.ports.empty()) return;
    const Port& p = inst.ports[0];

    // Floored, not hidden: top-level ports stay visible as long as instances
    // do (see kBoundaryPortHalfHBase).
    float halfH = std::max(kBoundaryPortHalfHBase * sv.transform.scale, kMinBoundaryPortHalfH);

    ImVec2 anchor = sv.worldToScreen(ImVec2(inst.x, inst.y), canvasPos, canvasSize);
    // This layout is a fixed left-to-right flow (see EquipotentialView.cpp's
    // termLx/termRx placement: primary inputs sit left of the design,
    // primary outputs sit right of it). Every boundary port's tip points
    // rightward -- the direction signal continues past this port, whether
    // that's on into the design (an input) or on off the sheet's right edge
    // (an output) -- flat base toward `anchor`, where the wire attaches.
    float outDir = 1.0f;

    ImU32 outline = p.color != 0 ? p.color : kBoundaryPortLineColor;
    float lineThickness = std::max(1.0f, 1.25f * sv.transform.scale);

    // Name shown whenever instance names are (same visibility threshold),
    // drawn at pin-label size but never below the legible minimum.
    float fontSize = labelFontSize(kInstanceLabelBaseSize, sv.transform.scale) > 0.0f
        ? std::max(kMinLabelFontSize, labelFontSize(kPortLabelBaseSize, sv.transform.scale))
        : 0.0f;
    ImFont* font = ImGui::GetFont();
    ImVec2 textSize = (fontSize > 0.0f && !p.name.empty())
        ? font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, p.name.c_str())
        : ImVec2(0.0f, 0.0f);

    float bodyLen = std::max(halfH * 2.0f, textSize.x + 10.0f);
    float tipLen  = halfH;

    float baseX = anchor.x;
    float bodyX = anchor.x + outDir * bodyLen;
    float tipX  = bodyX + outDir * tipLen;

    ImVec2 pts[5] = {
        { baseX, anchor.y - halfH },
        { bodyX, anchor.y - halfH },
        { tipX,  anchor.y },
        { bodyX, anchor.y + halfH },
        { baseX, anchor.y + halfH },
    };
    dl->AddConvexPolyFilled(pts, 5, kBoundaryPortFillColor);
    dl->AddPolyline(pts, 5, outline, lineThickness, ImDrawFlags_Closed);
    if (inst.diagOutline != 0)
        dl->AddPolyline(pts, 5, inst.diagOutline, 3.0f, ImDrawFlags_Closed);

    if (fontSize > 0.0f && !p.name.empty()) {
        ImVec2 textPos((baseX + bodyX) * 0.5f - textSize.x * 0.5f, anchor.y - textSize.y * 0.5f);
        dl->AddText(font, fontSize, textPos, contrastingTextColor(kBoundaryPortFillColor), p.name.c_str());
    }
}

// ---------------------------------------------------------------------------
// Gate dispatcher — add new model names here as the library grows
// ---------------------------------------------------------------------------
// To add a new gate type:
//   1. Write a static drawXxxInstance() function above with the same signature
//   2. Add an else-if branch below matching its modelName string
// ---------------------------------------------------------------------------
void SchematicView::drawInstance(ImDrawList* dl, const InstanceShape& inst,
                                 const ImVec2& canvasPos, const ImVec2& canvasSize) const {
    if (inst.modelName == "assign") {
        drawAssignInstance(dl, inst, *this, canvasPos, canvasSize);
    } else if (inst.modelName == "port") {
        drawBoundaryPortInstance(dl, inst, *this, canvasPos, canvasSize);
    }
    // else if (inst.modelName == "and2")  { drawAnd2Instance (...); }
    // else if (inst.modelName == "or2")   { drawOr2Instance  (...); }
    // else if (inst.modelName == "dff")   { drawDffInstance  (...); }
    else {
        drawGenericInstance(dl, inst, *this, canvasPos, canvasSize);
    }
}

void SchematicView::drawNet(ImDrawList* dl, const NetWire& net, const ImVec2& canvasPos, const ImVec2& canvasSize) const {
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
    float thickness = zoomedSizeOrHidden(kWireBaseThickness, transform.scale, kMinWireThickness);
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

    // Bus slash mark across the horizontal run, schematic-style, plus the net
    // name if we have one.
    if (net.isBus) {
        ImVec2 mid = ImVec2(midX, (p1.y + p4.y) * 0.5f);
        float slashLen = std::max(6.0f, 9.0f * transform.scale);
        dl->AddLine(ImVec2(mid.x - slashLen * 0.35f, mid.y + slashLen * 0.5f),
                    ImVec2(mid.x + slashLen * 0.35f, mid.y - slashLen * 0.5f),
                    col, thickness);
        if (!net.netName.empty()) {
            float fontSize = labelFontSize(kPortLabelBaseSize, transform.scale);
            if (fontSize > 0.0f) {
                ImFont* font = ImGui::GetFont();
                ImVec2 ts = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, net.netName.c_str());
                ImVec2 lp = ImVec2(mid.x + 6.0f, mid.y - ts.y - 4.0f);
                dl->AddRectFilled(ImVec2(lp.x - 2.0f, lp.y - 1.0f),
                                  ImVec2(lp.x + ts.x + 2.0f, lp.y + ts.y + 1.0f),
                                  IM_COL32(30, 30, 30, 190));
                dl->AddText(font, fontSize, lp, col, net.netName.c_str());
            }
        }
    }
}

// Main render entry
bool SchematicView::computeWorldBounds(ImVec2& outMin, ImVec2& outMax) const {
    if (instances.empty()) return false;
    float minX = instances[0].x;
    float minY = instances[0].y;
    float maxX = instances[0].x + instances[0].w;
    float maxY = instances[0].y + instances[0].h;
    for (const auto& inst : instances) {
        minX = std::min(minX, inst.x);
        minY = std::min(minY, inst.y);
        maxX = std::max(maxX, inst.x + inst.w);
        maxY = std::max(maxY, inst.y + inst.h);
    }
    outMin = ImVec2(minX, minY);
    outMax = ImVec2(maxX, maxY);
    return true;
}

void SchematicView::fitToContents(const ImVec2& /*canvasPos*/, const ImVec2& canvasSize, float padding) {
    ImVec2 boundsMin, boundsMax;
    if (!computeWorldBounds(boundsMin, boundsMax)) return;

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

void SchematicView::zoomBy(float factor) {
    transform.scale = clampf(transform.scale * factor, minScale, maxScale);
}

void SchematicView::requestFit(bool resetInteraction) {
    needsFit_ = true;
    if (resetInteraction) {
        hasUserInteraction_ = false;
    }
}

void SchematicView::updateFitIfNeeded(const ImVec2& canvasPos, const ImVec2& canvasSize, float padding) {
    if (!needsFit_ || hasUserInteraction_) return;
    fitToContents(canvasPos, canvasSize, padding);
    needsFit_ = false;
}

void SchematicView::handleInteraction(const ImVec2& canvasPos, const ImVec2& /*canvasSize*/) {
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

    if (allowKeyboard) {
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

        if (ImGui::IsKeyDown(ImGuiKey_Equal) || ImGui::IsKeyDown(ImGuiKey_KeypadAdd)) {
            float oldScale = transform.scale;
            transform.scale = clampf(transform.scale * 1.02f, minScale, maxScale);
            float deltaScale = transform.scale - oldScale;
            if (deltaScale != 0.0f) hasUserInteraction_ = true;
        }
        if (ImGui::IsKeyDown(ImGuiKey_Minus) || ImGui::IsKeyDown(ImGuiKey_KeypadSubtract)) {
            float oldScale = transform.scale;
            transform.scale = clampf(transform.scale * 0.98f, minScale, maxScale);
            float deltaScale = transform.scale - oldScale;
            if (deltaScale != 0.0f) hasUserInteraction_ = true;
        }

        if (ImGui::IsKeyPressed(ImGuiKey_0) || ImGui::IsKeyPressed(ImGuiKey_Home)) {
            requestFit(true);
        }
    }
}

void SchematicView::render(ImDrawList* dl, const ImVec2& canvasPos, const ImVec2& canvasSize) {
    dl->PushClipRect(canvasPos, ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y), true);

    // Schematic-style white "paper" canvas -- see kCanvasBgColor.
    dl->AddRectFilled(canvasPos, ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y), kCanvasBgColor);

    // Top-level nets draw first (so top-level instances render on top), as
    // before. A net nested inside an expanded instance (containerShapeId set)
    // is drawn later instead -- see drawSubtree below -- so that instance's
    // opaque box fill doesn't get painted over it afterward.
    for (const auto& net : nets) {
        if (net.containerShapeId < 0) drawNet(dl, net, canvasPos, canvasSize);
    }

    // Recursive draw: a box, then the nets internal to it, then its nested
    // children on top of those nets -- so hierarchy embedding never hides a
    // wire under a box's fill or a child under its own internal wiring.
    std::function<void(const InstanceShape&)> drawSubtree = [&](const InstanceShape& inst) {
        drawInstance(dl, inst, canvasPos, canvasSize);
        for (const auto& net : nets)
            if (net.containerShapeId == inst.id) drawNet(dl, net, canvasPos, canvasSize);
        for (const auto& child : instances)
            if (child.parentShapeId == inst.id) drawSubtree(child);
    };
    for (const auto& inst : instances) {
        if (inst.parentShapeId < 0) drawSubtree(inst);
    }

    dl->PopClipRect();
}

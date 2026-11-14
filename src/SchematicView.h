#pragma once

#include <string>
#include <vector>
#include <imgui.h>
#include "Types.h"   // ensures InstanceShape, Port, NetWire are known
#include "SchematicLayout.h"
#include "SchematicPainter.h"

// World width of an instance name drawn with the current ImGui font at
// SchematicLayout::kNameFontSize: what the layout makes room for.
float instanceNameWidth(const std::string& name);
// World width of a net label (Port::netLabel) at
// SchematicLayout::kNetLabelFontSize.
float netLabelWidth(const std::string& label);

// The world rect a shape covers when drawn: the shape, plus its name row or
// its port flag.
void shapeWorldExtent(const InstanceShape& inst, ImVec2& outMin, ImVec2& outMax);

struct Transform {
    float scale = 1.0f;
    ImVec2 offset = ImVec2(0,0);
    ImVec2 screenOrigin = ImVec2(0,0);
};

class SchematicView {
public:
    // Zooms by `factor` about `anchor` (canvas-local pixels): the world point
    // under it stays put on screen. Counts as user interaction (no auto-fit).
    void zoomBy(float factor, const ImVec2& anchor);

    std::vector<InstanceShape> instances;
    std::vector<NetWire> nets;
    // A routed net: one orthogonal wire tree from a driving pin to all its
    // receivers, in world coordinates (SchematicLayout::routeNets). Its
    // NetWires are marked `routed` and aren't drawn on their own.
    struct NetRoute {
        std::vector<SchematicLayout::RouteSegment> segments;
        std::vector<ImVec2> junctions;   // three or more directions meet here
        ImU32       color = NetWire{}.color;
        // Two or more visible traces share this tree: it's drawn striped in
        // their colors (world-anchored dashes) rather than in `color` (the
        // convergence color), which is kept for when stripes are too short
        // to read at the current zoom.
        std::vector<ImU32> stripeColors;
        bool        isBus = false;
        std::string netName;
        // Host styling (StyleStore): wire width in world units (0 = the
        // default), and a label (the legend's). Drawn as `labelText` (the
        // driver pin's Port::netLabel) starting at `labelAt`, on its bottom
        // line, when `hasLabel` (SchematicLayout::netLabelPlaces()), else on
        // the longest horizontal run.
        float       width = 0.0f;
        std::string label;
        bool        hasLabel = false;
        ImVec2      labelAt;
        std::string labelText;
        // The net this tree draws, and whether it's the selected one
        // (SelectionStore): drawn over a selection-colored halo.
        SchematicNetRef ref;
        bool        selected = false;
    };
    std::vector<NetRoute> routes;
    Transform transform;
    // Pin under the cursor (drawn highlighted), -1 = none. Set each frame by
    // the owning view, which does the hit-testing.
    int hoveredPortId = -1;

    float minScale = 0.1f;
    float maxScale = 6.0f;

    // Coordinate conversions
    ImVec2 worldToScreen(const ImVec2& world, const ImVec2& canvasPos, const ImVec2& canvasSize) const;
    void worldRectToScreen(float x, float y, float w, float h,
                           const ImVec2& canvasPos, const ImVec2& canvasSize,
                           ImVec2& outMin, ImVec2& outMax) const;

    // Helpers (marked const so they can be used from const methods)
    InstanceShape* findInstanceById(int id) const;
    Port* findPortById(InstanceShape& inst, int portId) const;

    // Port absolute position in world coords
    ImVec2 portWorldPos(const InstanceShape& inst, const Port& port) const;

    // Interaction + view helpers
    void handleInteraction(const ImVec2& canvasPos, const ImVec2& canvasSize);
    void requestFit(bool resetInteraction = false);
    void updateFitIfNeeded(const ImVec2& canvasPos, const ImVec2& canvasSize, float padding = 40.0f);
    // Like requestFit(true), but fits the given world-space rect instead of
    // the whole contents (e.g. zooming onto one hierarchy frame).
    void requestFitRect(const ImVec2& worldMin, const ImVec2& worldMax);

    // Draw helpers (const)
    void drawInstance(SchematicPainter* dl, const InstanceShape& inst,
                      const ImVec2& canvasPos, const ImVec2& canvasSize) const;
    void drawNet(SchematicPainter* dl, const NetWire& net,
                 const ImVec2& canvasPos, const ImVec2& canvasSize) const;
    void drawRoute(SchematicPainter* dl, const NetRoute& route,
                   const ImVec2& canvasPos, const ImVec2& canvasSize) const;

    // Main render entry (parameters by const reference)
    void render(ImDrawList* dl, const ImVec2& canvasPos, const ImVec2& canvasSize);
    void render(SchematicPainter& painter, const ImVec2& canvasPos, const ImVec2& canvasSize);

    // The whole schematic as an SVG document, at 1x with `margin` world
    // units around it, without transient interaction feedback (hover,
    // pending pins); the selection is kept. "" when there is nothing to draw.
    // Each net, instance, port flag, summary and module frame is a <g> with
    // data-* attributes naming it (the drawing itself is unchanged). The
    // export decorations of ViewOptions::current() apply (title band above,
    // legend and caption below, hide_ui_glyphs); exportPng() draws the same.
    std::string exportSvg(float margin = 24.0f) const;
    // The same drawing as a PNG file's bytes, `scale` pixels per SVG pixel
    // (RasterPainter: every stroke, radius and font size scaled with it,
    // over a white background). "" when nothing is drawn, or when the image
    // would be larger than kMaxPngPixels.
    std::string exportPng(float scale = 1.0f, float margin = 24.0f) const;
    static constexpr double kMaxPngPixels = 120e6;
    // Set on the copy exportSvg() draws: skips transient interaction feedback.
    bool exporting = false;
    // Set on the export copy by ViewOptions' hide_ui_glyphs: leaves out the
    // "more pins" dots of a partial box and the hollow stubs of open pins,
    // which only mean something to a viewer's user (clicks).
    bool hideUiGlyphs = false;

    // One row of an export's legend (ViewOptions' legend): a net color and
    // what it stands for -- a visible trace's label, or a host style's
    // label on a net. Filled by EquipotentialView with the shapes.
    struct LegendEntry {
        ImU32       color = 0;
        std::string label;
    };
    std::vector<LegendEntry> legend;

    bool computeWorldBounds(ImVec2& outMin, ImVec2& outMax) const;

    // A review badge as drawn, in screen coordinates: an instance box's or
    // module frame's (portId -1), or a pin's (a top-level port's flag
    // included). What the view hit-tests to show and open the threads.
    struct ReviewBadge {
        int    instanceId = -1;
        int    portId = -1;
        ImVec2 center;
        float  radius = 0.0f;
    };
    // Every badge shown at the current zoom (hidden with the labels).
    std::vector<ReviewBadge> reviewBadges(const ImVec2& canvasPos, const ImVec2& canvasSize) const;

    // The <g> attributes the export gives an instance box, port flag,
    // fanout summary or module frame, and a net (named by its driver pin).
    static SchematicPainter::GroupAttrs instanceGroup(const InstanceShape& inst);
    static SchematicPainter::GroupAttrs netGroup(const std::string& netName, const SchematicNetRef& driver,
                                                 const std::string& label);

private:
    bool exportSheet(float margin, SchematicView& sheet, ImVec2& size) const;

    bool needsFit_ = true;
    bool hasUserInteraction_ = false;
    bool hasFitRect_ = false;
    ImVec2 fitRectMin_{}, fitRectMax_{};

    void fitToContents(const ImVec2& canvasPos, const ImVec2& canvasSize, float padding);
    void fitToRect(ImVec2 boundsMin, ImVec2 boundsMax, const ImVec2& canvasSize, float padding);
};

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
    void zoomBy(float factor);

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
    // units around it, without interaction feedback (hover, selection,
    // pending pins). "" when there is nothing to draw.
    std::string exportSvg(float margin = 24.0f) const;
    // Set on the copy exportSvg() draws: skips interaction feedback.
    bool exporting = false;

    bool computeWorldBounds(ImVec2& outMin, ImVec2& outMax) const;

private:
    bool needsFit_ = true;
    bool hasUserInteraction_ = false;
    bool hasFitRect_ = false;
    ImVec2 fitRectMin_{}, fitRectMax_{};

    void fitToContents(const ImVec2& canvasPos, const ImVec2& canvasSize, float padding);
    void fitToRect(ImVec2 boundsMin, ImVec2 boundsMax, const ImVec2& canvasSize, float padding);
};

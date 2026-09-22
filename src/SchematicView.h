#pragma once

#include <vector>
#include <imgui.h>
#include "Types.h"   // ensures InstanceShape, Port, NetWire are known

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
    Transform transform;

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

    // Draw helpers (const)
    void drawInstance(ImDrawList* dl, const InstanceShape& inst,
                      const ImVec2& canvasPos, const ImVec2& canvasSize) const;
    void drawNet(ImDrawList* dl, const NetWire& net,
                 const ImVec2& canvasPos, const ImVec2& canvasSize) const;

    // Main render entry (parameters by const reference)
    void render(ImDrawList* dl, const ImVec2& canvasPos, const ImVec2& canvasSize);

    bool computeWorldBounds(ImVec2& outMin, ImVec2& outMax) const;

private:
    bool needsFit_ = true;
    bool hasUserInteraction_ = false;

    void fitToContents(const ImVec2& canvasPos, const ImVec2& canvasSize, float padding);
};

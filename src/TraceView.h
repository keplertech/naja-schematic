#pragma once

struct GUIData;

// Renders the "Traces" bottom-panel tab: one row per driver trace in the
// schematic (see TraceStore), to show/hide it, change its color or take it
// out of the view.
class TraceView {
  public:
    static void render(GUIData& data);
};

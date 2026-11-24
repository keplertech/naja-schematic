#pragma once

// Theme: the app's colors, panels and schematic together. One preset is
// current (theme()); View > Theme, the native --theme flag and
// $NAJA_SCHEMATIC_THEME (initialTheme()) pick it.
//
// The schematic's colors are a SchematicPalette, carried by each
// SchematicView (SchematicView::colors). Its default is the paper palette:
// near-black ink on a white sheet. Exports and the headless renderers
// always draw with it, whatever the theme, since they end up in documents.
//
// EquipotentialView bakes two colors into the shapes it builds -- the
// default wire color and the trace convergence color -- before the view
// that draws them is known. It always bakes the paper ones
// (kPaperWireColor, kPaperConvergenceColor), and SchematicView maps them
// to its palette's when it paints (SchematicPalette::ink()).

#include <imgui.h>

#include <optional>
#include <string_view>

// The default wire color and the convergence color (the color of a wire
// tree, and the outline of a gate, two visible traces go through), as
// baked into shapes. TraceStore refuses trace colors close to black or
// white, so a trace never looks like the convergence color, or vanishes
// into the canvas, in either a light or a dark palette.
inline constexpr ImU32 kPaperWireColor        = IM_COL32(150, 150, 150, 255);
inline constexpr ImU32 kPaperConvergenceColor = IM_COL32(30, 30, 30, 255);

struct SchematicPalette {
    ImU32 canvas        = IM_COL32(255, 255, 255, 255);
    ImU32 instanceFill  = IM_COL32(214, 216, 220, 255);
    ImU32 instanceLine  = IM_COL32(35, 35, 38, 235);
    ImU32 pinLine       = IM_COL32(40, 40, 40, 235);
    ImU32 pinLabel      = IM_COL32(45, 45, 50, 235);
    ImU32 instanceName  = IM_COL32(30, 40, 90, 235);
    ImU32 instanceModel = IM_COL32(95, 95, 110, 235);
    // Top-level port flags: green, so they don't read as a diagnosis
    // severity (red/amber/blue).
    ImU32 portFill      = IM_COL32(196, 232, 204, 255);
    ImU32 portLine      = IM_COL32(28, 110, 60, 255);
    // Interaction feedback: the pin under the cursor (and its halo), a pin
    // whose net is loading, the selection.
    ImU32 hover         = IM_COL32(20, 110, 235, 255);
    ImU32 hoverHalo     = IM_COL32(20, 110, 235, 45);
    ImU32 pending       = IM_COL32(150, 90, 220, 255);
    ImU32 selection     = IM_COL32(20, 110, 235, 255);
    // Module frames (View > Show Hierarchy): the tint's alpha is set by
    // depth, so `frameTint`'s own is ignored.
    ImU32 frameTint     = IM_COL32(110, 125, 175, 0);
    ImU32 frameLine     = IM_COL32(70, 80, 115, 220);
    ImU32 frameLabel    = IM_COL32(35, 40, 60, 235);
    ImU32 wire          = kPaperWireColor;
    ImU32 convergence   = kPaperConvergenceColor;
    // Export decorations (title, caption, legend labels).
    ImU32 title         = IM_COL32(20, 20, 25, 255);
    ImU32 caption       = IM_COL32(70, 70, 80, 255);
    // The empty view's hint text.
    ImU32 hint          = IM_COL32(120, 120, 120, 255);

    // A baked color (see above) in this palette.
    ImU32 ink(ImU32 c) const {
        if (c == kPaperWireColor) return wire;
        if (c == kPaperConvergenceColor) return convergence;
        return c;
    }
    bool operator==(const SchematicPalette&) const = default;
};

enum class ThemeId { Classic, Light, Dark };

struct Theme {
    ThemeId     id;
    const char* name;   // for --theme and $NAJA_SCHEMATIC_THEME
    const char* label;  // for the View > Theme menu
    bool        lightPanels;  // ImGui's light style, else its dark one
    SchematicPalette schematic;
    // Pin rows in the tree and the Equipotential table: a driver (a
    // top-level input, an instance output), a receiver, an inout.
    ImU32  driverText, receiverText, inoutText;
    // The schematic's scroll sliders.
    ImVec4 scrollFrame, scrollGrab, scrollGrabActive;
    // The About dialog's title.
    ImVec4 titleText;
    // The Log tab's warning and error lines.
    ImU32  warningText, errorText;
};

inline constexpr int kThemeCount = 3;
const Theme& themeAt(int i);           // 0 <= i < kThemeCount, menu order
const Theme& theme();                  // the current one (Classic at start)
void setTheme(ThemeId id);             // also restyles ImGui, if it has a context
// The ImGui colors of the current theme on `style` (setUiScale() resets
// the style, so it reapplies them).
void applyThemeColors(ImGuiStyle& style);
const SchematicPalette& paperPalette();
std::optional<ThemeId> parseTheme(std::string_view name);
// $NAJA_SCHEMATIC_THEME if set to a theme name, else Classic.
ThemeId initialTheme();

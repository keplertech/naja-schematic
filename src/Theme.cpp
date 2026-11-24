// Theme.cpp — the app's color themes (see Theme.h).
#include "Theme.h"

#include <cstdlib>

namespace {

SchematicPalette darkSchematic() {
    SchematicPalette p;
    p.canvas        = IM_COL32(30, 32, 36, 255);
    p.instanceFill  = IM_COL32(56, 60, 68, 255);
    p.instanceLine  = IM_COL32(200, 204, 212, 235);
    p.pinLine       = IM_COL32(190, 192, 198, 235);
    p.pinLabel      = IM_COL32(210, 212, 220, 235);
    p.instanceName  = IM_COL32(150, 178, 245, 235);
    p.instanceModel = IM_COL32(150, 152, 168, 235);
    p.portFill      = IM_COL32(36, 84, 52, 255);
    p.portLine      = IM_COL32(110, 205, 140, 255);
    p.hover         = IM_COL32(80, 160, 255, 255);
    p.hoverHalo     = IM_COL32(80, 160, 255, 55);
    p.pending       = IM_COL32(185, 140, 250, 255);
    p.selection     = IM_COL32(80, 160, 255, 255);
    p.frameTint     = IM_COL32(120, 140, 210, 0);
    p.frameLine     = IM_COL32(125, 140, 190, 220);
    p.frameLabel    = IM_COL32(185, 195, 228, 235);
    p.wire          = IM_COL32(140, 145, 152, 255);
    p.convergence   = IM_COL32(235, 235, 235, 255);
    p.title         = IM_COL32(235, 235, 240, 255);
    p.caption       = IM_COL32(180, 180, 190, 255);
    p.hint          = IM_COL32(140, 140, 140, 255);
    return p;
}

const Theme& themeData(int i) {
    // Saturated text reads on ImGui's dark panels; the light ones need
    // darker shades.
    static const Theme kThemes[kThemeCount] = {
        { ThemeId::Classic, "classic", "Classic (dark panels, white sheet)", false, SchematicPalette{},
          IM_COL32(255, 0, 0, 255), IM_COL32(0, 255, 0, 255), IM_COL32(255, 255, 0, 255),
          ImVec4(0.12f, 0.12f, 0.12f, 1), ImVec4(0.45f, 0.45f, 0.45f, 1), ImVec4(0.65f, 0.65f, 0.65f, 1),
          ImVec4(0.9f, 0.9f, 1.0f, 1.0f),
          IM_COL32(255, 200, 60, 255), IM_COL32(255, 90, 90, 255) },
        { ThemeId::Light, "light", "Light", true, SchematicPalette{},
          IM_COL32(200, 30, 30, 255), IM_COL32(20, 135, 40, 255), IM_COL32(165, 120, 0, 255),
          ImVec4(0.86f, 0.86f, 0.86f, 1), ImVec4(0.62f, 0.62f, 0.62f, 1), ImVec4(0.45f, 0.45f, 0.45f, 1),
          ImVec4(0.1f, 0.15f, 0.35f, 1.0f),
          IM_COL32(160, 100, 0, 255), IM_COL32(190, 20, 20, 255) },
        { ThemeId::Dark, "dark", "Dark", false, darkSchematic(),
          IM_COL32(255, 95, 95, 255), IM_COL32(95, 225, 120, 255), IM_COL32(240, 215, 80, 255),
          ImVec4(0.12f, 0.12f, 0.12f, 1), ImVec4(0.45f, 0.45f, 0.45f, 1), ImVec4(0.65f, 0.65f, 0.65f, 1),
          ImVec4(0.9f, 0.9f, 1.0f, 1.0f),
          IM_COL32(240, 200, 80, 255), IM_COL32(255, 105, 105, 255) },
    };
    return kThemes[i];
}

int g_current = 0;

} // namespace

const Theme& themeAt(int i) { return themeData(i); }

const Theme& theme() { return themeData(g_current); }

void applyThemeColors(ImGuiStyle& style) {
    if (theme().lightPanels) ImGui::StyleColorsLight(&style);
    else                     ImGui::StyleColorsDark(&style);
}

void setTheme(ThemeId id) {
    for (int i = 0; i < kThemeCount; ++i)
        if (themeData(i).id == id) g_current = i;
    if (ImGui::GetCurrentContext()) applyThemeColors(ImGui::GetStyle());
}

const SchematicPalette& paperPalette() {
    static const SchematicPalette kPaper;
    return kPaper;
}

std::optional<ThemeId> parseTheme(std::string_view name) {
    for (int i = 0; i < kThemeCount; ++i)
        if (name == themeData(i).name) return themeData(i).id;
    return std::nullopt;
}

ThemeId initialTheme() {
    const char* env = std::getenv("NAJA_SCHEMATIC_THEME");
    if (!env || !*env) return ThemeId::Classic;
    return parseTheme(env).value_or(ThemeId::Classic);
}

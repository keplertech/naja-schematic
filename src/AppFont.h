#pragma once

struct ImFont;
struct ImFontAtlas;

// Base UI font size in pixels: the app's default font is drawn at it, and
// schematic labels scale off of it (SchematicView's labelFontSize()).
inline constexpr float kBaseFontSizePx = 17.0f;

// Adds the app's default font to `atlas`: DroidSans, with the symbols it
// lacks (arrows, math operators, ...: SymbolFont.h) merged in, so a label
// never draws the replacement glyph. What setupFonts() loads first, and
// what the tests measure and draw with.
ImFont* addAppFont(ImFontAtlas* atlas);

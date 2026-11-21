#pragma once

// UI scale: multiplies the size of the app's text (menus, panels, dialogs,
// the Source tab) and its spacing (padding, rounding, ...), for large or
// high-resolution displays. 1 = kBaseFontSizePx text. The schematic's own
// labels are left alone: they follow the schematic's zoom. Clamped to
// [kMinUiScale, kMaxUiScale]. View > Text Size, the native --ui-scale flag
// and $NAJA_SCHEMATIC_UI_SCALE (initialUiScale()) set it. Needs an ImGui
// context.
inline constexpr float kMinUiScale = 0.5f;
inline constexpr float kMaxUiScale = 4.0f;
void  setUiScale(float scale);
float uiScale();
// $NAJA_SCHEMATIC_UI_SCALE if set to a positive number, else 1.
float initialUiScale();

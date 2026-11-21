// UiScale.cpp — the app's text and spacing scale (see UiScale.h).
#include "UiScale.h"

#include <algorithm>
#include <cstdlib>

#include <imgui.h>

namespace {
float g_uiScale = 1.0f;
}

void setUiScale(float scale) {
  if (!(scale > 0.0f)) scale = 1.0f;  // also NaN
  scale = std::clamp(scale, kMinUiScale, kMaxUiScale);
  ImGuiStyle& style = ImGui::GetStyle();
  // ScaleAllSizes() compounds (and truncates), so always start over from
  // ImGui's default style (the app doesn't customize it), keeping what
  // ImGui itself set.
  const float fontSizeBase = style.FontSizeBase, fontScaleDpi = style.FontScaleDpi;
  style = ImGuiStyle();
  style.FontSizeBase = fontSizeBase;
  style.FontScaleDpi = fontScaleDpi;
  if (scale != 1.0f) style.ScaleAllSizes(scale);
  // Fonts are dynamic (ImGui >= 1.92): text is rasterized at the scaled
  // size, so it stays crisp.
  style.FontScaleMain = scale;
  g_uiScale = scale;
}

float uiScale() { return g_uiScale; }

float initialUiScale() {
  const char* env = std::getenv("NAJA_SCHEMATIC_UI_SCALE");
  if (!env || !*env) return 1.0f;
  char* end = nullptr;
  const float v = std::strtof(env, &end);
  return (end != env && !*end && v > 0.0f) ? v : 1.0f;
}

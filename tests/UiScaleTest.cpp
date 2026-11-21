#include "UiScale.h"

#include <cstdlib>

#include <gtest/gtest.h>
#include <imgui.h>

namespace {

class UiScale : public ::testing::Test {
 protected:
  void SetUp() override { ctx = ImGui::CreateContext(); }
  void TearDown() override {
    setUiScale(1.0f);
    ImGui::DestroyContext(ctx);
    unsetenv("NAJA_SCHEMATIC_UI_SCALE");
  }
  ImGuiContext* ctx = nullptr;
};

}  // namespace

TEST_F(UiScale, ScalesTextAndSpacing) {
  const ImGuiStyle base;
  setUiScale(2.0f);
  const ImGuiStyle& s = ImGui::GetStyle();
  EXPECT_FLOAT_EQ(uiScale(), 2.0f);
  EXPECT_FLOAT_EQ(s.FontScaleMain, 2.0f);
  EXPECT_FLOAT_EQ(s.FramePadding.x, base.FramePadding.x * 2);
  EXPECT_FLOAT_EQ(s.ItemSpacing.y, base.ItemSpacing.y * 2);
}

// Changing the scale never compounds: each one is from the default style.
TEST_F(UiScale, RescalingStartsOverFromTheDefaultStyle) {
  const ImGuiStyle base;
  setUiScale(3.0f);
  setUiScale(2.0f);
  EXPECT_FLOAT_EQ(ImGui::GetStyle().WindowPadding.x, base.WindowPadding.x * 2);
  setUiScale(1.0f);
  const ImGuiStyle& s = ImGui::GetStyle();
  EXPECT_FLOAT_EQ(s.FontScaleMain, 1.0f);
  EXPECT_FLOAT_EQ(s.WindowPadding.x, base.WindowPadding.x);
  EXPECT_FLOAT_EQ(s.FramePadding.y, base.FramePadding.y);
}

TEST_F(UiScale, KeepsWhatImGuiSet) {
  ImGui::GetStyle().FontSizeBase = 17.0f;
  ImGui::GetStyle().FontScaleDpi = 1.5f;
  setUiScale(2.0f);
  EXPECT_FLOAT_EQ(ImGui::GetStyle().FontSizeBase, 17.0f);
  EXPECT_FLOAT_EQ(ImGui::GetStyle().FontScaleDpi, 1.5f);
}

TEST_F(UiScale, Clamped) {
  setUiScale(100.0f);
  EXPECT_FLOAT_EQ(uiScale(), kMaxUiScale);
  setUiScale(0.01f);
  EXPECT_FLOAT_EQ(uiScale(), kMinUiScale);
  setUiScale(-1.0f);
  EXPECT_FLOAT_EQ(uiScale(), 1.0f);
}

TEST_F(UiScale, InitialFromEnvironment) {
  unsetenv("NAJA_SCHEMATIC_UI_SCALE");
  EXPECT_FLOAT_EQ(initialUiScale(), 1.0f);
  setenv("NAJA_SCHEMATIC_UI_SCALE", "1.75", 1);
  EXPECT_FLOAT_EQ(initialUiScale(), 1.75f);
  setenv("NAJA_SCHEMATIC_UI_SCALE", "big", 1);
  EXPECT_FLOAT_EQ(initialUiScale(), 1.0f);
  setenv("NAJA_SCHEMATIC_UI_SCALE", "-2", 1);
  EXPECT_FLOAT_EQ(initialUiScale(), 1.0f);
}

// Color themes (Theme.h): the presets, ImGui's style following them, and
// the schematic drawn in the theme's palette on screen but on paper in
// exports -- including the two colors EquipotentialView bakes into shapes.
#include "EquipotentialView.h"
#include "SchematicPainter.h"
#include "SchematicView.h"
#include "SelectionStore.h"
#include "StyleStore.h"
#include "Theme.h"
#include "TraceStore.h"
#include "UiScale.h"
#include "ViewOptions.h"

#include <cstdlib>
#include <string>

#include <gtest/gtest.h>
#include <imgui.h>

#include "FakeNetlistProvider.h"

namespace {

std::string rgb(ImU32 c) {
  return "rgb(" + std::to_string((c >> IM_COL32_R_SHIFT) & 0xFF) + "," +
         std::to_string((c >> IM_COL32_G_SHIFT) & 0xFF) + "," +
         std::to_string((c >> IM_COL32_B_SHIFT) & 0xFF) + ")";
}

float luminance(const ImVec4& c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }

const Theme& themeById(ThemeId id) {
  for (int i = 0; i < kThemeCount; ++i)
    if (themeAt(i).id == id) return themeAt(i);
  return themeAt(0);
}

InstTermOccurrence occ(unsigned id, const std::string& name, const std::string& term, unsigned termId,
                       Direction dir) {
  InstTermOccurrence o;
  o.path       = { { id, name } };
  o.pathIds    = { id };
  o.pathModels = { "AND2" };
  o.term       = BitTerm{ term, termId, dir, std::nullopt };
  o.bit_term_count = 3;
  return o;
}

class ThemeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ImGui::CreateContext();
    ImGuiIO& io    = ImGui::GetIO();
    io.DisplaySize = ImVec2(1200.f, 800.f);
    io.DeltaTime   = 1.f / 60.f;
    io.IniFilename = nullptr;
    unsigned char* px; int w, h;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    EquipotentialView::setProvider(&provider);
    EquipotentialView::resetLayout();
    SelectionStore::clear();
    TraceStore::clear();
    StyleStore::clear();
    ViewOptions::current() = ViewOptions{};
  }
  void TearDown() override {
    setTheme(ThemeId::Classic);
    EquipotentialView::setProvider(nullptr);
    EquipotentialView::resetLayout();
    TraceStore::clear();
    for (auto* eq : eqs) delete eq;
    ImGui::DestroyContext();
    unsetenv("NAJA_SCHEMATIC_THEME");
  }

  void frame(int n = 3) {
    for (int i = 0; i < n; ++i) {
      unsigned char* px; int w, h;
      ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
      ImGui::NewFrame();
      ImGui::SetNextWindowPos(ImVec2(0.f, 0.f));
      ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
      ImGui::Begin("Schematic", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
      EquipotentialView::renderSchematic(eqs);
      ImGui::End();
      ImGui::Render();
    }
  }

  // Two nets: u1.Q -> u2.A untraced (the default wire color), and
  // u2.Q -> u3.A on two visible traces (the convergence color).
  void scene() {
    Equipotential plain{ true, {}, {} };
    plain.occurrences.push_back(occ(1, "u1", "Q", 2, Direction::Output));
    plain.occurrences.push_back(occ(2, "u2", "A", 0, Direction::Input));
    eqs.push_back(new Equipotential(plain));
    Equipotential shared{ true, {}, {} };
    shared.occurrences.push_back(occ(2, "u2", "Q", 2, Direction::Output));
    shared.occurrences.push_back(occ(3, "u3", "A", 0, Direction::Input));
    shared.traceIds = { TraceStore::begin("a"), TraceStore::begin("b") };
    eqs.push_back(new Equipotential(shared));
    frame();
  }

  // The view as it's drawn on screen, as SVG.
  std::string onScreen() const {
    SvgPainter svg(ImVec2(1200.f, 800.f));
    SchematicView view = EquipotentialView::schematicForTesting();
    view.render(svg, ImVec2(0.f, 0.f), ImVec2(1200.f, 800.f));
    return svg.finish();
  }

  FakeNetlistProvider         provider;
  std::vector<Equipotential*> eqs;
};

}  // namespace

TEST_F(ThemeTest, NamesParse) {
  EXPECT_EQ(parseTheme("classic"), ThemeId::Classic);
  EXPECT_EQ(parseTheme("light"), ThemeId::Light);
  EXPECT_EQ(parseTheme("dark"), ThemeId::Dark);
  EXPECT_FALSE(parseTheme("Dark"));
  EXPECT_FALSE(parseTheme(""));
  for (int i = 0; i < kThemeCount; ++i) EXPECT_EQ(parseTheme(themeAt(i).name), themeAt(i).id);
}

TEST_F(ThemeTest, InitialFromEnvironment) {
  EXPECT_EQ(initialTheme(), ThemeId::Classic);
  setenv("NAJA_SCHEMATIC_THEME", "dark", 1);
  EXPECT_EQ(initialTheme(), ThemeId::Dark);
  setenv("NAJA_SCHEMATIC_THEME", "nope", 1);
  EXPECT_EQ(initialTheme(), ThemeId::Classic);
}

TEST_F(ThemeTest, PanelsFollowTheTheme) {
  setTheme(ThemeId::Light);
  EXPECT_EQ(theme().id, ThemeId::Light);
  EXPECT_GT(luminance(ImGui::GetStyle().Colors[ImGuiCol_WindowBg]), 0.7f);
  setTheme(ThemeId::Dark);
  EXPECT_LT(luminance(ImGui::GetStyle().Colors[ImGuiCol_WindowBg]), 0.3f);
}

// setUiScale() starts over from ImGui's default style: the theme's colors
// must survive it.
TEST_F(ThemeTest, UiScaleKeepsTheColors) {
  setTheme(ThemeId::Light);
  setUiScale(2.0f);
  EXPECT_GT(luminance(ImGui::GetStyle().Colors[ImGuiCol_WindowBg]), 0.7f);
  setUiScale(1.0f);
}

TEST_F(ThemeTest, PaperIsTheClassicAndLightSchematic) {
  EXPECT_EQ(themeById(ThemeId::Classic).schematic, paperPalette());
  EXPECT_EQ(themeById(ThemeId::Light).schematic, paperPalette());
  EXPECT_FALSE(themeById(ThemeId::Dark).schematic == paperPalette());
  EXPECT_EQ(SchematicView{}.colors, paperPalette());
}

// The baked colors are the paper ones, mapped to a palette's when drawn.
TEST_F(ThemeTest, InkMapsBakedColors) {
  const SchematicPalette& dark = themeById(ThemeId::Dark).schematic;
  EXPECT_EQ(NetWire{}.color, kPaperWireColor);
  EXPECT_EQ(TraceStore::convergenceColor(), kPaperConvergenceColor);
  EXPECT_EQ(dark.ink(kPaperWireColor), dark.wire);
  EXPECT_EQ(dark.ink(kPaperConvergenceColor), dark.convergence);
  EXPECT_EQ(dark.ink(IM_COL32(1, 2, 3, 255)), IM_COL32(1, 2, 3, 255));
  EXPECT_EQ(paperPalette().ink(kPaperWireColor), kPaperWireColor);
  // A trace color can't be either: TraceStore refuses near-black and
  // near-white, so it's never taken for the convergence color, or lost on
  // the canvas, in either palette.
  for (ImU32 c : { dark.canvas, dark.convergence, paperPalette().canvas, paperPalette().convergence })
    EXPECT_TRUE(TraceStore::tooCloseToConvergence(c) || TraceStore::tooCloseToCanvas(c)) << rgb(c);
}

TEST_F(ThemeTest, DarkSchematicOnScreen) {
  setTheme(ThemeId::Dark);
  scene();
  const SchematicPalette& dark = themeById(ThemeId::Dark).schematic;
  EXPECT_EQ(EquipotentialView::schematicForTesting().colors, dark);
  const std::string svg = onScreen();
  EXPECT_NE(svg.find(rgb(dark.canvas)), std::string::npos);
  EXPECT_NE(svg.find(rgb(dark.wire)), std::string::npos);
  EXPECT_NE(svg.find(rgb(dark.convergence)), std::string::npos);
  EXPECT_NE(svg.find(rgb(dark.instanceFill)), std::string::npos);
  EXPECT_EQ(svg.find(rgb(kPaperWireColor)), std::string::npos);
  EXPECT_EQ(svg.find(rgb(kPaperConvergenceColor)), std::string::npos);
  EXPECT_EQ(svg.find(rgb(paperPalette().instanceFill)), std::string::npos);
}

// Exports go into documents: on paper, whatever the theme, and the same as
// with the default theme.
TEST_F(ThemeTest, ExportsStayOnPaper) {
  scene();
  const std::string classic = EquipotentialView::exportSvg();
  ASSERT_FALSE(classic.empty());
  setTheme(ThemeId::Dark);
  frame();
  const std::string dark = EquipotentialView::exportSvg();
  EXPECT_EQ(dark, classic);
  const SchematicPalette& palette = themeById(ThemeId::Dark).schematic;
  EXPECT_EQ(dark.find(rgb(palette.canvas)), std::string::npos);
  EXPECT_NE(dark.find(rgb(kPaperWireColor)), std::string::npos);
  EXPECT_NE(dark.find(rgb(kPaperConvergenceColor)), std::string::npos);
}

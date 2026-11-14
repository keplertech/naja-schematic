// The schematic export's structure (SVG groups with data-* attributes) and
// decorations (title, caption, legend, hide_ui_glyphs), on the real
// EquipotentialView in a headless ImGui context -- and a golden file holding
// the drawing as it was before the groups existed: an export with no
// decoration must still draw exactly that, group tags aside.
#include "EquipotentialView.h"
#include "SchematicView.h"
#include "SelectionStore.h"
#include "StyleStore.h"
#include "TraceStore.h"
#include "ViewOptions.h"

#include <imgui.h>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <gtest/gtest.h>

#include "FakeNetlistProvider.h"

namespace {

InstTermOccurrence occ(std::vector<std::pair<unsigned, std::string>> path, const std::string& model,
                       const std::string& term, unsigned termId, Direction dir, size_t bitTermCount) {
  InstTermOccurrence o;
  for (const auto& [id, name] : path) {
    o.path.push_back({ id, name });
    o.pathIds.push_back(id);
    o.pathModels.push_back("M");
  }
  o.pathModels.back() = model;
  o.term              = BitTerm{ term, termId, dir, std::nullopt };
  o.bit_term_count    = bitTermCount;
  return o;
}

class SvgExport : public ::testing::Test {
 protected:
  void SetUp() override {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
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
    EquipotentialView::setProvider(nullptr);
    EquipotentialView::resetLayout();
    SelectionStore::clear();
    TraceStore::clear();
    StyleStore::clear();
    ViewOptions::current() = ViewOptions{};
    for (auto* eq : eqs) delete eq;
    ImGui::DestroyContext();
  }

  void add(Equipotential eq) { eqs.push_back(new Equipotential(std::move(eq))); }

  void frame(int n = 1) {
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

  // The scene: top-level input `en` into u1.A; u1.Q (traced, labeled) to
  // u2.A, u2 drawn with a partial interface, then expanded (open pins); an
  // escaped name holding '/' and a space; u1.Q also fans out to five flops,
  // gathered past max_sinks=4 (natural name order) into a summary.
  int scene() {
    const int trace = TraceStore::begin("u1/Q (net)");
    Equipotential in{ true, {}, {} };
    in.terms.push_back(BitTerm{ "en", 0, Direction::Input, std::nullopt });
    in.occurrences.push_back(occ({ { 1, "u1" } }, "AND2", "A", 10, Direction::Input, 3));
    add(in);
    Equipotential q{ true, {}, {} };
    q.occurrences.push_back(occ({ { 1, "u1" } }, "AND2", "Q", 12, Direction::Output, 3));
    q.occurrences.push_back(occ({ { 4, "top" }, { 2, "u2" } }, "BOX3", "A", 20, Direction::Input, 3));
    q.occurrences.push_back(occ({ { 3, "\\a/b c " } }, "BUF", "I", 30, Direction::Input, 2));
    for (unsigned i = 0; i < 5; ++i)
      q.occurrences.push_back(occ({ { 100 + i, "r" + std::to_string(i) } }, "DFF", "D", 40, Direction::Input, 3));
    q.traceIds = { trace };
    add(q);
    ViewOptions::current().maxSinks = 4;
    frame(3);
    return trace;
  }

  std::string svg() const { return EquipotentialView::exportSvg(); }
  const SchematicView& sv() const { return EquipotentialView::schematicForTesting(); }

  FakeNetlistProvider         provider;
  std::vector<Equipotential*> eqs;
};

// The export with every <g ...> and </g> line taken out: what was drawn.
std::string withoutGroups(const std::string& svg) {
  std::istringstream in(svg);
  std::string out, line;
  while (std::getline(in, line))
    if (line.rfind("<g ", 0) != 0 && line != "</g>") out += line + "\n";
  return out;
}

std::string readFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream s;
  s << f.rdbuf();
  return s.str();
}

// Every opening group tag, as its attributes.
std::vector<std::map<std::string, std::string>> groups(const std::string& svg, const std::string& cls) {
  std::vector<std::map<std::string, std::string>> out;
  std::regex tag("<g( [^>]*)>");
  std::regex attr(" ([a-z-]+)=\"([^\"]*)\"");
  for (auto it = std::sregex_iterator(svg.begin(), svg.end(), tag); it != std::sregex_iterator(); ++it) {
    std::map<std::string, std::string> attrs;
    const std::string body = (*it)[1];
    for (auto a = std::sregex_iterator(body.begin(), body.end(), attr); a != std::sregex_iterator(); ++a)
      attrs[(*a)[1]] = (*a)[2];
    if (attrs["class"] == cls) out.push_back(attrs);
  }
  return out;
}

const std::map<std::string, std::string>* find(const std::vector<std::map<std::string, std::string>>& gs,
                                               const std::string& key, const std::string& value) {
  for (const auto& g : gs)
    if (auto it = g.find(key); it != g.end() && it->second == value) return &g;
  return nullptr;
}

} // namespace

#ifdef NAJA_SCHEMATIC_TEST_DATA
// The drawing is the same, byte for byte, as before the groups existed.
// NAJA_SCHEMATIC_UPDATE_GOLDEN=1 rewrites the file (only for a deliberate
// drawing change).
TEST_F(SvgExport, DrawingWithoutDecorationsMatchesTheGolden) {
  scene();
  const std::string drawn = withoutGroups(svg());
  ASSERT_FALSE(drawn.empty());
  const std::string path = std::string(NAJA_SCHEMATIC_TEST_DATA) + "/export_golden.svg";
  if (std::getenv("NAJA_SCHEMATIC_UPDATE_GOLDEN")) {
    std::ofstream(path, std::ios::binary) << drawn;
    GTEST_SKIP() << "wrote " << path;
  }
  const std::string golden = readFile(path);
  ASSERT_FALSE(golden.empty()) << path;
  EXPECT_EQ(drawn, golden);
}
#endif

// The text between a group's opening tag (found by `marker`) and its </g>.
std::string groupBody(const std::string& svg, const std::string& marker) {
  const size_t at = svg.find(marker);
  if (at == std::string::npos) return "";
  const size_t from = svg.find('\n', at) + 1;
  return svg.substr(from, svg.find("</g>", from) - from);
}

TEST_F(SvgExport, EveryNetInstancePortAndSummaryIsAGroup) {
  scene();
  const std::string out = svg();
  // Balanced: every group closed.
  size_t opened = 0, closed = 0;
  for (size_t p = out.find("<g "); p != std::string::npos; p = out.find("<g ", p + 1)) ++opened;
  for (size_t p = out.find("</g>"); p != std::string::npos; p = out.find("</g>", p + 1)) ++closed;
  EXPECT_EQ(opened, closed);

  const auto nets = groups(out, "net");
  ASSERT_EQ(nets.size(), sv().routes.size());
  const auto* en = find(nets, "data-driver", "en");
  ASSERT_NE(en, nullptr);
  EXPECT_EQ(en->at("data-net"), "en");
  EXPECT_EQ(en->count("data-driver-id-path"), 0u);  // a top-level port
  const auto* q = find(nets, "data-driver", "u1:Q");
  ASSERT_NE(q, nullptr);
  EXPECT_EQ(q->at("data-driver-id-path"), "1");
  // Its wires and junction dots are inside it.
  const std::string qBody = groupBody(out, "data-driver=\"u1:Q\"");
  EXPECT_NE(qBody.find("<line"), std::string::npos);
  EXPECT_NE(qBody.find("<circle"), std::string::npos);

  const auto insts = groups(out, "instance");
  // u1, the escaped one, top/u2 (a group of one), r0..r2: the first sinks
  // in natural name order.
  EXPECT_EQ(insts.size(), 6u);
  const auto* u1 = find(insts, "data-path", "u1");
  ASSERT_NE(u1, nullptr);
  EXPECT_EQ(u1->at("data-id-path"), "1");
  EXPECT_EQ(u1->at("data-model"), "AND2");
  EXPECT_EQ(u1->count("data-path-json"), 0u);
  const std::string u1Body = groupBody(out, "data-path=\"u1\"");
  EXPECT_NE(u1Body.find(">u1</text>"), std::string::npos);  // its name
  EXPECT_NE(u1Body.find(">Q</text>"), std::string::npos);   // its pins
  const auto* u2 = find(insts, "data-id-path", "4,2");
  ASSERT_NE(u2, nullptr);
  EXPECT_EQ(u2->at("data-path"), "top/u2");
  // An escaped name with '/' and a space: the text form is ambiguous, so
  // the path is also JSON (XML-escaped in the file).
  const auto* esc = find(insts, "data-id-path", "3");
  ASSERT_NE(esc, nullptr);
  EXPECT_EQ(esc->at("data-path"), "\\a/b c ");
  EXPECT_EQ(esc->at("data-path-json"), "[&quot;\\\\a/b c &quot;]");

  const auto ports = groups(out, "port");
  ASSERT_EQ(ports.size(), 1u);
  EXPECT_EQ(ports[0].at("data-name"), "en");
  EXPECT_EQ(ports[0].at("data-direction"), "input");
  EXPECT_NE(groupBody(out, "class=\"port\"").find(">en</text>"), std::string::npos);

  const auto sums = groups(out, "summary");
  ASSERT_EQ(sums.size(), 1u);
  EXPECT_EQ(sums[0].at("data-group"), "DFF");
  EXPECT_EQ(sums[0].at("data-count"), "2");  // r3, r4

  const auto mods = groups(out, "module");
  ASSERT_EQ(mods.size(), 1u);
  EXPECT_EQ(mods[0].at("data-path"), "top");
}

TEST_F(SvgExport, AStyleLabelIsOnItsNetGroup) {
  scene();
  std::vector<std::string> errors;
  StyleStore::setStyles(StyleStore::parse(nlohmann::json::parse(R"({"items": [
      {"kind": "net", "id_path": [1], "terminal": "Q", "color": "#1f77b4", "label": "copy 0"}]})"), &errors));
  ASSERT_TRUE(errors.empty()) << errors[0];
  frame(2);
  const auto nets = groups(svg(), "net");
  const auto* q = find(nets, "data-driver", "u1:Q");
  ASSERT_NE(q, nullptr);
  EXPECT_EQ(q->at("data-label"), "copy 0");
}

TEST_F(SvgExport, TitleCaptionAndLegendFrameTheDrawing) {
  const int trace = scene();
  const std::string plain = svg();
  ViewOptions::current().title   = "Fan-out of u1/Q";
  ViewOptions::current().caption = "line one\nline <two>";
  ViewOptions::current().legend  = true;
  const std::string out = svg();
  // The same drawing, moved down by the title band.
  EXPECT_NE(out.find("<g class=\"title\">"), std::string::npos);
  EXPECT_NE(groupBody(out, "class=\"title\"").find(">Fan-out of u1/Q</text>"), std::string::npos);
  const std::string caption = groupBody(out, "class=\"caption\"");
  EXPECT_NE(caption.find(">line one</text>"), std::string::npos);
  EXPECT_NE(caption.find(">line &lt;two&gt;</text>"), std::string::npos);
  // One legend entry: the trace, in its color.
  const auto entries = groups(out, "legend-entry");
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].at("data-label"), "u1/Q (net)");
  const ImU32 c = TraceStore::color(trace);
  char rgb[48];
  std::snprintf(rgb, sizeof(rgb), "stroke=\"rgb(%u,%u,%u)\"", c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF);
  EXPECT_NE(groupBody(out, "class=\"legend-entry\"").find(rgb), std::string::npos) << rgb;
  // A taller page; the groups of the drawing are all still there.
  auto height = [](const std::string& s) {
    const size_t at = s.find(" height=\"") + 9;
    return std::stof(s.substr(at, s.find('"', at) - at));
  };
  EXPECT_GT(height(out), height(plain) + 30.f);
  EXPECT_EQ(groups(out, "net").size(), groups(plain, "net").size());
  EXPECT_EQ(groups(out, "instance").size(), groups(plain, "instance").size());

  // A hidden trace isn't in the legend; legend with nothing to list adds nothing.
  ViewOptions::current().title.clear();
  ViewOptions::current().caption.clear();
  TraceStore::setVisible(trace, false);
  frame(2);
  EXPECT_TRUE(groups(svg(), "legend-entry").empty());
  EXPECT_EQ(withoutGroups(svg()).find("class=\"legend\""), std::string::npos);
}

TEST_F(SvgExport, DecorationsNeverReachTheCanvasLayout) {
  scene();
  const auto before = sv().transform.scale;
  ViewOptions::current().title = "t";
  frame(2);
  EXPECT_EQ(sv().transform.scale, before);
}

TEST_F(SvgExport, HideUiGlyphsDropsMorePinsDotsAndOpenStubs) {
  scene();
  EquipotentialView::applyInstanceExpansion(InstancePath{ { 4, "top" }, { 2, "u2" } }, {
      { "A", Direction::Input, 20, std::nullopt },
      { "B", Direction::Input, 21, std::nullopt },
      { "Y", Direction::Output, 22, std::nullopt },
  });
  frame(2);
  auto circles = [](const std::string& s) {
    size_t n = 0;
    for (size_t p = s.find("<circle"); p != std::string::npos; p = s.find("<circle", p + 1)) ++n;
    return n;
  };
  const std::string u2 = "data-id-path=\"4,2\"";
  const std::string shown = groupBody(svg(), u2);
  ViewOptions::current().hideUiGlyphs = true;
  const std::string hidden = groupBody(svg(), u2);
  // B and Y: two circles each (white fill, outline).
  EXPECT_EQ(circles(shown) - circles(hidden), 4u);
  EXPECT_EQ(circles(hidden), 0u);
  // The canvas still shows them.
  frame();
  EXPECT_FALSE(sv().hideUiGlyphs);
}

TEST_F(SvgExport, HideUiGlyphsDropsThePartialBoxDots) {
  scene();  // u2: one pin of three shown, "..." under it
  const std::string u2 = "data-id-path=\"4,2\"";
  auto circles = [](const std::string& s) {
    size_t n = 0;
    for (size_t p = s.find("<circle"); p != std::string::npos; p = s.find("<circle", p + 1)) ++n;
    return n;
  };
  const std::string with = svg();
  EXPECT_EQ(circles(groupBody(with, u2)), 3u);
  ViewOptions::current().hideUiGlyphs = true;
  const std::string without = svg();
  EXPECT_EQ(circles(groupBody(without, u2)), 0u);
  // The junction dots stay.
  const std::string q = "data-driver=\"u1:Q\"";
  EXPECT_GT(circles(groupBody(without, q)), 0u);
  EXPECT_EQ(circles(groupBody(without, q)), circles(groupBody(with, q)));
}

// The PNG draws the same page: decorations make it larger.
TEST_F(SvgExport, PngHasTheSameDecorations) {
  scene();
  auto dims = [](const std::string& png) {
    EXPECT_GE(png.size(), 24u);
    auto be32 = [&](size_t at) {
      return (unsigned(uint8_t(png[at])) << 24) | (unsigned(uint8_t(png[at + 1])) << 16) |
             (unsigned(uint8_t(png[at + 2])) << 8) | unsigned(uint8_t(png[at + 3]));
    };
    return std::pair{ be32(16), be32(20) };  // IHDR width, height
  };
  auto svgDims = [](const std::string& s) {
    auto attr = [&](const char* key) {
      const size_t at = s.find(key) + std::strlen(key);
      return unsigned(std::lround(std::stof(s.substr(at, s.find('"', at) - at))));
    };
    return std::pair{ attr(" width=\""), attr(" height=\"") };
  };
  const auto plain = dims(EquipotentialView::exportPng(1.0f));
  EXPECT_EQ(plain, svgDims(svg()));
  ViewOptions::current().title   = "A title";
  ViewOptions::current().caption = "A caption";
  ViewOptions::current().legend  = true;
  const auto decorated = dims(EquipotentialView::exportPng(1.0f));
  EXPECT_EQ(decorated, svgDims(svg()));
  EXPECT_GT(decorated.second, plain.second);
}

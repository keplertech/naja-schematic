#include "AppLogic.h"
#include "Theme.h"
#include "UiScale.h"

#include <algorithm>
#include <iostream>
#include <sstream>

#include <imgui.h>
#include <backends/imgui_impl_sdl2.h>
#include <backends/imgui_impl_opengl3.h>
#include <SDL.h>
#include <SDL_opengl.h>

#include <nlohmann/json.hpp>
using json = nlohmann::json;

#include "INetlistProvider.h"
#include "NetlistTree.h"
#include "GUIData.h"
#include "Types.h"
#include "Console.h"
#include "EquipotentialView.h"
#include "PngWriter.h"
#include "ViewOptions.h"
#include "DiagnosisStore.h"
#include "DiagnosisView.h"
#include "SourceStore.h"
#include "SourceView.h"
#include "PropertiesStore.h"
#include "SelectionStore.h"
#include "TraceStore.h"
#include "ShowNetStore.h"
#include "TraceView.h"
#include "PropertiesView.h"
#include "LogView.h"
#include "ReviewStore.h"
#include "ReviewView.h"

#ifndef __EMSCRIPTEN__
#include <fstream>
#include "LocalSNLProvider.h"
#include "DesignLoader.h"
#include "NativeFileDialog.h"
#else
#include <emscripten.h>

// Hands `text` to the browser as a file download.
EM_JS(void, najaDownloadText, (const char* name, const char* mime, const char* text), {
  const blob = new Blob([UTF8ToString(text)], { type: UTF8ToString(mime) });
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = UTF8ToString(name);
  document.body.appendChild(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
});
#endif

// File > Export Schematic as SVG: the schematic as drawn, whole sheet.
static void exportSchematicSvg() {
  const std::string svg = EquipotentialView::exportSvg();
  if (svg.empty()) return;
#ifdef __EMSCRIPTEN__
  najaDownloadText("schematic.svg", "image/svg+xml", svg.c_str());
  Console::Log("Schematic exported as schematic.svg");
#else
  const std::string path = NativeFileDialog::saveFile("Export Schematic as SVG", "schematic.svg", "svg");
  if (path.empty()) return;
  std::ofstream out(path, std::ios::binary);
  out << svg;
  if (out) Console::Log("Schematic exported to " + path);
  else     Console::Error("Could not write " + path);
#endif
}

// ImGuiMod_Ctrl is Cmd on macOS (ImGui swaps them), so name it that way there.
#if defined(__APPLE__) && !defined(__EMSCRIPTEN__)
#define NAJA_SHORTCUT(key) "Cmd+" key
#else
#define NAJA_SHORTCUT(key) "Ctrl+" key
#endif

// The View menu's shortcuts. Global, but not while a text field has focus.
static void handleShortcuts(AppState& state) {
  if (ImGui::GetIO().WantTextInput) return;
  constexpr ImGuiInputFlags kRepeat = ImGuiInputFlags_RouteGlobal | ImGuiInputFlags_Repeat;
  constexpr ImGuiInputFlags kOnce   = ImGuiInputFlags_RouteGlobal;
  // "+" is Shift+= on most layouts: accept both.
  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Equal, kRepeat) ||
      ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Equal, kRepeat) ||
      ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_KeypadAdd, kRepeat))
    EquipotentialView::zoomIn();
  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Minus, kRepeat) ||
      ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_KeypadSubtract, kRepeat))
    EquipotentialView::zoomOut();
  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_0, kOnce) ||
      ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Keypad0, kOnce))
    EquipotentialView::fitView();
  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_K, kOnce)) {
    ShowNetStore::forget();
    state.guiData->clearEquipotentials();
  }
}

// Help > Keyboard Shortcuts. Keep in step with handleShortcuts() above,
// SchematicView::handleInteraction(), EquipotentialView's canvas clicks and
// the README's "Controls" section.
static void renderShortcutsWindow(bool& open) {
  if (!open) return;
  struct Row { const char* keys; const char* action; };
  static const Row kAnywhere[] = {
    {NAJA_SHORTCUT("+") "  /  " NAJA_SHORTCUT("-"), "Zoom the schematic in / out"},
    {NAJA_SHORTCUT("0"),                             "Fit the schematic to the canvas"},
    {NAJA_SHORTCUT("K"),                             "Clear all nets from the schematic"},
  };
  static const Row kCanvasKeys[] = {
    {"W  A  S  D",         "Pan up / left / down / right"},
    {"=  /  -  (hold)",    "Zoom in / out about the canvas center"},
    {"0  or  Home",        "Fit the schematic to the canvas"},
  };
  static const Row kSource[] = {
    {NAJA_SHORTCUT("F"),           "Find in the shown file"},
    {NAJA_SHORTCUT("G"),           "Find the next match"},
    {NAJA_SHORTCUT("C"),           "Copy the selected text"},
    {NAJA_SHORTCUT("A"),           "Select the whole file"},
  };
  static const Row kMouse[] = {
    {"Wheel",                        "Zoom about the cursor"},
    {"Right or middle drag",         "Pan"},
    {"Click an open pin or port",    "Add its net to the schematic"},
    {"Click a bus pin or port",      "Add the nets of all its bits (it stays one pin)"},
    {"Double-click a bus pin or port", "Show its bits; on a bit, merge them back"},
    {"Double-click a dashed box",    "Show all of its pins"},
    {"Click a box",                  "Select the instance (also in the tree, its pins and nets below it)"},
    {"Click a stacked (summary) box","Show the sinks it gathers (View > Fanout)"},
    {"Click a wire",                 "Select (highlight) its net"},
    {"Click a box's + glyph",        "Open the instance: its contents, laid out, in its frame"},
    {"Click a frame's - glyph",      "Close the opened instance: its box again"},
    {"Click a review badge",         "Open its thread in the Review tab"},
    {"Right-click",                  "Context menu: trace, sources, properties, comment, open/close, fit, clear"},
    {"Right-click a tree, pin or net row", "Its menu; Add Comment... starts a review thread (with --review)"},
    {"Tree's Show menu (top right)", "Check Pins / Nets to list them in the tree; uncheck Primitives for modules only"},
  };
  auto table = [](const char* id, const Row* rows, size_t n) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) return;
    for (size_t i = 0; i < n; ++i) {
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::TextUnformatted(rows[i].keys);
      ImGui::TableSetColumnIndex(1);
      ImGui::TextUnformatted(rows[i].action);
    }
    ImGui::EndTable();
  };
  ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 31, 0), ImGuiCond_FirstUseEver);
  if (ImGui::Begin("Keyboard Shortcuts", &open, ImGuiWindowFlags_NoSavedSettings)) {
    ImGui::SeparatorText("Anywhere");
    table("##anywhere", kAnywhere, IM_COUNTOF(kAnywhere));
    ImGui::SeparatorText("Mouse over the schematic");
    table("##canvaskeys", kCanvasKeys, IM_COUNTOF(kCanvasKeys));
    ImGui::SeparatorText("Mouse");
    table("##mouse", kMouse, IM_COUNTOF(kMouse));
    ImGui::SeparatorText("Source tab (text clicked first)");
    table("##source", kSource, IM_COUNTOF(kSource));
  }
  ImGui::End();
}


// ---------------------------------------------------------------------------
// Per-frame rendering — identical for both WASM and native entry points.
// ---------------------------------------------------------------------------

#ifndef __EMSCRIPTEN__
namespace {
DesignLoader g_designLoader;
std::function<void()> g_afterDesignLoad;
}  // namespace

void startDesignLoad(AppState& state, std::string title,
                     std::function<bool(LocalSNLProvider&)> load,
                     std::function<void()> then) {
  auto* provider = dynamic_cast<LocalSNLProvider*>(state.provider);
  if (!provider) return;
  if (!g_designLoader.start(*provider, std::move(title), std::move(load))) {
    Console::Warn("A design is already loading");
    return;
  }
  g_afterDesignLoad = std::move(then);
}

bool designLoading() { return g_designLoader.active(); }

// The load started by startDesignLoad(), once it's over: the new design
// shown, or the Log tab saying why not.
static void finishDesignLoad(AppState& state) {
  bool ok = false;
  if (!g_designLoader.takeResult(ok)) return;
  auto then = std::move(g_afterDesignLoad);
  g_afterDesignLoad = nullptr;
  if (!ok) {
    state.focusLogTab = true;
    // The design shown is kept, but a request the tree made meanwhile was
    // dropped (setLoading()) and will never be answered: start it over.
    auto* provider = dynamic_cast<LocalSNLProvider*>(state.provider);
    if (provider && provider->takeDroppedRequests() > 0) reloadNetlist(state);
    return;
  }
  char took[64];
  std::snprintf(took, sizeof(took), ": done in %.1f s", g_designLoader.elapsedSeconds());
  Console::Log(g_designLoader.title() + took);
  reloadNetlist(state);
  if (then) then();
}

// The modal shown while a design loads: naja's stage and what it's on, a
// bar (moving back and forth when naja can't tell how much is left), the
// time spent, and Cancel.
static void renderDesignLoadDialog() {
  const char* kTitle = "Loading design";
  const bool active = g_designLoader.active();
  if (!active && !ImGui::IsPopupOpen(kTitle)) return;
  if (active && !ImGui::IsPopupOpen(kTitle)) ImGui::OpenPopup(kTitle);
  const float em = ImGui::GetFontSize();
  ImGui::SetNextWindowSize(ImVec2(em * 28, 0), ImGuiCond_Always);
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
    return;
  if (!active) {   // over (finishDesignLoad(), earlier this frame)
    ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return;
  }
  const DesignLoader::Progress p = g_designLoader.progress();
  ImGui::TextUnformatted(g_designLoader.title().c_str());
  ImGui::Spacing();
  ImGui::Text("%s", DesignLoader::stageLabel(p.stage).c_str());
  // The file or design being read; long paths keep their end.
  std::string detail = p.detail;
  const float room = ImGui::GetContentRegionAvail().x;
  for (size_t cut = 1; cut < p.detail.size() && ImGui::CalcTextSize(detail.c_str()).x > room; ++cut)
    detail = "..." + p.detail.substr(cut);
  ImGui::TextDisabled("%s", detail.empty() ? " " : detail.c_str());
  if (p.total > 0) {
    const float fraction = float(double(std::min(p.current, p.total)) / double(p.total));
    const std::string count = std::to_string(p.current) + " / " + std::to_string(p.total);
    ImGui::ProgressBar(fraction, ImVec2(-1, 0), count.c_str());
  } else {
    ImGui::ProgressBar(-1.0f * float(ImGui::GetTime()), ImVec2(-1, 0), "");
  }
  ImGui::Text("%.1f s", g_designLoader.elapsedSeconds());
  ImGui::Spacing();
  if (g_designLoader.cancelling()) {
    ImGui::BeginDisabled();
    ImGui::Button("Cancelling...", ImVec2(em * 7, 0));
    ImGui::EndDisabled();
  } else if (ImGui::Button("Cancel", ImVec2(em * 7, 0))) {
    g_designLoader.cancel();
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Stop loading; the design shown before stays");
  ImGui::EndPopup();
}
#endif

bool appFrame(AppState& state) {
  SDL_Event event;
  bool quit = false;
  while (SDL_PollEvent(&event)) {
    ImGui_ImplSDL2_ProcessEvent(&event);
    if (event.type == SDL_QUIT) {
      quit = true;
    }
  }

  // View > Text Size, last frame: the style changes between frames.
  static float requestedUiScale = 0.0f;
  if (requestedUiScale > 0.0f) { setUiScale(requestedUiScale); requestedUiScale = 0.0f; }
  // View > Theme, likewise.
  static std::optional<ThemeId> requestedTheme;
  if (requestedTheme) { setTheme(*requestedTheme); requestedTheme.reset(); }

  ImGuiIO& io = ImGui::GetIO();
  ImGui_ImplOpenGL3_NewFrame();
  ImGui_ImplSDL2_NewFrame();
  if (state.hooks.beforeNewFrame) state.hooks.beforeNewFrame();
  ImGui::NewFrame();
  if (state.hooks.afterNewFrame) state.hooks.afterNewFrame();

#ifndef __EMSCRIPTEN__
  finishDesignLoad(state);
#endif

  // A review thread clicked last frame: its object in the tree (down to a
  // term/net row) and in the schematic (added if not shown). Net threads
  // have no schematic object (a wire is an equipotential, not a net).
  if (auto go = ReviewView::takeGoTo()) {
    const ReviewAnchor& a = go->anchor;
    std::optional<NetlistTree::RevealLeaf> leaf;
    if (a.kind == ReviewAnchorKind::Term && a.termId)
      leaf = NetlistTree::RevealLeaf{NetlistTree::RevealLeaf::Kind::Term, *a.termId, a.bit};
    else if (a.kind == ReviewAnchorKind::Net && a.netId)
      leaf = NetlistTree::RevealLeaf{NetlistTree::RevealLeaf::Kind::Net, *a.netId, a.bit};
    if (state.guiData->netlist_) state.guiData->netlist_->reveal(go->path, leaf);
    if (a.kind == ReviewAnchorKind::Instance)
      EquipotentialView::focusObject({go->path, std::nullopt, std::nullopt});
    else if (a.kind == ReviewAnchorKind::Term && a.termId)
      EquipotentialView::focusObject({go->path, a.termId, a.bit});
  }

  // ==== Top Menu Bar ====
  static bool aboutOpen = false;
  static bool shortcutsOpen = false;
#ifndef __EMSCRIPTEN__
  static bool snlDialogOpen = false;
  static bool vrlDialogOpen = false;
  static bool svDialogOpen  = false;
  static char snlPathBuf[1024]    = {};
  static char vrlFilesBuf[8192]   = {};
  static char vrlLibertyBuf[4096] = {};
  static bool vrlYosysSrc         = false;
  static char svFilesBuf[8192]    = {};
  static char svTopBuf[256]       = {};
  static bool vhdlDialogOpen      = false;
  static char vhdlFilesBuf[8192]  = {};
  static char vhdlTopBuf[256]     = {};
#endif

  handleShortcuts(state);

  if (ImGui::BeginMainMenuBar()) {
    if (ImGui::BeginMenu("File")) {
#ifndef __EMSCRIPTEN__
      const bool canOpen = !designLoading();
      if (ImGui::MenuItem("Open naja-if...",        "", false, canOpen)) { snlDialogOpen = true; snlPathBuf[0] = '\0'; }
      if (ImGui::MenuItem("Open Verilog...",        "", false, canOpen)) { vrlDialogOpen = true; vrlFilesBuf[0] = '\0'; vrlLibertyBuf[0] = '\0'; }
      if (ImGui::MenuItem("Open SystemVerilog...",  "", false, canOpen)) { svDialogOpen  = true; svFilesBuf[0]  = '\0'; svTopBuf[0] = '\0'; }
      if (ImGui::MenuItem("Open VHDL...",           "", false, canOpen)) { vhdlDialogOpen = true; vhdlFilesBuf[0] = '\0'; vhdlTopBuf[0] = '\0'; }
      ImGui::Separator();
      // Diagnosis UI temporarily hidden — see DiagnosisStore.cpp kDiagnosisUIHidden.
      // if (ImGui::MenuItem("Load Diagnosis JSON...", "")) {
      //   auto files = NativeFileDialog::pickFiles("Select Diagnosis JSON", {"json"});
      //   if (!files.empty()) {
      //     loadDiagnosisFile(files[0]);
      //   }
      // }
      // ImGui::Separator();
#endif
      if (ImGui::MenuItem("Export Schematic as SVG...", "", false,
                          EquipotentialView::hasSchematic()))
        exportSchematicSvg();
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
      if (ImGui::MenuItem("Zoom In",    NAJA_SHORTCUT("+"))) EquipotentialView::zoomIn();
      if (ImGui::MenuItem("Zoom Out",   NAJA_SHORTCUT("-"))) EquipotentialView::zoomOut();
      if (ImGui::MenuItem("Fit",        NAJA_SHORTCUT("0"))) EquipotentialView::fitView();
      if (ImGui::MenuItem("Show Hierarchy", "", EquipotentialView::showHierarchy()))
        EquipotentialView::setShowHierarchy(!EquipotentialView::showHierarchy());
      // The size of the app's text and spacing, not the schematic's (its
      // labels follow its zoom): setUiScale(), also --ui-scale and
      // $NAJA_SCHEMATIC_UI_SCALE.
      if (ImGui::BeginMenu("Text Size")) {
        const float scale = uiScale();
        if (ImGui::MenuItem("Larger", "", false, scale < kMaxUiScale)) requestedUiScale = scale * 1.25f;
        if (ImGui::MenuItem("Smaller", "", false, scale > kMinUiScale)) requestedUiScale = scale / 1.25f;
        if (ImGui::MenuItem("Reset", "", false, scale != 1.0f)) requestedUiScale = 1.0f;
        float percent = scale * 100.0f;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7);
        // Applied on release: rescaling while dragging moves the slider
        // under the mouse.
        ImGui::SliderFloat("##uiscale", &percent, kMinUiScale * 100.0f, kMaxUiScale * 100.0f, "%.0f%%",
                           ImGuiSliderFlags_AlwaysClamp);
        if (ImGui::IsItemDeactivatedAfterEdit()) requestedUiScale = percent / 100.0f;
        ImGui::EndMenu();
      }
      // The app's colors, panels and schematic together: setTheme(), also
      // --theme and $NAJA_SCHEMATIC_THEME. Exports stay on paper.
      if (ImGui::BeginMenu("Theme")) {
        for (int i = 0; i < kThemeCount; ++i) {
          const Theme& t = themeAt(i);
          if (ImGui::MenuItem(t.label, "", theme().id == t.id)) requestedTheme = t.id;
        }
        ImGui::EndMenu();
      }
      // How the schematic is drawn (ViewOptions; also the host's
      // view_options push and the native --instance-label/--sink-rows/
      // --max-sinks/--group-by/--no-convergence-outline flags).
      ViewOptions& vo = ViewOptions::current();
      if (ImGui::BeginMenu("Instance Labels")) {
        for (InstanceLabel l : { InstanceLabel::Name, InstanceLabel::Model, InstanceLabel::Both }) {
          const char* text = l == InstanceLabel::Name ? "Name" : l == InstanceLabel::Model ? "Cell Model" : "Name and Cell Model";
          if (ImGui::MenuItem(text, "", vo.instanceLabel == l)) vo.instanceLabel = l;
        }
        ImGui::EndMenu();
      }
      if (ImGui::BeginMenu("Fanout")) {
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7);
        ImGui::DragInt("Rows per sink column", &vo.sinkRows, 0.2f, 0, 1000, vo.sinkRows ? "%d" : "no limit");
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7);
        ImGui::DragInt("Sinks drawn per net", &vo.maxSinks, 0.2f, 0, 100000, vo.maxSinks ? "%d" : "all");
        ImGui::SeparatorText("Gather the others by");
        for (FanoutGroupBy g : { FanoutGroupBy::Model, FanoutGroupBy::Hierarchy, FanoutGroupBy::Role, FanoutGroupBy::Pin }) {
          const char* text = g == FanoutGroupBy::Model     ? "Cell Model"
                           : g == FanoutGroupBy::Hierarchy ? "Module"
                           : g == FanoutGroupBy::Role      ? "Pin Role (clock / reset / enable / data)"
                                                           : "Pin Name";
          if (ImGui::MenuItem(text, "", vo.groupBy == g)) vo.groupBy = g;
        }
        ImGui::EndMenu();
      }
      if (ImGui::MenuItem("Outline Trace Convergence", "", vo.convergenceOutline))
        vo.convergenceOutline = !vo.convergenceOutline;
      ImGui::Separator();
      if (ImGui::MenuItem("Clear nets", NAJA_SHORTCUT("K"))) {
        ShowNetStore::forget();
        state.guiData->clearEquipotentials();
      }
      // Diagnosis UI temporarily hidden — see DiagnosisStore.cpp kDiagnosisUIHidden.
      // if (ImGui::MenuItem("Clear diagnosis", ""))  DiagnosisStore::clear();
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
      if (ImGui::MenuItem("Keyboard Shortcuts")) shortcutsOpen = true;
      ImGui::Separator();
      if (ImGui::MenuItem("About")) aboutOpen = true;
      ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
  }

#ifndef __EMSCRIPTEN__
  // Dialog sizes are in font units, so the dialogs grow with the text (UI
  // scale, or a bigger kBaseFontSizePx) instead of clipping their buttons,
  // and never get wider than the window.
  auto dialogWidth = [&io](float ems) {
    const float em = ImGui::GetFontSize();
    return std::max(em * 10, std::min(em * ems, io.DisplaySize.x - 2 * em));
  };
  auto linesHeight = [](int lines) {
    return ImGui::GetTextLineHeight() * lines + ImGui::GetStyle().FramePadding.y * 2;
  };
  auto buttonWidth = [] { return ImGui::GetFontSize() * 7; };
  // Put the small button labelled `label` on the current line if it fits
  // there, else on the next one: a narrow window never pushes it out of reach.
  auto sameLineIfFits = [](const char* label) {
    const ImGuiStyle& st = ImGui::GetStyle();
    const float w = ImGui::CalcTextSize(label, nullptr, true).x + st.FramePadding.x * 2;
    const float right = ImGui::GetItemRectMax().x + st.ItemSpacing.x + w;
    if (right <= ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - st.WindowPadding.x) ImGui::SameLine();
  };

  // Helper: split a text buffer into non-empty trimmed lines
  auto splitLines = [](const char* buf) {
    std::vector<std::string> out;
    std::istringstream ss(buf);
    std::string line;
    while (std::getline(ss, line)) {
      auto s = line.find_first_not_of(" \t\r");
      if (s == std::string::npos) continue;
      auto e = line.find_last_not_of(" \t\r");
      out.push_back(line.substr(s, e - s + 1));
    }
    return out;
  };

  // Helper: append paths to a char buffer (newline-separated)
  auto appendPaths = [](char* buf, size_t bufSize,
                        const std::vector<std::string>& paths) {
    for (const auto& p : paths) {
      size_t cur = strlen(buf);
      if (cur > 0 && buf[cur - 1] != '\n' && cur + 1 < bufSize)
        buf[cur++] = '\n';
      size_t room = bufSize - cur - 1;
      strncat(buf + cur, p.c_str(), room);
    }
  };

  // ==== Open naja-if dialog ====
  if (snlDialogOpen) { ImGui::OpenPopup("Open naja-if"); snlDialogOpen = false; }
  ImGui::SetNextWindowSize(ImVec2(dialogWidth(32), 0), ImGuiCond_Always);
  if (ImGui::BeginPopupModal("Open naja-if", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::Text("naja-if directory path:");
    sameLineIfFits("Browse...##snl");
    if (ImGui::SmallButton("Browse...##snl")) {
      auto dir = NativeFileDialog::pickDirectory("Select naja-if Directory");
      if (!dir.empty()) {
        strncpy(snlPathBuf, dir.c_str(), sizeof(snlPathBuf) - 1);
        snlPathBuf[sizeof(snlPathBuf) - 1] = '\0';
      }
    }
    ImGui::SetNextItemWidth(-1);
    bool ok = ImGui::InputText("##snlpath", snlPathBuf, sizeof(snlPathBuf),
                               ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::Spacing();
    if ((ImGui::Button("Open", ImVec2(buttonWidth(),0)) || ok) && snlPathBuf[0]) {
      startDesignLoad(state, "Loading naja-if",
                      [path = std::string(snlPathBuf)](LocalSNLProvider& p) { return p.loadSNL(path); });
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(buttonWidth(),0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }

  // ==== Open Verilog dialog ====
  if (vrlDialogOpen) { ImGui::OpenPopup("Open Verilog"); vrlDialogOpen = false; }
  ImGui::SetNextWindowSize(ImVec2(dialogWidth(33), 0), ImGuiCond_Always);
  if (ImGui::BeginPopupModal("Open Verilog", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::Text("Verilog source files (one path per line):");
    sameLineIfFits("Browse...##vrlsrc");
    if (ImGui::SmallButton("Browse...##vrlsrc")) {
      auto files = NativeFileDialog::pickFiles("Select Verilog Files", {"v"});
      appendPaths(vrlFilesBuf, sizeof(vrlFilesBuf), files);
    }
    ImGui::InputTextMultiline("##vrlfiles", vrlFilesBuf, sizeof(vrlFilesBuf), ImVec2(-1, linesHeight(6)));
    ImGui::Spacing();
    ImGui::Text("Liberty files (optional, one path per line):");
    sameLineIfFits("Browse...##vrlliberty");
    if (ImGui::SmallButton("Browse...##vrlliberty")) {
      auto files = NativeFileDialog::pickFiles("Select Liberty Files", {"lib"});
      appendPaths(vrlLibertyBuf, sizeof(vrlLibertyBuf), files);
    }
    ImGui::InputTextMultiline("##vrlliberty", vrlLibertyBuf, sizeof(vrlLibertyBuf), ImVec2(-1, linesHeight(4)));
    ImGui::Spacing();
    ImGui::Checkbox("Decode Yosys src attributes", &vrlYosysSrc);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("For a Yosys-synthesized netlist: Show Sources then also\n"
                        "lists the RTL each gate came from");
    ImGui::Spacing();
    if (ImGui::Button("Open", ImVec2(buttonWidth(),0)) && vrlFilesBuf[0]) {
      startDesignLoad(state, "Loading Verilog",
                      [files = splitLines(vrlFilesBuf), liberty = splitLines(vrlLibertyBuf),
                       yosysSrc = vrlYosysSrc](LocalSNLProvider& p) {
                        return p.loadVerilog(files, liberty, yosysSrc);
                      });
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(buttonWidth(),0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }

  // ==== Open SystemVerilog dialog ====
  if (svDialogOpen) { ImGui::OpenPopup("Open SystemVerilog"); svDialogOpen = false; }
  ImGui::SetNextWindowSize(ImVec2(dialogWidth(33), 0), ImGuiCond_Always);
  if (ImGui::BeginPopupModal("Open SystemVerilog", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::Text("SV/V files or Flist path (one per line):");
    sameLineIfFits("Browse SV...");
    if (ImGui::SmallButton("Browse SV...")) {
      auto files = NativeFileDialog::pickFiles("Select SystemVerilog Files", {"sv", "v"});
      appendPaths(svFilesBuf, sizeof(svFilesBuf), files);
    }
    sameLineIfFits("Browse Flist...");
    if (ImGui::SmallButton("Browse Flist...")) {
      auto files = NativeFileDialog::pickFiles("Select Flist");
      appendPaths(svFilesBuf, sizeof(svFilesBuf), files);
    }
    ImGui::InputTextMultiline("##svfiles", svFilesBuf, sizeof(svFilesBuf), ImVec2(-1, linesHeight(7)));
    ImGui::Spacing();
    ImGui::Text("Top module (optional -- overrides auto-detection):");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##svtop", svTopBuf, sizeof(svTopBuf));
    ImGui::Spacing();
    if (ImGui::Button("Open", ImVec2(buttonWidth(),0)) && svFilesBuf[0]) {
      startDesignLoad(state, "Loading SystemVerilog",
                      [files = splitLines(svFilesBuf), top = std::string(svTopBuf)](LocalSNLProvider& p) {
                        return p.loadSystemVerilog(files, top);
                      });
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(buttonWidth(),0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }

  // ==== Open VHDL dialog ====
  if (vhdlDialogOpen) { ImGui::OpenPopup("Open VHDL"); vhdlDialogOpen = false; }
  ImGui::SetNextWindowSize(ImVec2(dialogWidth(33), 0), ImGuiCond_Always);
  if (ImGui::BeginPopupModal("Open VHDL", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::Text("VHDL files, in any order (one per line):");
    sameLineIfFits("Browse...");
    if (ImGui::SmallButton("Browse...")) {
      auto files = NativeFileDialog::pickFiles("Select VHDL Files", {"vhd", "vhdl"});
      appendPaths(vhdlFilesBuf, sizeof(vhdlFilesBuf), files);
    }
    ImGui::InputTextMultiline("##vhdlfiles", vhdlFilesBuf, sizeof(vhdlFilesBuf), ImVec2(-1, linesHeight(7)));
    ImGui::Spacing();
    ImGui::Text("Top entity (optional -- else the unique uninstantiated one):");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##vhdltop", vhdlTopBuf, sizeof(vhdlTopBuf));
    ImGui::TextDisabled("naja's VHDL frontend is experimental (bit/bit_vector subset).");
    ImGui::Spacing();
    if (ImGui::Button("Open", ImVec2(buttonWidth(),0)) && vhdlFilesBuf[0]) {
      startDesignLoad(state, "Loading VHDL",
                      [files = splitLines(vhdlFilesBuf), top = std::string(vhdlTopBuf)](LocalSNLProvider& p) {
                        return p.loadVHDL(files, top);
                      });
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(buttonWidth(),0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }

  renderDesignLoadDialog();
#endif

  // ==== Main layout ====
  ImGuiWindowFlags window_flags =
    ImGuiWindowFlags_NoTitleBar     | ImGuiWindowFlags_NoResize   |
    ImGuiWindowFlags_NoMove         | ImGuiWindowFlags_NoCollapse |
    ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
    ImGuiWindowFlags_NoScrollbar    | ImGuiWindowFlags_NoScrollWithMouse;

  float menuBarHeight = ImGui::GetFrameHeight();
  ImGui::SetNextWindowPos(ImVec2(0, menuBarHeight));
  ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, io.DisplaySize.y - menuBarHeight));
  ImGui::Begin("MainWindow", nullptr, window_flags);
  {
    // Two columns: the left one holds the hierarchy tree over the
    // selection panel (the selected instance's terms and nets), the right
    // one the schematic over the tabs. Each column has its own horizontal
    // splitter.
    static float leftWidth       = state.initialLeftWidth;
    static float treeHeight      = -1.0f;
    static float schematicHeight = -1.0f;
    const float  minPanelW = 0.0f;
    const float  maxPanelW = io.DisplaySize.x - 100.0f;
    const float  minPanelH = 40.0f;
    float totalH = ImGui::GetContentRegionAvail().y;
    if (treeHeight < 0.0f)      treeHeight      = totalH * 0.65f;
    if (schematicHeight < 0.0f) schematicHeight = totalH * state.initialSchematicShare;
    if (state.requestedSchematicShare > 0.0f) {
      schematicHeight = totalH * state.requestedSchematicShare;
      state.requestedSchematicShare = -1.0f;
    }
    // Show Sources: the reference list plus readable code need more than
    // the default bottom panel. Grow it to this share of the height, once
    // per Show Sources, and only if it's smaller: a panel the user made
    // bigger stays as is, and later drags aren't undone.
    constexpr float kSourcePanelShare = 0.45f;
    if (state.growSourcePanel) {
      state.growSourcePanel = false;
      schematicHeight = std::min(schematicHeight, totalH * (1.0f - kSourcePanelShare));
    }

    auto vSplitter = [&](const char* id, float& width) {
      ImGui::SameLine();
      ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0,0,0,0));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f,0.3f,0.3f,0.5f));
      ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.2f,0.2f,0.2f,0.5f));
      ImGui::Button(id, ImVec2(4.0f, -1));
      ImGui::PopStyleColor(3);
      if (ImGui::IsItemActive()) {
        width += ImGui::GetIO().MouseDelta.x;
        if (width < minPanelW) width = minPanelW;
        if (width > maxPanelW) width = maxPanelW;
      }
      if (ImGui::IsItemHovered() || ImGui::IsItemActive())
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    };

    auto hSplitter = [&](const char* id, float& height) {
      ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0,0,0,0));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f,0.3f,0.3f,0.5f));
      ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.2f,0.2f,0.2f,0.5f));
      ImGui::Button(id, ImVec2(-1, 4.0f));
      ImGui::PopStyleColor(3);
      if (ImGui::IsItemActive()) {
        height += ImGui::GetIO().MouseDelta.y;
        float maxH = totalH - minPanelH - 4.0f;
        if (height < minPanelH) height = minPanelH;
        if (height > maxH)      height = maxH;
      }
      if (ImGui::IsItemHovered() || ImGui::IsItemActive())
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    };

    // === Left column: tree / hsplitter / selection panel ===
    if (state.showLeftColumn) {
    // A child width of 0 means "fill the rest": keep a collapsed column at 1.
    ImGui::BeginChild("LeftColumn", ImVec2(std::max(leftWidth, 1.0f), 0), false,
      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    {
      ImGui::BeginChild("TreePanel", ImVec2(0, treeHeight), true);
      {
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Netlist Hierarchy");
        NetlistTree::renderFilterMenu();
        ImGui::Separator();
        ImGui::BeginChild("TreeScroll", ImVec2(0, 0), false,
          ImGuiWindowFlags_HorizontalScrollbar);
        {
          if (state.guiData->netlist_ && state.connected) {
            state.guiData->netlist_->render();
          } else {
            ImGui::Text("Connecting to netlist provider...");
          }
        }
        ImGui::EndChild();
      }
      ImGui::EndChild();

      hSplitter("##HSplitLeft", treeHeight);

      // The selected instance's Pins / Nets (the top's Ports / Nets).
      ImGui::BeginChild("SelectionPanel", ImVec2(0, 0), true,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
      if (state.guiData->netlist_ && state.connected)
        state.guiData->netlist_->renderSelectionPanel();
      ImGui::EndChild();
    }
    ImGui::EndChild();

    vSplitter("##VSplit", leftWidth);
    ImGui::SameLine();
    }

    // === Right column: schematic / hsplitter / tabs ===
    ImGui::BeginChild("RightColumn", ImVec2(0, 0), false,
      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    {
      ImGui::BeginChild("SchematicPanel", ImVec2(0, schematicHeight), true,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
      {
        if (EquipotentialView::takePendingClear()) {
          ShowNetStore::forget();
          state.guiData->clearEquipotentials();
        }
        EquipotentialView::renderSchematic(state.guiData->equipotentials_);
        if (state.svgExportRequested && EquipotentialView::hasSchematic()) {
          state.svgExportRequested = false;
          json reply;
          reply["request"] = "svg_exported";
          reply["svg"]     = EquipotentialView::exportSvg();
          state.provider->send(reply.dump());
        }
        if (state.pngExportRequested && EquipotentialView::hasSchematic()) {
          state.pngExportRequested = false;
          json reply;
          reply["request"] = "png_exported";
          reply["png"]     = PngWriter::base64(EquipotentialView::exportPng(state.pngScale));
          state.provider->send(reply.dump());
        }
      }
      ImGui::EndChild();

      hSplitter("##HSplitter", schematicHeight);

      // Equipotential / Traces / Review / Source / Properties tabs, under
      // the schematic.
      ImGui::BeginChild("TablePanel", ImVec2(0, 0), true,
        ImGuiWindowFlags_AlwaysVerticalScrollbar | ImGuiWindowFlags_HorizontalScrollbar);
      {
        if (ImGui::BeginTabBar("##BottomTabs")) {
          if (ImGui::BeginTabItem("Equipotential")) {
            const auto& eqs = state.guiData->equipotentials_;
            size_t n = std::min(state.tableEquipotentialCount, eqs.size());
            std::vector<Equipotential*> lastEquip(eqs.end() - n, eqs.end());
            EquipotentialView::renderTable(lastEquip);
            ImGui::EndTabItem();
          }
          if (ImGui::BeginTabItem("Traces")) {
            TraceView::render(*state.guiData);
            ImGui::EndTabItem();
          }
          // Diagnosis UI temporarily hidden — see DiagnosisStore.cpp kDiagnosisUIHidden.
          // ImGuiTabItemFlags diagFlags = state.focusDiagnosisTab ? ImGuiTabItemFlags_SetSelected : 0;
          // if (ImGui::BeginTabItem("Diagnostics", nullptr, diagFlags)) {
          //   DiagnosisView::render();
          //   ImGui::EndTabItem();
          // }
          // "Review (3)": the open threads. A fixed ID, so the tab stays put
          // as the count changes.
          size_t open = 0;
          for (const auto& t : ReviewStore::threads()) open += t.isOpen();
          std::string reviewTab = open ? "Review (" + std::to_string(open) + ")###Review"
                                       : std::string("Review###Review");
          ImGuiTabItemFlags reviewFlags =
              ReviewView::takeFocusRequest() ? ImGuiTabItemFlags_SetSelected : 0;
          state.reviewTabShown = ImGui::BeginTabItem(reviewTab.c_str(), nullptr, reviewFlags);
          if (state.reviewTabShown) {
            ReviewView::render(state.provider);
            ImGui::EndTabItem();
          }
          ImGuiTabItemFlags srcFlags = state.focusSourceTab ? ImGuiTabItemFlags_SetSelected : 0;
          if (ImGui::BeginTabItem("Source", nullptr, srcFlags)) {
            SourceView::render(state.provider);
            ImGui::EndTabItem();
          }
          ImGuiTabItemFlags propFlags = state.focusPropertiesTab ? ImGuiTabItemFlags_SetSelected : 0;
          if (ImGui::BeginTabItem("Properties", nullptr, propFlags)) {
            PropertiesView::render();
            ImGui::EndTabItem();
          }
          // "Log (2)": errors since the tab was last shown. A fixed ID, so
          // the tab stays put as the count changes.
          const size_t unseen = LogView::unseenErrors();
          std::string logTab = unseen ? "Log (" + std::to_string(unseen) + ")###Log"
                                      : std::string("Log###Log");
          ImGuiTabItemFlags logFlags = state.focusLogTab ? ImGuiTabItemFlags_SetSelected : 0;
          if (ImGui::BeginTabItem(logTab.c_str(), nullptr, logFlags)) {
            LogView::render();
            ImGui::EndTabItem();
          }
          state.focusDiagnosisTab  = false;
          state.focusSourceTab     = false;
          state.focusPropertiesTab = false;
          state.focusLogTab        = false;
          ImGui::EndTabBar();
        }
      }
      ImGui::EndChild();
    }
    ImGui::EndChild();
  }
  ImGui::End();

  renderShortcutsWindow(shortcutsOpen);
  ReviewView::renderComposer(state.provider);

  // === About modal ===
  if (aboutOpen) { ImGui::OpenPopup("About naja-schematic"); aboutOpen = false; }
  ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 25, 0), ImGuiCond_Always);
  if (ImGui::BeginPopupModal("About naja-schematic", nullptr,
      ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {

    ImGui::PushStyleColor(ImGuiCol_Text, theme().titleText);
    ImGui::SetWindowFontScale(1.2f);
    ImGui::Text("naja-schematic");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopStyleColor();

    ImGui::TextDisabled("Netlist schematic viewer for naja SNL designs");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::SeparatorText("Viewer");
    ImGui::Text("Version: %s", NAJA_SCHEMATIC_VERSION_STRING);
    ImGui::Text("Commit:  %s", NAJA_SCHEMATIC_GIT_HASH);
    // What loads the design and answers the viewer: the Python host, or
    // the native app itself (its root_response "server").
    ImGui::SeparatorText("Server");
    if (state.serverInfo.empty()) {
      ImGui::TextDisabled("Not connected yet");
    } else {
      auto field = [&](const char* key) {
        const auto it = state.serverInfo.find(key);
        return it != state.serverInfo.end() && it->is_string() ? it->get<std::string>()
                                                                : std::string("?");
      };
      ImGui::TextUnformatted(field("name").c_str());
      ImGui::Text("Version: %s", field("version").c_str());
      ImGui::Text("naja:    %s (%s)", field("naja").c_str(), field("naja_git").c_str());
    }
    ImGui::Spacing();
    ImGui::Text("Project: github.com/keplertech/naja-schematic");
    ImGui::Spacing();

    ImGui::SeparatorText("License");
    ImGui::TextWrapped("Apache License 2.0  —  Copyright najaeda contributors");
    ImGui::Spacing();

    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - 80.0f) * 0.5f);
    if (ImGui::Button("Close", ImVec2(ImGui::GetFontSize() * 5, 0)))
      ImGui::CloseCurrentPopup();

    ImGui::EndPopup();
  }

  // === GL render ===
  if (state.hooks.beforeRender) state.hooks.beforeRender();
  ImGui::Render();
  glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
  glClearColor(0.1f, 0.1f, 0.2f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
  if (state.hooks.afterDraw) state.hooks.afterDraw();
  SDL_GL_SwapWindow(state.window);

  return !quit;
}

#include "AppLogic.h"

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
#include "ReviewStore.h"
#include "ReviewView.h"

#ifndef __EMSCRIPTEN__
#include <fstream>
#include "LocalSNLProvider.h"
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
    {"Click an open (hollow) pin",   "Add its net to the schematic"},
    {"Click a bus pin",              "Show its bits"},
    {"Double-click a dashed box",    "Show all of its pins"},
    {"Click a box",                  "Select the instance (also in the tree)"},
    {"Click a stacked (summary) box","Show the sinks it gathers (View > Fanout)"},
    {"Click a wire",                 "Select (highlight) its net"},
    {"Click a box's hierarchy glyph","Show / hide what is inside it"},
    {"Click a review badge",         "Open its thread in the Review tab"},
    {"Right-click",                  "Context menu: trace, sources, properties, comment, fit, clear"},
    {"Right-click a tree row",       "Its menu; Add Comment... starts a review thread (with --review)"},
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
  ImGui::SetNextWindowSize(ImVec2(520, 0), ImGuiCond_FirstUseEver);
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

bool appFrame(AppState& state) {
  SDL_Event event;
  bool quit = false;
  while (SDL_PollEvent(&event)) {
    ImGui_ImplSDL2_ProcessEvent(&event);
    if (event.type == SDL_QUIT) {
      quit = true;
    }
  }

  ImGuiIO& io = ImGui::GetIO();
  ImGui_ImplOpenGL3_NewFrame();
  ImGui_ImplSDL2_NewFrame();
  ImGui::NewFrame();

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
      if (ImGui::MenuItem("Open naja-if...",        "")) { snlDialogOpen = true; snlPathBuf[0] = '\0'; }
      if (ImGui::MenuItem("Open Verilog...",        "")) { vrlDialogOpen = true; vrlFilesBuf[0] = '\0'; vrlLibertyBuf[0] = '\0'; }
      if (ImGui::MenuItem("Open SystemVerilog...",  "")) { svDialogOpen  = true; svFilesBuf[0]  = '\0'; svTopBuf[0] = '\0'; }
      if (ImGui::MenuItem("Open VHDL...",           "")) { vhdlDialogOpen = true; vhdlFilesBuf[0] = '\0'; vhdlTopBuf[0] = '\0'; }
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
      // How the schematic is drawn (ViewOptions; also the host's
      // view_options push and the native --instance-label/--sink-rows/
      // --max-sinks/--group-by flags).
      ViewOptions& vo = ViewOptions::current();
      if (ImGui::BeginMenu("Instance Labels")) {
        for (InstanceLabel l : { InstanceLabel::Name, InstanceLabel::Model, InstanceLabel::Both }) {
          const char* text = l == InstanceLabel::Name ? "Name" : l == InstanceLabel::Model ? "Cell Model" : "Name and Cell Model";
          if (ImGui::MenuItem(text, "", vo.instanceLabel == l)) vo.instanceLabel = l;
        }
        ImGui::EndMenu();
      }
      if (ImGui::BeginMenu("Fanout")) {
        ImGui::SetNextItemWidth(120.0f);
        ImGui::DragInt("Rows per sink column", &vo.sinkRows, 0.2f, 0, 1000, vo.sinkRows ? "%d" : "no limit");
        ImGui::SetNextItemWidth(120.0f);
        ImGui::DragInt("Sinks drawn per net", &vo.maxSinks, 0.2f, 0, 100000, vo.maxSinks ? "%d" : "all");
        ImGui::SeparatorText("Gather the others by");
        for (FanoutGroupBy g : { FanoutGroupBy::Model, FanoutGroupBy::Hierarchy, FanoutGroupBy::Role }) {
          const char* text = g == FanoutGroupBy::Model ? "Cell Model" : g == FanoutGroupBy::Hierarchy ? "Module" : "Pin (clock / name)";
          if (ImGui::MenuItem(text, "", vo.groupBy == g)) vo.groupBy = g;
        }
        ImGui::EndMenu();
      }
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

  auto* localProvider = dynamic_cast<LocalSNLProvider*>(state.provider);

  // ==== Open naja-if dialog ====
  if (snlDialogOpen) { ImGui::OpenPopup("Open naja-if"); snlDialogOpen = false; }
  ImGui::SetNextWindowSize(ImVec2(540, 0), ImGuiCond_Always);
  if (ImGui::BeginPopupModal("Open naja-if", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::Text("naja-if directory path:");
    ImGui::SameLine();
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
    if ((ImGui::Button("Open", ImVec2(120,0)) || ok) && snlPathBuf[0]) {
      if (localProvider) { localProvider->loadSNL(snlPathBuf); reloadNetlist(state); }
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120,0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }

  // ==== Open Verilog dialog ====
  if (vrlDialogOpen) { ImGui::OpenPopup("Open Verilog"); vrlDialogOpen = false; }
  ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_Always);
  if (ImGui::BeginPopupModal("Open Verilog", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::Text("Verilog source files (one path per line):");
    ImGui::SameLine();
    if (ImGui::SmallButton("Browse...##vrlsrc")) {
      auto files = NativeFileDialog::pickFiles("Select Verilog Files", {"v"});
      appendPaths(vrlFilesBuf, sizeof(vrlFilesBuf), files);
    }
    ImGui::InputTextMultiline("##vrlfiles", vrlFilesBuf, sizeof(vrlFilesBuf), ImVec2(-1, 120));
    ImGui::Spacing();
    ImGui::Text("Liberty files (optional, one path per line):");
    ImGui::SameLine();
    if (ImGui::SmallButton("Browse...##vrlliberty")) {
      auto files = NativeFileDialog::pickFiles("Select Liberty Files", {"lib"});
      appendPaths(vrlLibertyBuf, sizeof(vrlLibertyBuf), files);
    }
    ImGui::InputTextMultiline("##vrlliberty", vrlLibertyBuf, sizeof(vrlLibertyBuf), ImVec2(-1, 80));
    ImGui::Spacing();
    ImGui::Checkbox("Decode Yosys src attributes", &vrlYosysSrc);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("For a Yosys-synthesized netlist: Show Sources then also\n"
                        "lists the RTL each gate came from");
    ImGui::Spacing();
    if (ImGui::Button("Open", ImVec2(120,0)) && vrlFilesBuf[0]) {
      if (localProvider) {
        localProvider->loadVerilog(splitLines(vrlFilesBuf), splitLines(vrlLibertyBuf), vrlYosysSrc);
        reloadNetlist(state);
      }
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120,0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }

  // ==== Open SystemVerilog dialog ====
  if (svDialogOpen) { ImGui::OpenPopup("Open SystemVerilog"); svDialogOpen = false; }
  ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_Always);
  if (ImGui::BeginPopupModal("Open SystemVerilog", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::Text("SV/V files or Flist path (one per line):");
    ImGui::SameLine();
    if (ImGui::SmallButton("Browse SV...")) {
      auto files = NativeFileDialog::pickFiles("Select SystemVerilog Files", {"sv", "v"});
      appendPaths(svFilesBuf, sizeof(svFilesBuf), files);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Browse Flist...")) {
      auto files = NativeFileDialog::pickFiles("Select Flist");
      appendPaths(svFilesBuf, sizeof(svFilesBuf), files);
    }
    ImGui::InputTextMultiline("##svfiles", svFilesBuf, sizeof(svFilesBuf), ImVec2(-1, 140));
    ImGui::Spacing();
    ImGui::Text("Top module (optional -- overrides auto-detection):");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##svtop", svTopBuf, sizeof(svTopBuf));
    ImGui::Spacing();
    if (ImGui::Button("Open", ImVec2(120,0)) && svFilesBuf[0]) {
      if (localProvider) {
        localProvider->loadSystemVerilog(splitLines(svFilesBuf), svTopBuf);
        reloadNetlist(state);
      }
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120,0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }

  // ==== Open VHDL dialog ====
  if (vhdlDialogOpen) { ImGui::OpenPopup("Open VHDL"); vhdlDialogOpen = false; }
  ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_Always);
  if (ImGui::BeginPopupModal("Open VHDL", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::Text("VHDL files, in any order (one per line):");
    ImGui::SameLine();
    if (ImGui::SmallButton("Browse...")) {
      auto files = NativeFileDialog::pickFiles("Select VHDL Files", {"vhd", "vhdl"});
      appendPaths(vhdlFilesBuf, sizeof(vhdlFilesBuf), files);
    }
    ImGui::InputTextMultiline("##vhdlfiles", vhdlFilesBuf, sizeof(vhdlFilesBuf), ImVec2(-1, 140));
    ImGui::Spacing();
    ImGui::Text("Top entity (optional -- else the unique uninstantiated one):");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##vhdltop", vhdlTopBuf, sizeof(vhdlTopBuf));
    ImGui::TextDisabled("naja's VHDL frontend is experimental (bit/bit_vector subset).");
    ImGui::Spacing();
    if (ImGui::Button("Open", ImVec2(120,0)) && vhdlFilesBuf[0]) {
      if (localProvider) {
        localProvider->loadVHDL(splitLines(vhdlFilesBuf), vhdlTopBuf);
        reloadNetlist(state);
      }
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120,0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
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
    static float leftWidthTop    = 300.0f;
    static float schematicHeight = -1.0f;
    const float  minPanelW = 0.0f;
    const float  maxPanelW = io.DisplaySize.x - 100.0f;
    const float  minPanelH = 40.0f;
    float totalH = ImGui::GetContentRegionAvail().y;
    if (schematicHeight < 0.0f) schematicHeight = totalH * 0.65f;
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

    // === Top row: tree | vsplitter | schematic ===
    ImGui::BeginChild("SchematicRow", ImVec2(0, schematicHeight), false,
      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    {
      ImGui::BeginChild("TreePanel", ImVec2(leftWidthTop, 0), true);
      {
        ImGui::Text("Netlist Hierarchy");
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

      vSplitter("##VSplitTop", leftWidthTop);

      ImGui::SameLine();
      ImGui::BeginChild("SchematicPanel", ImVec2(0, 0), true,
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
    }
    ImGui::EndChild();

    // === Horizontal splitter ===
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f,0.3f,0.3f,0.5f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.2f,0.2f,0.2f,0.5f));
    ImGui::Button("##HSplitter", ImVec2(-1, 4.0f));
    ImGui::PopStyleColor(3);
    if (ImGui::IsItemActive()) {
      schematicHeight += ImGui::GetIO().MouseDelta.y;
      float maxH = totalH - minPanelH - 4.0f;
      if (schematicHeight < minPanelH) schematicHeight = minPanelH;
      if (schematicHeight > maxH)      schematicHeight = maxH;
    }
    if (ImGui::IsItemHovered() || ImGui::IsItemActive())
      ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);

    // === Bottom row: Table / Diagnostics / RTL Source tabs ===
    ImGui::BeginChild("TableRow", ImVec2(0, 0), false,
      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    {
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
          state.focusDiagnosisTab  = false;
          state.focusSourceTab     = false;
          state.focusPropertiesTab = false;
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
  ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_Always);
  if (ImGui::BeginPopupModal("About naja-schematic", nullptr,
      ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.9f, 1.0f, 1.0f));
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
    if (ImGui::Button("Close", ImVec2(80, 0)))
      ImGui::CloseCurrentPopup();

    ImGui::EndPopup();
  }

  // === GL render ===
  ImGui::Render();
  glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
  glClearColor(0.1f, 0.1f, 0.2f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
  SDL_GL_SwapWindow(state.window);

  return !quit;
}

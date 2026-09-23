#include "AppLogic.h"

#include <algorithm>
#include <iostream>
#include <sstream>

#include <imgui.h>
#include <backends/imgui_impl_sdl2.h>
#include <backends/imgui_impl_opengl3.h>
#include <SDL_opengl.h>

#include <nlohmann/json.hpp>
using json = nlohmann::json;

#include "INetlistProvider.h"
#include "NetlistTree.h"
#include "GUIData.h"
#include "Types.h"
#include "Console.h"
#include "EquipotentialView.h"
#include "DiagnosisStore.h"
#include "DiagnosisView.h"
#include "SourceStore.h"
#include "SourceView.h"
#include "PropertiesStore.h"
#include "PropertiesView.h"
#include "DroidSansFont.h"
#include "Version.h"

#ifndef __EMSCRIPTEN__
#include <fstream>
#include "LocalSNLProvider.h"
#include "NativeFileDialog.h"
#endif

// Base UI font size in pixels. ImGui's built-in fallback (Proggy Clean, ~13px
// bitmap font) reads as tiny/blurry on most displays; DroidSans at this size
// is crisp and legible for both chrome (menus, panels) and schematic labels,
// which scale off of it (see SchematicView's labelFontSize()).
static constexpr float kBaseFontSizePx = 17.0f;

void setupFonts() {
  ImGuiIO& io = ImGui::GetIO();
  io.Fonts->AddFontFromMemoryCompressedTTF(
      DroidSans_compressed_data, DroidSans_compressed_size, kBaseFontSizePx);
}

// ---------------------------------------------------------------------------
// Provider setup — identical message dispatch for both WASM and native modes.
// ---------------------------------------------------------------------------

// Wire the equipotential-reset callback onto whichever NetlistTree is current.
// Called both at startup and after every design reload.
static void attachTreeCallbacks(AppState& state) {
  state.guiData->netlist_->setOnEquipotentialRequest([&state]() {
    state.guiData->clearEquipotentials();
    EquipotentialView::resetLayout();
  });
}

void setupProvider(AppState& state) {
  state.guiData->netlist_ = new NetlistTree(state.provider);
  EquipotentialView::setProvider(state.provider);
  attachTreeCallbacks(state);

  state.provider->on_open([&state]() {
    state.connected = true;
    Console::Log("Connected to netlist provider");
    state.provider->send(R"({"request":"load_root"})");
  });

  state.provider->on_message([&state](const std::string& msg) {
    Console::Log("Message received: " + msg);
    std::string clean = msg;
    auto nullPos = clean.find('\0');
    if (nullPos != std::string::npos) clean.resize(nullPos);
    while (!clean.empty() &&
           (clean.back() == '\n' || clean.back() == '\r' ||
            clean.back() == ' '  || clean.back() == '\t')) {
      clean.pop_back();
    }

    json j;
    try {
      j = json::parse(clean);
    } catch (const std::exception& e) {
      Console::Error("Failed to parse JSON: " + std::string(e.what()));
      return;
    }

    std::string resp = j.value("response", "");
    if (resp.empty()) {
      Console::Error("Missing response field.");
      return;
    }

    if (resp == "root_response" || resp == "root_loaded") {
      Console::Log("Root node data received");
      DiagnosisStore::clear();  // stale diagnoses reference the old design
      PropertiesStore::clear(); // stale properties reference the old design
      const auto& root = j["root"];
      if (root.contains("has_terms") || root.contains("has_primitives") || root.contains("has_instances")) {
        InstanceResponseJson data = root.get<InstanceResponseJson>();
        state.guiData->netlist_->createRootNode(
          data.name, data.design_ref,
          data.has_terms, data.has_primitives, data.has_instances, data.has_nets,
          data.source_loc);
      } else {
        DesignRef designRef{};
        if (root.contains("design_ref")) {
          designRef = root["design_ref"].get<DesignRef>();
        }
        state.guiData->netlist_->createRootNode(
          root.value("name", std::string("<unnamed root>")),
          designRef, false, false, root.value("has_children", false), false);
      }
    } else if (resp == "instances_response" || resp == "primitives_response" || resp == "children_loaded") {
      unsigned gui_id = 0;
      std::vector<InstanceResponseJson> children;
      if (resp == "children_loaded") {
        gui_id = j.value("node_gui_id", 0);
        const auto& rawChildren = j["children"];
        if (rawChildren.is_array()) {
          for (const auto& child : rawChildren) {
            InstanceResponseJson item;
            item.name       = child.value("name", "");
            item.model_name = child.value("model_name", "");
            item.child_id   = child.value("instance_id", 0);
            if (child.contains("design_ref"))
              item.design_ref = child["design_ref"].get<DesignRef>();
            bool hasChildren   = child.value("has_children", false);
            item.has_terms     = false;
            item.has_primitives = false;
            item.has_instances  = hasChildren;
            item.has_nets       = false;
            children.push_back(std::move(item));
          }
        }
      } else {
        InstancesResponseJson data = j.get<InstancesResponseJson>();
        gui_id   = data.gui_id;
        children = std::move(data.children);
      }
      auto parent = state.guiData->netlist_->getNode(gui_id);
      if (!parent) {
        Console::Error("Cannot find node: " + std::to_string(gui_id));
        return;
      }
      if (parent->hasChildren()) {
        Console::Error("internal error: node already has children");
        return;
      }
      parent->createChildren();
      for (auto& instance : children) {
        parent->createInstanceNode(
          instance.name, instance.model_name, instance.child_id, instance.design_ref,
          instance.has_terms, instance.has_primitives, instance.has_instances, instance.has_nets,
          instance.source_loc);
      }
    } else if (resp == "terms_response") {
      TermsResponseJson data = j.get<TermsResponseJson>();
      auto parent = state.guiData->netlist_->getNode(data.gui_id);
      if (!parent) {
        Console::Error("Cannot find node: " + std::to_string(data.gui_id));
        return;
      }
      if (parent->hasChildren()) {
        Console::Error("internal error: node already has children");
        return;
      }
      parent->createChildren();
      for (auto& term : data.children) {
        parent->createTermNode(term.name, term.child_id,
                               Direction(term.direction), term.msb, term.lsb);
      }
    } else if (resp == "nets_response") {
      NetsResponseJson data = j.get<NetsResponseJson>();
      auto parent = state.guiData->netlist_->getNode(data.gui_id);
      if (!parent) {
        Console::Error("Cannot find node: " + std::to_string(data.gui_id));
        return;
      }
      if (parent->hasChildren()) {
        Console::Error("internal error: node already has children");
        return;
      }
      parent->createChildren();
      for (auto& net : data.children) {
        parent->createNetNode(net.name, net.msb, net.lsb);
      }
    } else if (resp == "equipotential_response") {
      Console::Log("Equipotential data received");
      state.guiData->addEquipotential(new Equipotential(j.get<Equipotential>()));
      state.tableEquipotentialCount = 1;
    } else if (resp == "trace_driver_response") {
      // Nets arrive breadth-first from the traced net toward the drivers, so
      // adding them in order lets the layout chain each one off an instance
      // that's already placed.
      size_t n = 0;
      if (j.contains("equipotentials") && j["equipotentials"].is_array()) {
        for (const auto& e : j["equipotentials"]) {
          state.guiData->addEquipotential(new Equipotential(e.get<Equipotential>()));
          ++n;
        }
      }
      state.tableEquipotentialCount = std::max<size_t>(n, 1);
      Console::Log("Driver trace received: " + std::to_string(n) + " net(s)" +
                   (j.value("truncated", false) ? " (truncated)" : ""));
    } else if (resp == "expanded_instance_terms") {
      std::string pathKey = j.value("path_key", std::string(""));
      std::vector<EquipotentialView::ExpandedPort> ports;
      if (j.contains("terms") && j["terms"].is_array()) {
        for (const auto& t : j["terms"]) {
          EquipotentialView::ExpandedPort ep;
          ep.name    = t.value("name", std::string(""));
          ep.childId = t.value("child_id", 0u);
          if (t.contains("bit") && !t["bit"].is_null())
            ep.bit = t["bit"].get<int>();
          int dirInt   = t.value("direction", 0);
          ep.direction = dirInt == 1 ? Direction::Output
                       : dirInt == 2 ? Direction::Inout
                                     : Direction::Input;
          ports.push_back(std::move(ep));
        }
      }
      EquipotentialView::applyInstanceExpansion(pathKey, ports);
    } else if (resp == "instance_internals_response") {
      std::string pathKey = j.value("path_key", std::string(""));
      EquipotentialView::InstanceInternals data;
      if (j.contains("children") && j["children"].is_array()) {
        for (const auto& c : j["children"]) {
          EquipotentialView::InstanceChild ic;
          ic.name    = c.value("name", std::string(""));
          ic.childId = c.value("child_id", 0u);
          if (c.contains("design_ref"))
            ic.designRef = c["design_ref"].get<DesignRef>();
          ic.hasInstances = c.value("has_instances", false) || c.value("has_primitives", false);
          data.children.push_back(std::move(ic));
        }
      }
      if (j.contains("nets") && j["nets"].is_array()) {
        for (const auto& n : j["nets"]) {
          EquipotentialView::InternalNet in;
          in.name = n.value("name", std::string(""));
          if (n.contains("bit") && !n["bit"].is_null()) in.bit = n["bit"].get<int>();
          if (n.contains("pins") && n["pins"].is_array()) {
            for (const auto& p : n["pins"]) {
              EquipotentialView::InternalPin ip;
              ip.name    = p.value("name", std::string(""));
              int dirInt = p.value("direction", 0);
              ip.direction = dirInt == 1 ? Direction::Output
                           : dirInt == 2 ? Direction::Inout
                                         : Direction::Input;
              if (p.contains("bit") && !p["bit"].is_null()) ip.bit = p["bit"].get<int>();
              if (p.contains("inst_id") && !p["inst_id"].is_null())
                ip.instChildId = p["inst_id"].get<unsigned>();
              in.pins.push_back(std::move(ip));
            }
          }
          data.nets.push_back(std::move(in));
        }
      }
      EquipotentialView::applyInstanceInternals(pathKey, data);
    } else if (resp == "source_response") {
      std::string file = j.value("file", std::string(""));
      bool found = j.value("found", false);
      if (found) {
        SourceStore::setSource(file, j.value("line", 0), j.value("text", std::string("")));
        state.focusSourceTab = true;
        Console::Log("Loaded RTL source: " + file);
      } else {
        Console::Error("Failed to load RTL source: " + file);
      }
    } else if (resp == "properties_response") {
      PropertiesResponseJson data = j.get<PropertiesResponseJson>();
      std::string subject = j.value("subject", std::string(""));
      Console::Log("Properties received: " + std::to_string(data.properties.size()) + " item(s)");
      PropertiesStore::setProperties(subject, std::move(data.properties));
      state.focusPropertiesTab = true;
    } else if (resp == "diagnosis_response") {
      std::vector<DiagnosisItem> items;
      if (j.contains("items") && j["items"].is_array()) {
        for (const auto& raw : j["items"]) {
          items.push_back(raw.get<DiagnosisItem>());
        }
      }
      Console::Log("Diagnosis received: " + std::to_string(items.size()) + " item(s)");
      DiagnosisStore::setDiagnostics(std::move(items));
      state.focusDiagnosisTab = true;
    } else if (resp == "error") {
      std::cerr << "Backend error: " << j["message"] << std::endl;
    }
  });

  state.provider->on_error([](const std::string& err) {
    Console::Error("Provider error: " + err);
  });

  state.provider->on_close([]() {
    Console::Error("Provider connection closed");
  });

  state.provider->start();
}

#ifndef __EMSCRIPTEN__
// ---------------------------------------------------------------------------
// Native diagnosis-file loading — shared by the menu action and by main()'s
// --diagnosis flag. See AppLogic.h for the calling contract.
// ---------------------------------------------------------------------------

bool loadDiagnosisFile(const std::string& path) {
  std::ifstream ifs(path);
  if (!ifs) {
    Console::Error("Failed to open diagnosis file: " + path);
    return false;
  }
  try {
    json j; ifs >> j;
    const auto& arr = j.contains("items") ? j["items"] : j;
    std::vector<DiagnosisItem> items;
    if (arr.is_array()) {
      for (const auto& raw : arr) items.push_back(raw.get<DiagnosisItem>());
    }
    Console::Log("Loaded " + std::to_string(items.size()) + " diagnosis item(s) from " + path);
    DiagnosisStore::setDiagnostics(std::move(items));
    return true;
  } catch (const std::exception& e) {
    Console::Error("Failed to parse diagnosis JSON: " + std::string(e.what()));
    return false;
  }
}
#endif // __EMSCRIPTEN__

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

  // ==== Top Menu Bar ====
  static bool aboutOpen = false;
#ifndef __EMSCRIPTEN__
  static bool snlDialogOpen = false;
  static bool vrlDialogOpen = false;
  static bool svDialogOpen  = false;
  static char snlPathBuf[1024]    = {};
  static char vrlFilesBuf[8192]   = {};
  static char vrlLibertyBuf[4096] = {};
  static char svFilesBuf[8192]    = {};
  static char svTopBuf[256]       = {};
#endif

  if (ImGui::BeginMainMenuBar()) {
    if (ImGui::BeginMenu("File")) {
#ifndef __EMSCRIPTEN__
      if (ImGui::MenuItem("Open naja-if...",        "")) { snlDialogOpen = true; snlPathBuf[0] = '\0'; }
      if (ImGui::MenuItem("Open Verilog...",        "")) { vrlDialogOpen = true; vrlFilesBuf[0] = '\0'; vrlLibertyBuf[0] = '\0'; }
      if (ImGui::MenuItem("Open SystemVerilog...",  "")) { svDialogOpen  = true; svFilesBuf[0]  = '\0'; svTopBuf[0] = '\0'; }
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
      if (ImGui::MenuItem("About")) aboutOpen = true;
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
      if (ImGui::MenuItem("Zoom In",    "Ctrl++")) EquipotentialView::zoomIn();
      if (ImGui::MenuItem("Zoom Out",   "Ctrl+-")) EquipotentialView::zoomOut();
      if (ImGui::MenuItem("Fit",        "Ctrl+0")) EquipotentialView::fitView();
      if (ImGui::MenuItem("Show Hierarchy", "", EquipotentialView::showHierarchy()))
        EquipotentialView::setShowHierarchy(!EquipotentialView::showHierarchy());
      ImGui::Separator();
      if (ImGui::MenuItem("Clear nets", "Ctrl+K")) state.guiData->clearEquipotentials();
      // Diagnosis UI temporarily hidden — see DiagnosisStore.cpp kDiagnosisUIHidden.
      // if (ImGui::MenuItem("Clear diagnosis", ""))  DiagnosisStore::clear();
      ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
  }

#ifndef __EMSCRIPTEN__
  // Helper: reset the netlist tree and re-request root after loading
  auto reloadNetlist = [&]() {
    state.guiData->clearEquipotentials();
    EquipotentialView::clearNets();
    delete state.guiData->netlist_;
    state.guiData->netlist_ = new NetlistTree(state.provider);
    attachTreeCallbacks(state);
    state.provider->send(R"({"request":"load_root"})");
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
      if (localProvider) { localProvider->loadSNL(snlPathBuf); reloadNetlist(); }
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
    if (ImGui::Button("Open", ImVec2(120,0)) && vrlFilesBuf[0]) {
      if (localProvider) {
        localProvider->loadVerilog(splitLines(vrlFilesBuf), splitLines(vrlLibertyBuf));
        reloadNetlist();
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
        reloadNetlist();
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
        if (EquipotentialView::takePendingClear()) state.guiData->clearEquipotentials();
        EquipotentialView::renderSchematic(state.guiData->equipotentials_);
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
          // Diagnosis UI temporarily hidden — see DiagnosisStore.cpp kDiagnosisUIHidden.
          // ImGuiTabItemFlags diagFlags = state.focusDiagnosisTab ? ImGuiTabItemFlags_SetSelected : 0;
          // if (ImGui::BeginTabItem("Diagnostics", nullptr, diagFlags)) {
          //   DiagnosisView::render();
          //   ImGui::EndTabItem();
          // }
          ImGuiTabItemFlags srcFlags = state.focusSourceTab ? ImGuiTabItemFlags_SetSelected : 0;
          if (ImGui::BeginTabItem("RTL Source", nullptr, srcFlags)) {
            SourceView::render();
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

    ImGui::Text("Version: %s", naja_schematic::VERSION.c_str());
    ImGui::Text("Commit:  %s", NAJA_SCHEMATIC_GIT_HASH);
    ImGui::Text("naja:    %s", NAJA_VERSION_STRING);
    ImGui::Text("Project: github.com/najaeda/naja-schematic");
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

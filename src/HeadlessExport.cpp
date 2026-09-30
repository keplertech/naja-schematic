#ifndef __EMSCRIPTEN__

#include "HeadlessExport.h"

#include <fstream>
#include <iostream>

#include <imgui.h>

#include "AppLogic.h"
#include "EquipotentialView.h"
#include "GUIData.h"
#include "LocalSNLProvider.h"
#include "SelectionStore.h"
#include "TraceStore.h"

using json = nlohmann::json;

namespace {

bool endsWith(const std::string& s, const std::string& suffix) {
  return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool allOf(const json& list, bool (json::*is)() const noexcept) {
  for (const auto& v : list) if (!(v.*is)()) return false;
  return true;
}

// "u1/u2.A[3]", for a trace's label and error messages.
std::string termLabel(const json& spec) {
  std::string out;
  const json& path = spec.contains("id_path") ? spec["id_path"] : spec.value("path", json::array());
  for (const auto& seg : path)
    out += (seg.is_string() ? seg.get<std::string>() : "<#" + seg.dump() + ">") + "/";
  if (!out.empty()) out.back() = '.';
  out += spec.value("terminal", "");
  if (spec.contains("bit")) out += "[" + spec["bit"].dump() + "]";
  return out;
}

// One headless frame: the schematic panel alone, filling the display. The
// layout (and any request it sends, e.g. a partial gate's full interface)
// happens here, exactly as in the GUI.
void frame(GUIData& gui) {
  ImGui::NewFrame();
  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
  ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
  ImGui::Begin("Schematic", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
  if (EquipotentialView::takePendingClear()) gui.clearEquipotentials();
  EquipotentialView::renderSchematic(gui.equipotentials_);
  ImGui::End();
  ImGui::Render();
}

} // namespace

std::optional<json> parseFocusArg(const std::string& arg, std::string& error) {
  json j = json::parse(arg, nullptr, /*allow_exceptions=*/false);
  if (j.is_array() && allOf(j, &json::is_number_unsigned)) return json{{"id_path", j}};
  if (j.is_array() && allOf(j, &json::is_string))          return json{{"path", j}};
  if (j.is_object() && (j.contains("id_path") || j.contains("path"))) return j;
  error = "--focus takes a JSON list of instance ids ([3,7]) or names ([\"u1\",\"u2\"]), got: " + arg;
  return std::nullopt;
}

std::optional<json> parseTermArg(const std::string& arg, std::string& error) {
  json j = json::parse(arg, nullptr, /*allow_exceptions=*/false);
  if (j.is_object() && j.contains("terminal") && j["terminal"].is_string()) {
    if (!j.contains("id_path") && !j.contains("path")) j["id_path"] = json::array();
    return j;
  }
  error = "expected a JSON object like {\"id_path\":[3,7],\"terminal\":\"A\",\"bit\":0}, got: " + arg;
  return std::nullopt;
}

int runHeadlessExport(LocalSNLProvider* provider, const HeadlessExportOptions& options) {
  if (!endsWith(options.outPath, ".svg")) {
    std::cerr << "--export writes SVG: give a .svg path (convert to PDF with e.g. "
                 "`rsvg-convert -f pdf out.svg -o out.pdf`)\n";
    return 1;
  }
  if (!provider->hasDesign()) {
    std::cerr << "--export: the design did not load (see the messages above)\n";
    return 1;
  }
  if (!options.focus && options.traces.empty() && options.equipotentials.empty()) {
    std::cerr << "--export needs something to draw: --focus, --trace or --equipotential\n";
    return 1;
  }

  // An ImGui context with the app's font, so names are measured (and the
  // layout sized) exactly as in the GUI. No backend: nothing is rendered.
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.DisplaySize = ImVec2(1600.0f, 1000.0f);
  io.DeltaTime   = 1.0f / 60.0f;
  setupFonts();
  unsigned char* pixels; int w, h;
  io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);

  AppState state;
  state.guiData  = new GUIData();
  state.provider = provider;
  setupProvider(state);   // answers load_root synchronously

  int status = 0;
  if (!options.diagnosisPath.empty() && !loadDiagnosisFile(options.diagnosisPath)) status = 1;

  // The provider answers each request synchronously, through the same
  // response handling as the GUI (AppLogic.cpp).
  const auto isTop = [](const json& focus) {
    const json& p = focus.contains("id_path") ? focus["id_path"] : focus.value("path", json::array());
    return p.is_array() && p.empty();
  };
  if (options.focus && isTop(*options.focus)) {
    std::cerr << "--focus []: the top design has no box to draw; use --equipotential or "
                 "--trace on its ports ({\"terminal\":\"<port>\"})\n";
    status = 1;
  } else if (options.focus) {
    json req = *options.focus;
    req["request"] = "resolve_instance";
    provider->send(req.dump());
    if (!SelectionStore::hasSelection()) {
      std::cerr << "--focus: no instance " << options.focus->dump() << " in the design\n";
      status = 1;
    }
  }
  auto sendTerm = [&](const json& spec, const char* request, bool trace) {
    std::string error;
    auto req = provider->termRequest(spec, error);
    if (!req) {
      std::cerr << "--" << (trace ? "trace" : "equipotential") << " " << termLabel(spec) << ": " << error << "\n";
      status = 1;
      return;
    }
    (*req)["request"] = request;
    if (trace) (*req)["trace_id"] = TraceStore::begin(termLabel(spec));
    provider->send(req->dump());
  };
  for (const auto& spec : options.equipotentials) sendTerm(spec, "load_equipotential", false);
  for (const auto& spec : options.traces)         sendTerm(spec, "trace_driver", true);

  // A few frames: the first lays the schematic out, and may ask for more
  // (a gate reached through one input loads its whole interface), which the
  // next ones draw.
  for (int i = 0; i < 4; ++i) frame(*state.guiData);

  // Nothing is written when part of the request failed: a script must not
  // take a partial drawing for the one it asked for.
  const std::string svg = status == 0 ? EquipotentialView::exportSvg() : std::string();
  if (status != 0) {
    // already reported
  } else if (svg.empty()) {
    std::cerr << "--export: nothing to draw\n";
    status = 1;
  } else {
    std::ofstream out(options.outPath, std::ios::binary);
    out << svg;
    if (out) {
      std::cerr << "Schematic exported to " << options.outPath << "\n";
    } else {
      std::cerr << "--export: could not write " << options.outPath << "\n";
      status = 1;
    }
  }

  EquipotentialView::setProvider(nullptr);
  ImGui::DestroyContext();
  return status;
}

#endif // __EMSCRIPTEN__

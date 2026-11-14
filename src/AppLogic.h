#pragma once

#include <string>

#include <nlohmann/json.hpp>

class INetlistProvider;
class GUIData;

// SDL's types, declared here so that the headless renderer (no SDL) can
// use AppState and the message handling (AppMessages.cpp).
struct SDL_Window;

// Shared application state used by both native and WASM entry points.
struct AppState {
  SDL_Window*       window    {nullptr};   // null when headless
  void*             glContext {nullptr};   // an SDL_GLContext
  GUIData*          guiData   {nullptr};
  INetlistProvider* provider  {nullptr};
  bool              connected {false};
  // One-shot flags: set by the message handler when a source_response /
  // diagnosis_response / properties_response arrives, consumed (and
  // cleared) by appFrame() to auto-select the matching bottom-panel tab for
  // that one frame.
  bool              focusSourceTab     {false};
  // Set by a sources_response: give the bottom panel room for the code
  // once (see kSourcePanelShare in AppLogic.cpp).
  bool              growSourcePanel    {false};
  bool              focusDiagnosisTab  {false};
  bool              focusPropertiesTab {false};
  // The Review tab is the one shown: selecting a thread selects its
  // instance, whose properties_response then doesn't take the panel away
  // from the thread being read.
  bool              reviewTabShown     {false};
  // How many of the most recently added equipotentials the bottom
  // "Equipotential" table lists: 1 after a single load_equipotential, every
  // net of the cone after a trace_driver_response.
  size_t            tableEquipotentialCount {1};
  // Set by a host's export_svg push (notebook Schematic.export_svg()):
  // appFrame() answers with an svg_exported message once the schematic
  // shows something, so a request sent while the view is still loading
  // waits for its first drawing.
  bool              svgExportRequested {false};
  // Same for an export_png push: answered by a png_exported notification
  // (base64 PNG bytes), drawn `pngScale` pixels per SVG pixel.
  bool              pngExportRequested {false};
  float             pngScale {1.0f};
  // The root_response's "server": what answers the viewer (name, version,
  // naja, naja_git), shown in Help > About.
  nlohmann::json    serverInfo;
};

// Wire all provider callbacks (on_open, on_message, on_close, on_error)
// and call provider->start().  Must be called once after SDL/GL/ImGui init.
void setupProvider(AppState& state);

// Throw the loaded design away and ask the provider for the root again:
// after File > Open (native) and on a host's design_changed push. The
// root_response clears diagnoses, properties and the selection.
void reloadNetlist(AppState& state);

// Load the app's UI font (embedded DroidSans, larger and crisper than
// ImGui's default bitmap font). Must be called once after ImGui::CreateContext()
// and before the backend Init() calls (ImGui_ImplOpenGL3_Init() builds the
// font atlas texture from whatever is registered at that point).
void setupFonts();

// One frame: poll events, build ImGui, render.
// Returns false when the application should quit.
bool appFrame(AppState& state);

#ifndef __EMSCRIPTEN__
// Native only: load a diagnosis_response-shaped JSON file (a top-level
// {"items": [...]} object, or a bare array) from disk and install it into
// DiagnosisStore, replacing whatever diagnosis set is currently loaded.
// Shared by the "File > Load Diagnosis JSON..." menu action and by main()'s
// optional --diagnosis command-line flag, so an external caller (a script,
// or naja-agent's skill) can open a fully annotated view in one shot instead
// of requiring a human to click through the file picker.
// Must be called after setupProvider() has returned, since a fresh
// root_response/root_loaded clears any diagnosis set already installed.
// Returns true on success (logs the reason to Console and returns false on
// a missing/unreadable/malformed file).
bool loadDiagnosisFile(const std::string& path);
// main()'s --style flag: a style_response-shaped JSON ({"items": [...]} or
// a bare array; see StyleStore), same calling contract as
// loadDiagnosisFile(). False also when an item was skipped as malformed.
bool loadStyleFile(const std::string& path);
#endif

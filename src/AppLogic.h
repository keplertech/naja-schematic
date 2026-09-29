#pragma once

#include <SDL.h>
#include <SDL_opengl.h>
#include <string>

class INetlistProvider;
class GUIData;

// Shared application state used by both native and WASM entry points.
struct AppState {
  SDL_Window*       window    {nullptr};
  SDL_GLContext     glContext {};
  GUIData*          guiData   {nullptr};
  INetlistProvider* provider  {nullptr};
  bool              connected {false};
  // One-shot flags: set by the message handler when a source_response /
  // diagnosis_response / properties_response arrives, consumed (and
  // cleared) by appFrame() to auto-select the matching bottom-panel tab for
  // that one frame.
  bool              focusSourceTab     {false};
  bool              focusDiagnosisTab  {false};
  bool              focusPropertiesTab {false};
  // How many of the most recently added equipotentials the bottom
  // "Equipotential" table lists: 1 after a single load_equipotential, every
  // net of the cone after a trace_driver_response.
  size_t            tableEquipotentialCount {1};
};

// Wire all provider callbacks (on_open, on_message, on_close, on_error)
// and call provider->start().  Must be called once after SDL/GL/ImGui init.
void setupProvider(AppState& state);

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
#endif

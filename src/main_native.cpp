// Standalone desktop entry point.
// Compiled natively (no Emscripten); uses LocalSNLProvider to load netlists directly.
#ifndef __EMSCRIPTEN__

#include <SDL.h>
#include <SDL_opengl.h>
#include <imgui.h>
#include <backends/imgui_impl_sdl2.h>
#include <backends/imgui_impl_opengl3.h>
#include <iostream>
#include <string>

#include "AppLogic.h"
#include "HeadlessExport.h"
#include "GUIData.h"
#include "LocalSNLProvider.h"
#include "Console.h"

int main(int argc, char* argv[]) {
  // CLI usage: naja-schematic-standalone [<design>] [--liberty <path>]... [--diagnosis <path>]
  //                                     [--export <out.svg> [--focus <json>]
  //                                      [--trace <json>]... [--equipotential <json>]...]
  // <design> is loaded by extension (.sv/.v/otherwise-assumed-SNL-directory);
  // --liberty defines the primitive cell library for a .v design (repeatable,
  // once per file) -- it is not supported for .sv, since loadSystemVerilog()/
  // SNLSVConstructor has no liberty hook;
  // --diagnosis pre-loads a diagnosis_response-shaped JSON so an external
  // caller (a script, or naja-agent's skill, after an edit-check cycle) can
  // open a fully annotated view in one command instead of requiring a human
  // to click through File > Open .../Load Diagnosis JSON... by hand.
  // --export draws the schematic to an SVG file and exits, with no window
  // (see HeadlessExport.h for what --focus/--trace/--equipotential take).
  auto* provider = new LocalSNLProvider();

  std::string designPath;
  std::string diagnosisPath;
  std::vector<std::string> libertyPaths;
  HeadlessExportOptions exportOptions;
  bool exporting = false;
  for (int i = 1; i < argc; ++i) {
    std::string arg(argv[i]);
    std::string error;
    if (arg == "--export" && i + 1 < argc) {
      exportOptions.outPath = argv[++i];
      exporting = true;
    } else if (arg == "--focus" && i + 1 < argc) {
      exportOptions.focus = parseFocusArg(argv[++i], error);
      if (!exportOptions.focus) { std::cerr << error << "\n"; return 1; }
    } else if ((arg == "--trace" || arg == "--equipotential") && i + 1 < argc) {
      auto spec = parseTermArg(argv[++i], error);
      if (!spec) { std::cerr << arg << ": " << error << "\n"; return 1; }
      (arg == "--trace" ? exportOptions.traces : exportOptions.equipotentials).push_back(*spec);
    } else if (arg == "--diagnosis" && i + 1 < argc) {
      diagnosisPath = argv[++i];
    } else if (arg == "--liberty" && i + 1 < argc) {
      libertyPaths.push_back(argv[++i]);
    } else if (designPath.empty()) {
      designPath = arg;
    }
  }

  if (!designPath.empty()) {
    std::string ext = designPath.size() >= 3 ? designPath.substr(designPath.rfind('.') + 1) : "";
    if (ext == "sv") {
      if (!libertyPaths.empty()) {
        std::cerr << "--liberty is only supported for .v designs, not .sv\n";
        return 1;
      }
      provider->loadSystemVerilog({designPath});
    } else if (ext == "v") {
      provider->loadVerilog({designPath}, libertyPaths);
    } else {
      provider->loadSNL(designPath); // assume SNL directory
    }
  }

  const bool drawRequested = exportOptions.focus || !exportOptions.traces.empty() ||
                             !exportOptions.equipotentials.empty();
  if (drawRequested && !exporting) {
    std::cerr << "--focus/--trace/--equipotential only apply with --export\n";
    return 1;
  }
  if (exporting) {
    if (designPath.empty()) {
      std::cerr << "--export needs a design to load\n";
      return 1;
    }
    exportOptions.diagnosisPath = diagnosisPath;
    return runHeadlessExport(provider, exportOptions);
  }

  SDL_Init(SDL_INIT_VIDEO);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);

  AppState state;
  state.window = SDL_CreateWindow(
    "najaeda Netlist Viewer (Standalone)",
    SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
    1280, 720,
    SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
  if (!state.window) {
    std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << "\n";
    return 1;
  }

  state.glContext = SDL_GL_CreateContext(state.window);
  SDL_GL_MakeCurrent(state.window, state.glContext);
  SDL_GL_SetSwapInterval(1); // vsync

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  setupFonts();
  ImGui_ImplSDL2_InitForOpenGL(state.window, state.glContext);
  ImGui_ImplOpenGL3_Init("#version 330 core");

  state.guiData  = new GUIData();

  state.provider = provider;
  setupProvider(state);

  // Load any requested diagnosis only after setupProvider() has completed
  // its synchronous root-load handshake (provider->start() at the end of
  // setupProvider fires it, which clears any previously-set diagnosis) --
  // otherwise the diagnosis we just loaded would be wiped immediately.
  if (!diagnosisPath.empty()) {
    loadDiagnosisFile(diagnosisPath);
  }

  while (appFrame(state)) {
    // loop until the window is closed
  }

  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplSDL2_Shutdown();
  ImGui::DestroyContext();
  SDL_GL_DeleteContext(state.glContext);
  SDL_DestroyWindow(state.window);
  SDL_Quit();
  return 0;
}

#endif // __EMSCRIPTEN__

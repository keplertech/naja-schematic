// Standalone desktop entry point.
// Compiled natively (no Emscripten); uses LocalSNLProvider to load netlists directly.
#ifndef __EMSCRIPTEN__

#include <SDL.h>
#include <SDL_opengl.h>
#include <imgui.h>
#include <backends/imgui_impl_sdl2.h>
#include <backends/imgui_impl_opengl3.h>
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>

#include "AppLogic.h"
#include "HeadlessExport.h"
#include "GUIData.h"
#include "LocalSNLProvider.h"
#include "Console.h"
#include "ViewOptions.h"

int main(int argc, char* argv[]) {
  // CLI usage: naja-schematic-standalone [<design>...] [--liberty <path>]... [--diagnosis <path>]
  //                                     [--style <path>]
  //                                     [--source-root <dir>]... [--source-map FROM=TO]...
  //                                     [--yosys-src] [--top <name>]
  //                                     [--instance-label name|model|both]
  //                                     [--sink-rows N] [--max-sinks N]
  //                                     [--group-by model|hierarchy|role]
  //                                     [--export <out.svg>] [--png <out.png> [--scale S]]
  //                                     [--focus <json>] [--trace <json>]... [--equipotential <json>]...
  // <design> is loaded by extension (.sv/.v/.vhd/.vhdl/otherwise-assumed-SNL-directory);
  // only VHDL takes several, all .vhd/.vhdl, in any order;
  // --top (.sv and VHDL) forces the top module/entity instead of inferring it;
  // --liberty defines the primitive cell library for a .v design (repeatable,
  // once per file) -- it is not supported for .sv or VHDL, since loadSystemVerilog()/
  // SNLSVConstructor and loadVHDL()/VHDLConstructor have no liberty hook;
  // --diagnosis pre-loads a diagnosis_response-shaped JSON so an external
  // caller (a script, or naja-agent's skill, after an edit-check cycle) can
  // open a fully annotated view in one command instead of requiring a human
  // to click through File > Open .../Load Diagnosis JSON... by hand.
  // --style pre-loads a style_response-shaped JSON (colors, widths, labels
  // on nets, instances and pins; see StyleStore.h), kept in --export too;
  // --yosys-src (.v only) decodes Yosys (* src *) attributes into source
  // origins, so Show Sources leads from a gate back to its RTL;
  // --source-map FROM=TO (repeatable) rewrites source paths recorded on
  // another machine (e.g. a synthesis server's) to start with TO instead;
  // --source-root adds a directory relative source file references (Show
  // Sources) are looked up in (repeatable, tried in order, then the current
  // directory);
  // --instance-label, --sink-rows, --max-sinks and --group-by set how the
  // schematic is drawn (ViewOptions.h; also in the View menu);
  // --export draws the schematic to an SVG file and exits, with no window,
  // --png to a PNG file, --scale pixels per SVG pixel (default 1); both
  // can be given (see HeadlessExport.h for what --focus/--trace/
  // --equipotential take).
  auto* provider = new LocalSNLProvider();

  std::vector<std::string> designPaths;
  std::string top;
  std::string diagnosisPath;
  std::string stylePath;
  std::vector<std::string> libertyPaths;
  bool yosysSrc = false;
  HeadlessExportOptions exportOptions;
  bool exporting = false;
  for (int i = 1; i < argc; ++i) {
    std::string arg(argv[i]);
    std::string error;
    if (arg == "--export" && i + 1 < argc) {
      exportOptions.outPath = argv[++i];
      exporting = true;
    } else if (arg == "--png" && i + 1 < argc) {
      exportOptions.pngPath = argv[++i];
      exporting = true;
    } else if (arg == "--scale" && i + 1 < argc) {
      const std::string text = argv[++i];
      char* end = nullptr;
      const double scale = std::strtod(text.c_str(), &end);
      if (end == text.c_str() || *end || !(scale > 0.0) || scale > 64.0) {
        std::cerr << "--scale expects a number in (0, 64], got " << text << "\n";
        return 1;
      }
      exportOptions.pngScale = float(scale);
    } else if ((arg == "--instance-label" || arg == "--sink-rows" || arg == "--max-sinks" ||
                arg == "--group-by") && i + 1 < argc) {
      // ViewOptions keys are the flags' names with '_' for '-'.
      std::string key = arg.substr(2);
      std::replace(key.begin(), key.end(), '-', '_');
      const std::string text = argv[++i];
      nlohmann::json value = text;
      if (key == "sink_rows" || key == "max_sinks") {
        char* end = nullptr;
        const long n = std::strtol(text.c_str(), &end, 10);
        value = (end != text.c_str() && !*end) ? nlohmann::json(n) : nlohmann::json(text);
      }
      if (!ViewOptions::current().apply({ { key, value } }, error)) {
        std::cerr << arg << ": " << error << "\n";
        return 1;
      }
    } else if (arg == "--focus" && i + 1 < argc) {
      exportOptions.focus = parseFocusArg(argv[++i], error);
      if (!exportOptions.focus) { std::cerr << error << "\n"; return 1; }
    } else if ((arg == "--trace" || arg == "--equipotential") && i + 1 < argc) {
      auto spec = parseTermArg(argv[++i], error);
      if (!spec) { std::cerr << arg << ": " << error << "\n"; return 1; }
      (arg == "--trace" ? exportOptions.traces : exportOptions.equipotentials).push_back(*spec);
    } else if (arg == "--diagnosis" && i + 1 < argc) {
      diagnosisPath = argv[++i];
    } else if (arg == "--style" && i + 1 < argc) {
      stylePath = argv[++i];
    } else if (arg == "--liberty" && i + 1 < argc) {
      libertyPaths.push_back(argv[++i]);
    } else if (arg == "--top" && i + 1 < argc) {
      top = argv[++i];
    } else if (arg == "--yosys-src") {
      yosysSrc = true;
    } else if (arg == "--source-map" && i + 1 < argc) {
      const std::string map = argv[++i];
      const auto eq = map.find('=');
      if (eq == std::string::npos || eq == 0) {
        std::cerr << "--source-map expects FROM=TO, got " << map << "\n";
        return 1;
      }
      provider->addSourceMap(map.substr(0, eq), map.substr(eq + 1));
    } else if (arg == "--source-root" && i + 1 < argc) {
      provider->addSourceRoot(argv[++i]);
    } else {
      designPaths.push_back(arg);
    }
  }

  auto extension = [](const std::string& path) {
    const auto dot = path.rfind('.');
    return dot == std::string::npos ? std::string() : path.substr(dot + 1);
  };
  auto isVHDL = [&](const std::string& path) {
    const std::string ext = extension(path);
    return ext == "vhd" || ext == "vhdl";
  };
  const std::string designPath = designPaths.empty() ? "" : designPaths.front();
  if (designPaths.size() > 1 && !std::all_of(designPaths.begin(), designPaths.end(), isVHDL)) {
    std::cerr << "only VHDL designs take several files (all .vhd/.vhdl)\n";
    return 1;
  }
  if (!top.empty() && (designPath.empty() || (extension(designPath) != "sv" && !isVHDL(designPath)))) {
    std::cerr << "--top only applies to .sv and VHDL designs\n";
    return 1;
  }

  if (!designPath.empty()) {
    std::string ext = extension(designPath);
    if (ext == "sv") {
      if (!libertyPaths.empty()) {
        std::cerr << "--liberty is only supported for .v designs, not .sv\n";
        return 1;
      }
      if (yosysSrc) {
        std::cerr << "--yosys-src is only supported for .v designs\n";
        return 1;
      }
      provider->loadSystemVerilog({designPath}, top);
    } else if (isVHDL(designPath)) {
      if (!libertyPaths.empty() || yosysSrc) {
        std::cerr << "--liberty and --yosys-src are only supported for .v designs, not VHDL\n";
        return 1;
      }
      provider->loadVHDL(designPaths, top);
    } else if (ext == "v") {
      provider->loadVerilog({designPath}, libertyPaths, yosysSrc);
    } else {
      provider->loadSNL(designPath); // assume SNL directory
    }
  }

  const bool drawRequested = exportOptions.focus || !exportOptions.traces.empty() ||
                             !exportOptions.equipotentials.empty();
  if (drawRequested && !exporting) {
    std::cerr << "--focus/--trace/--equipotential only apply with --export or --png\n";
    return 1;
  }
  if (exporting) {
    if (designPath.empty()) {
      std::cerr << "--export/--png needs a design to load\n";
      return 1;
    }
    exportOptions.diagnosisPath = diagnosisPath;
    exportOptions.stylePath     = stylePath;
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
  // Same for styles, cleared by a root load too.
  if (!stylePath.empty()) {
    loadStyleFile(stylePath);
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

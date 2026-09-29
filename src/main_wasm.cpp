// Browser / VSCode webview / notebook entry point.
// Compiled with Emscripten. Talks to the netlist backend through
// JsBridgeProvider when the host page supplies Module.najaSend (embedded
// mode, e.g. the naja_schematic Jupyter widget), otherwise through
// WebSocketProvider, at Module.najaWsUrl if set (the naja-schematic server's
// own page sets it) or ws://localhost:8081/ws.
#ifdef __EMSCRIPTEN__

#include <cstdlib>
#include <emscripten.h>
#include <emscripten/html5.h>
#include <SDL.h>
#include <SDL_opengl.h>
#include <imgui.h>
#include <backends/imgui_impl_sdl2.h>
#include <backends/imgui_impl_opengl3.h>

#include "AppLogic.h"
#include "GUIData.h"
#include "JsBridgeProvider.h"
#include "WebSocketProvider.h"
#include "Console.h"

static AppState g_state;

EM_JS_DEPS(naja_main, "$specialHTMLTargets,$stringToNewUTF8");

// SDL2 addresses its canvas by the "#canvas" selector. Map that selector to
// this module's own Module.canvas rather than whatever document.querySelector
// finds first, so several viewers can live on one page (one per notebook
// cell) -- specialHTMLTargets is per module instance.
EM_JS(void, naja_bind_canvas, (), {
  if (Module['canvas']) specialHTMLTargets['#canvas'] = Module['canvas'];
});

EM_JS(int, naja_is_embedded, (), {
  return Module['najaEmbedded'] ? 1 : 0;
});

// Caller frees; nullptr when the host didn't set Module.najaWsUrl.
EM_JS(char*, naja_ws_url, (), {
  const url = Module['najaWsUrl'];
  return url ? stringToNewUTF8(url) : 0;
});

static std::string webSocketUrl() {
  std::string url = "ws://localhost:8081/ws";
  if (char* hostUrl = naja_ws_url()) {
    url = hostUrl;
    free(hostUrl);
  }
  return url;
}

// Lets an embedding host stop the viewer when its view is torn down (e.g.
// the notebook output is cleared), as Module._naja_shutdown().
extern "C" EMSCRIPTEN_KEEPALIVE void naja_shutdown() {
  emscripten_cancel_main_loop();
}

// The <canvas> is sized by CSS (it fills the browser window / fullscreen, see
// shell_minimal.html). SDL follows window resizes on its own, but not every
// canvas size change (e.g. entering element fullscreen), so also track the CSS
// size here to keep SDL's window -- and thus ImGui's DisplaySize and glViewport
// -- in step with what is actually on screen.
static void syncWindowToCanvas() {
  double cssW = 0, cssH = 0;
  if (emscripten_get_element_css_size("#canvas", &cssW, &cssH) != EMSCRIPTEN_RESULT_SUCCESS ||
      cssW < 1 || cssH < 1)
    return;
  const int cw = (int)(cssW + 0.5), ch = (int)(cssH + 0.5);
  int ww = 0, wh = 0;
  SDL_GetWindowSize(g_state.window, &ww, &wh);
  if (ww != cw || wh != ch)
    SDL_SetWindowSize(g_state.window, cw, ch);
}

static void mainLoop() {
  syncWindowToCanvas();
  try {
    appFrame(g_state);
  } catch (const std::exception& e) {
    Console::Error("Exception in main loop: " + std::string(e.what()));
  }
}

int main() {
  naja_bind_canvas();
  if (naja_is_embedded()) {
    // Only take keystrokes while the canvas has focus: by default SDL listens
    // on the whole window, which would swallow typing in notebook cells.
    SDL_SetHint(SDL_HINT_EMSCRIPTEN_KEYBOARD_ELEMENT, "#canvas");
  }
  SDL_Init(SDL_INIT_VIDEO);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);

  g_state.window = SDL_CreateWindow(
    "najaeda Netlist Viewer",
    SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
    1280, 720,
    SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
  g_state.glContext = SDL_GL_CreateContext(g_state.window);
  SDL_GL_MakeCurrent(g_state.window, g_state.glContext);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  setupFonts();
  ImGui_ImplSDL2_InitForOpenGL(g_state.window, g_state.glContext);
  ImGui_ImplOpenGL3_Init("#version 300 es");

  g_state.guiData  = new GUIData();
  if (JsBridgeProvider::available())
    g_state.provider = new JsBridgeProvider();
  else
    g_state.provider = new WebSocketProvider(webSocketUrl());

  try {
    setupProvider(g_state);
  } catch (const std::exception& e) {
    Console::Error("Error setting up provider: " + std::string(e.what()));
  }

  emscripten_set_main_loop(mainLoop, 0, true);

  // Cleanup (unreachable under Emscripten, kept for completeness).
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplSDL2_Shutdown();
  ImGui::DestroyContext();
  SDL_GL_DeleteContext(g_state.glContext);
  SDL_DestroyWindow(g_state.window);
  SDL_Quit();
  return 0;
}

#endif // __EMSCRIPTEN__

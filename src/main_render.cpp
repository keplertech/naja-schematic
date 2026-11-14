// naja-schematic-render: the viewer with no window, no SDL/GL and no naja,
// for the Python package's export_svg()/`naja-schematic render` (shipped
// in the platform wheels). It's a viewer like the browser one, talking the
// same wire protocol to the Python host (naja_schematic/render.py), which
// answers from the live NLUniverse -- so designs whose primitives were
// built in Python (no Liberty, no file to reload them from) draw exactly
// as the notebook widget draws them.
//
// Transport: JSON lines on stdin/stdout, strictly alternating. Each message
// the viewer sends (a line on stdout: a request, or a notification such as
// svg_exported) gets exactly one line back on stdin: a JSON array of the
// messages answering it (replies and pushes, possibly none). When it has
// nothing left to do, the viewer sends {"request":"render_idle"}; the host
// answers with its next pushes (focus_instance, show_net, export_svg,
// design_changed to reset the view between drawings), or closes stdin to
// end the process. An export_svg push is answered, once its schematic has
// settled, by {"request":"svg_exported","svg":"..."} ("" if nothing is
// drawn). Likewise an export_png push ({"scale": S}) is answered by
// {"request":"png_exported","png":"<base64 PNG>"} ("" if nothing is drawn).
//
// stdout is the protocol channel only: fd 1 is pointed at stderr, so
// stray output (Console::Log, with NAJA_SCHEMATIC_RENDER_VERBOSE=1) can't
// land on it.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#define dup _dup
#define dup2 _dup2
#define fdopen _fdopen
#else
#include <unistd.h>
#endif

#include <nlohmann/json.hpp>

#include "Console.h"
#include "HeadlessView.h"
#include "INetlistProvider.h"
#include "PngWriter.h"

using json = nlohmann::json;

namespace {

class StdioProvider : public INetlistProvider {
  public:
    explicit StdioProvider(FILE* out) : out_(out) {}

    void send(const std::string& msg) override {
      if (!exchange(msg)) {
        std::cerr << "naja-schematic-render: the host closed the channel mid-request\n";
        std::exit(1);
      }
    }
    // Sends `msg` and delivers the answer. false: stdin was closed instead.
    bool exchange(const std::string& msg) {
      std::fputs(msg.c_str(), out_);
      std::fputc('\n', out_);
      std::fflush(out_);
      std::string line;
      if (!std::getline(std::cin, line)) return false;
      if (!line.empty() && line.back() == '\r') line.pop_back();  // a CRLF host
      json batch = json::parse(line, nullptr, /*allow_exceptions=*/false);
      if (!batch.is_array()) {
        std::cerr << "naja-schematic-render: expected a JSON array, got: " << line.substr(0, 200) << "\n";
        std::exit(1);
      }
      for (const auto& m : batch)
        if (onMessage_) onMessage_(m.dump());
      return true;
    }

    void on_open(std::function<void()> cb) override { onOpen_ = std::move(cb); }
    void on_message(std::function<void(const std::string&)> cb) override { onMessage_ = std::move(cb); }
    void on_close(std::function<void()>) override {}
    void on_error(std::function<void(const std::string&)>) override {}
    void start() override { if (onOpen_) onOpen_(); }

  private:
    FILE* out_;
    std::function<void()> onOpen_;
    std::function<void(const std::string&)> onMessage_;
};

} // namespace

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--version") == 0) {
      std::cout << NAJA_SCHEMATIC_VERSION_STRING << "\n";
      return 0;
    }
    std::cerr << "usage: naja-schematic-render [--version]\n"
                 "  a headless viewer driven over stdin/stdout by the naja_schematic Python\n"
                 "  package (naja_schematic.export_svg(), `naja-schematic render`)\n";
    return 2;
  }
  const char* verbose = std::getenv("NAJA_SCHEMATIC_RENDER_VERBOSE");
  Console::setVerbose(verbose && *verbose && std::strcmp(verbose, "0") != 0);

  // Keep the real stdout for the protocol; everything else writing to fd 1
  // goes to stderr.
  std::fflush(stdout);
#ifdef _WIN32
  // No CRLF translation on the protocol channel (or on the SVG it carries).
  _setmode(0, _O_BINARY);
  _setmode(1, _O_BINARY);
  FILE* protocolOut = fdopen(dup(1), "wb");
#else
  FILE* protocolOut = fdopen(dup(1), "w");
#endif
  if (!protocolOut) { std::perror("naja-schematic-render"); return 1; }
  dup2(2, 1);

  StdioProvider provider(protocolOut);
  HeadlessView view(&provider);   // load_root, answered before this returns
  AppState& state = view.state();
  for (;;) {
    view.settle();
    if (state.svgExportRequested) {
      state.svgExportRequested = false;
      json reply;
      reply["request"] = "svg_exported";
      reply["svg"]     = view.exportSvg();
      provider.send(reply.dump());
      continue;  // settle again: the reply may have carried pushes
    }
    if (state.pngExportRequested) {
      state.pngExportRequested = false;
      json reply;
      reply["request"] = "png_exported";
      reply["png"]     = PngWriter::base64(view.exportPng(state.pngScale));
      provider.send(reply.dump());
      continue;
    }
    if (!provider.exchange(R"({"request":"render_idle"})")) break;
  }
  return 0;
}

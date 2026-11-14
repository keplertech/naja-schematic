#pragma once
#ifndef __EMSCRIPTEN__

#include <string>

#include "AppLogic.h"

class INetlistProvider;

// The viewer with no window: an ImGui context with the app's font (so
// names are measured, and the layout sized, exactly as in the GUI) and no
// render backend, the usual message handling (setupProvider()) on
// `provider`, and the schematic panel alone, drawn into nothing. Shared by
// the native `--export` (HeadlessExport.cpp, over LocalSNLProvider) and
// naja-schematic-render (main_render.cpp, over stdin/stdout to the Python
// host). Expects a provider answering each request synchronously, inside
// send(). One at a time: the stores and the ImGui context are global.
class HeadlessView {
  public:
    explicit HeadlessView(INetlistProvider* provider);
    ~HeadlessView();
    HeadlessView(const HeadlessView&) = delete;
    HeadlessView& operator=(const HeadlessView&) = delete;

    AppState& state() { return state_; }

    // A few frames: the first lays the schematic out, and may ask for more
    // (a gate reached through one input loads its whole interface), which
    // the next ones draw.
    void settle(int frames = 4);

    // The whole sheet as SVG (SchematicView::exportSvg()); "" when nothing
    // is drawn.
    std::string exportSvg() const;
    // The whole sheet as PNG bytes, `scale` pixels per SVG pixel
    // (SchematicView::exportPng()); "" when nothing is drawn.
    std::string exportPng(float scale) const;

  private:
    AppState state_;
};

#endif // __EMSCRIPTEN__

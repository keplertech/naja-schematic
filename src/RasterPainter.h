#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "SchematicPainter.h"

// Paints the schematic into pixels with no GPU, for the PNG export: the
// drawing is recorded `scale` times larger into an ImDrawList -- the same
// anti-aliased tessellation ImGui draws on screen, and text from the font
// atlas baked at the scaled size, so it stays sharp -- then its triangles
// are filled in software over a white background. So a PNG is the 1x SVG
// export of the same view, `scale` times denser: every coordinate, stroke
// width, radius and font size is multiplied.
//
// Needs the current ImGui context, after at least one frame (the draw list
// shared data), and a font atlas the export may bake new sizes into: an
// atlas with ImGuiBackendFlags_RendererHasTextures (the GUI backends, and
// HeadlessView), or one already holding the sizes it needs.
class RasterPainter : public SchematicPainter {
  public:
    RasterPainter(ImVec2 size, float scale);
    ~RasterPainter() override;

    void AddLine(const ImVec2& a, const ImVec2& b, ImU32 col, float thickness) override;
    void AddPolyline(const ImVec2* pts, int n, ImU32 col, float thickness, ImDrawFlags flags) override;
    void AddConvexPolyFilled(const ImVec2* pts, int n, ImU32 col) override;
    void AddConcavePolyFilled(const ImVec2* pts, int n, ImU32 col) override;
    void AddRect(const ImVec2& rmin, const ImVec2& rmax, ImU32 col, float rounding,
                 ImDrawFlags flags, float thickness) override;
    void AddRectFilled(const ImVec2& rmin, const ImVec2& rmax, ImU32 col, float rounding) override;
    void AddCircle(const ImVec2& c, float r, ImU32 col, int segments, float thickness) override;
    void AddCircleFilled(const ImVec2& c, float r, ImU32 col) override;
    void AddText(ImFont* font, float fontSize, const ImVec2& pos, ImU32 col, const char* text) override;
    void PushClipRect(const ImVec2& rmin, const ImVec2& rmax) override;
    void PopClipRect() override;

    int width() const { return width_; }
    int height() const { return height_; }

    // Fills the recorded triangles into RGB pixels (width*height*3, rows
    // from the top) over white.
    std::vector<uint8_t> rasterize() const;
    // rasterize(), as a PNG file's bytes.
    std::string png() const;

  private:
    ImVec2 at(const ImVec2& p) const { return ImVec2(p.x * scale_, p.y * scale_); }
    std::vector<ImVec2> scaled(const ImVec2* pts, int n) const;

    float                       scale_;
    int                         width_, height_;
    std::unique_ptr<ImDrawList> dl_;
};

// Fills `dl`'s triangles into `rgb` (width*height*3, opaque), blending each
// over what's there, as ImGui's GPU backends do: vertex color times the
// bilinearly sampled texture, clipped to each command's clip rectangle.
// Exposed for tests.
void rasterizeDrawList(const ImDrawList& dl, std::vector<uint8_t>& rgb, int width, int height);

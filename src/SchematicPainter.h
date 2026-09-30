#pragma once

#include <sstream>
#include <string>
#include <imgui.h>

// What SchematicView draws with: the subset of ImDrawList it uses, with the
// same names and argument conventions (screen-space points, ImU32 colors,
// text placed by its top-left corner), so the draw code reads the same
// whichever backend it paints on. ImDrawListPainter paints on screen;
// SvgPainter writes the same drawing as an SVG document (schematic export).
class SchematicPainter {
public:
    virtual ~SchematicPainter() = default;

    virtual void AddLine(const ImVec2& a, const ImVec2& b, ImU32 col, float thickness = 1.0f) = 0;
    virtual void AddPolyline(const ImVec2* pts, int n, ImU32 col, float thickness,
                             ImDrawFlags flags) = 0;
    virtual void AddConvexPolyFilled(const ImVec2* pts, int n, ImU32 col) = 0;
    virtual void AddConcavePolyFilled(const ImVec2* pts, int n, ImU32 col) = 0;
    // (rounding, flags, thickness): the pre-1.92.8 ImDrawList order the draw
    // code was written against.
    virtual void AddRect(const ImVec2& rmin, const ImVec2& rmax, ImU32 col, float rounding = 0.0f,
                         ImDrawFlags flags = 0, float thickness = 1.0f) = 0;
    virtual void AddRectFilled(const ImVec2& rmin, const ImVec2& rmax, ImU32 col,
                               float rounding = 0.0f) = 0;
    virtual void AddCircle(const ImVec2& c, float r, ImU32 col, int segments = 0,
                           float thickness = 1.0f) = 0;
    virtual void AddCircleFilled(const ImVec2& c, float r, ImU32 col) = 0;
    virtual void AddText(ImFont* font, float fontSize, const ImVec2& pos, ImU32 col,
                         const char* text) = 0;
    virtual void PushClipRect(const ImVec2& rmin, const ImVec2& rmax) = 0;
    virtual void PopClipRect() = 0;
};

class ImDrawListPainter : public SchematicPainter {
public:
    explicit ImDrawListPainter(ImDrawList* dl) : dl_(dl) {}

    void AddLine(const ImVec2& a, const ImVec2& b, ImU32 col, float thickness) override {
        dl_->AddLine(a, b, col, thickness);
    }
    void AddPolyline(const ImVec2* pts, int n, ImU32 col, float thickness, ImDrawFlags flags) override {
        dl_->AddPolyline(pts, n, col, thickness, flags);
    }
    void AddConvexPolyFilled(const ImVec2* pts, int n, ImU32 col) override {
        dl_->AddConvexPolyFilled(pts, n, col);
    }
    void AddConcavePolyFilled(const ImVec2* pts, int n, ImU32 col) override {
        dl_->AddConcavePolyFilled(pts, n, col);
    }
    void AddRect(const ImVec2& rmin, const ImVec2& rmax, ImU32 col, float rounding,
                 ImDrawFlags flags, float thickness) override {
        dl_->AddRect(rmin, rmax, col, rounding, thickness, flags);
    }
    void AddRectFilled(const ImVec2& rmin, const ImVec2& rmax, ImU32 col, float rounding) override {
        dl_->AddRectFilled(rmin, rmax, col, rounding);
    }
    void AddCircle(const ImVec2& c, float r, ImU32 col, int segments, float thickness) override {
        dl_->AddCircle(c, r, col, segments, thickness);
    }
    void AddCircleFilled(const ImVec2& c, float r, ImU32 col) override {
        dl_->AddCircleFilled(c, r, col);
    }
    void AddText(ImFont* font, float fontSize, const ImVec2& pos, ImU32 col, const char* text) override {
        dl_->AddText(font, fontSize, pos, col, text);
    }
    void PushClipRect(const ImVec2& rmin, const ImVec2& rmax) override {
        dl_->PushClipRect(rmin, rmax, true);
    }
    void PopClipRect() override { dl_->PopClipRect(); }

private:
    ImDrawList* dl_;
};

// Writes an SVG document of `size` (user units = world units at 1x, i.e.
// pixels). Shapes stay vector paths and text stays text: selectable and
// searchable in a browser or a PDF printed from it. Text is measured with
// the ImGui font the layout was sized with and pinned to that width
// (textLength), so a viewer substituting another font never overflows a box.
// Clipping is ignored: an export draws the whole sheet.
class SvgPainter : public SchematicPainter {
public:
    explicit SvgPainter(ImVec2 size);

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
    void PushClipRect(const ImVec2&, const ImVec2&) override {}
    void PopClipRect() override {}

    // The complete document (call once, after drawing).
    std::string finish();

private:
    void points(const ImVec2* pts, int n);
    std::ostringstream out_;
};

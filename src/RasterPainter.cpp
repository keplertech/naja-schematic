#include "RasterPainter.h"

#include <algorithm>
#include <cmath>

#include <imgui_internal.h>

#include "PngWriter.h"

RasterPainter::RasterPainter(ImVec2 size, float scale)
    : scale_(scale),
      width_(std::max(1, int(std::ceil(size.x * scale)))),
      height_(std::max(1, int(std::ceil(size.y * scale)))),
      dl_(std::make_unique<ImDrawList>(ImGui::GetDrawListSharedData())) {
  dl_->_ResetForNewFrame();
  dl_->_SetPixelDensity(1.0f);  // the scale is applied to the coordinates here
  // Anti-aliased by geometry (a fringe), not by sampling the atlas's baked
  // line texture: the same look, with no dependence on where the atlas
  // keeps those lines.
  dl_->Flags &= ~ImDrawListFlags_AntiAliasedLinesUseTex;
  dl_->PushClipRect(ImVec2(0.0f, 0.0f), ImVec2(float(width_), float(height_)));
  dl_->PushTexture(ImGui::GetIO().Fonts->TexRef);
}

RasterPainter::~RasterPainter() = default;

std::vector<ImVec2> RasterPainter::scaled(const ImVec2* pts, int n) const {
  std::vector<ImVec2> out(size_t(std::max(n, 0)));
  for (int i = 0; i < n; ++i) out[size_t(i)] = at(pts[i]);
  return out;
}

void RasterPainter::AddLine(const ImVec2& a, const ImVec2& b, ImU32 col, float thickness) {
  dl_->AddLine(at(a), at(b), col, thickness * scale_);
}
void RasterPainter::AddPolyline(const ImVec2* pts, int n, ImU32 col, float thickness, ImDrawFlags flags) {
  const auto p = scaled(pts, n);
  dl_->AddPolyline(p.data(), n, col, thickness * scale_, flags);
}
void RasterPainter::AddConvexPolyFilled(const ImVec2* pts, int n, ImU32 col) {
  const auto p = scaled(pts, n);
  dl_->AddConvexPolyFilled(p.data(), n, col);
}
void RasterPainter::AddConcavePolyFilled(const ImVec2* pts, int n, ImU32 col) {
  const auto p = scaled(pts, n);
  dl_->AddConcavePolyFilled(p.data(), n, col);
}
void RasterPainter::AddRect(const ImVec2& rmin, const ImVec2& rmax, ImU32 col, float rounding,
                            ImDrawFlags flags, float thickness) {
  dl_->AddRect(at(rmin), at(rmax), col, rounding * scale_, thickness * scale_, flags);
}
void RasterPainter::AddRectFilled(const ImVec2& rmin, const ImVec2& rmax, ImU32 col, float rounding) {
  dl_->AddRectFilled(at(rmin), at(rmax), col, rounding * scale_);
}
void RasterPainter::AddCircle(const ImVec2& c, float r, ImU32 col, int segments, float thickness) {
  dl_->AddCircle(at(c), r * scale_, col, segments, thickness * scale_);
}
void RasterPainter::AddCircleFilled(const ImVec2& c, float r, ImU32 col) {
  dl_->AddCircleFilled(at(c), r * scale_, col);
}
void RasterPainter::AddText(ImFont* font, float fontSize, const ImVec2& pos, ImU32 col, const char* text) {
  dl_->AddText(font, fontSize * scale_, at(pos), col, text);
}
void RasterPainter::PushClipRect(const ImVec2& rmin, const ImVec2& rmax) {
  dl_->PushClipRect(at(rmin), at(rmax), true);
}
void RasterPainter::PopClipRect() { dl_->PopClipRect(); }

std::vector<uint8_t> RasterPainter::rasterize() const {
  std::vector<uint8_t> rgb(size_t(width_) * size_t(height_) * 3, 255);
  rasterizeDrawList(*dl_, rgb, width_, height_);
  return rgb;
}

std::string RasterPainter::png() const { return PngWriter::encodeRgb(rasterize(), width_, height_); }

// ---------------------------------------------------------------------------
// Software rasterizer
// ---------------------------------------------------------------------------
namespace {

struct Texture {
  const unsigned char* pixels = nullptr;
  int                  width = 0, height = 0, bpp = 4;  // RGBA32 or Alpha8
};

Texture textureOf(const ImDrawCmd& cmd) {
  Texture t;
  const ImTextureData* tex = cmd.TexRef._TexData;
  if (!tex || !tex->Pixels) return t;
  t.pixels = tex->Pixels;
  t.width  = tex->Width;
  t.height = tex->Height;
  t.bpp    = tex->BytesPerPixel;
  return t;
}

// RGBA in [0, 1], bilinear, clamped to the edges (GL_LINEAR + CLAMP_TO_EDGE).
void sample(const Texture& t, float u, float v, float out[4]) {
  if (!t.pixels) { out[0] = out[1] = out[2] = out[3] = 1.0f; return; }
  const float x = u * float(t.width) - 0.5f, y = v * float(t.height) - 0.5f;
  const int   x0 = int(std::floor(x)), y0 = int(std::floor(y));
  const float fx = x - float(x0), fy = y - float(y0);
  float acc[4] = { 0, 0, 0, 0 };
  for (int dy = 0; dy < 2; ++dy) {
    for (int dx = 0; dx < 2; ++dx) {
      const int   px = std::clamp(x0 + dx, 0, t.width - 1), py = std::clamp(y0 + dy, 0, t.height - 1);
      const float wgt = (dx ? fx : 1.0f - fx) * (dy ? fy : 1.0f - fy);
      const unsigned char* p = t.pixels + (size_t(py) * size_t(t.width) + size_t(px)) * size_t(t.bpp);
      if (t.bpp == 1) {
        acc[0] += wgt; acc[1] += wgt; acc[2] += wgt; acc[3] += wgt * float(p[0]) / 255.0f;
      } else {
        for (int c = 0; c < 4; ++c) acc[c] += wgt * float(p[c]) / 255.0f;
      }
    }
  }
  for (int c = 0; c < 4; ++c) out[c] = acc[c];
}

float channel(ImU32 col, int shift) { return float((col >> shift) & 0xFF) / 255.0f; }

// Twice the signed area of (p, q, r): > 0 when r is on the same side of
// p -> q for every edge of a triangle wound like the normalized ones below.
float edge(const ImVec2& p, const ImVec2& q, float rx, float ry) {
  return (q.x - p.x) * (ry - p.y) - (q.y - p.y) * (rx - p.x);
}

// A pixel center exactly on an edge belongs to one of the two triangles
// sharing it (the "top-left" fill rule): the triangles are wound the same
// way, so they run along a shared edge in opposite directions, and only one
// direction passes this test. Otherwise translucent seams would be filled
// twice and show darker.
bool ownsEdge(const ImVec2& p, const ImVec2& q) {
  const float dy = q.y - p.y, dx = q.x - p.x;
  return dy > 0.0f || (dy == 0.0f && dx < 0.0f);
}

void fillTriangle(const ImDrawVert& a, const ImDrawVert& b0, const ImDrawVert& c0, const Texture& tex,
                  const ImVec4& clip, std::vector<uint8_t>& rgb, int width, int height) {
  float area = edge(a.pos, b0.pos, c0.pos.x, c0.pos.y);
  if (std::fabs(area) < 1e-12f) return;
  // One winding for every triangle (see ownsEdge).
  const ImDrawVert& b = area > 0.0f ? b0 : c0;
  const ImDrawVert& c = area > 0.0f ? c0 : b0;
  area = std::fabs(area);
  const float minX = std::max({ clip.x, 0.0f, std::min({ a.pos.x, b.pos.x, c.pos.x }) });
  const float minY = std::max({ clip.y, 0.0f, std::min({ a.pos.y, b.pos.y, c.pos.y }) });
  const float maxX = std::min({ clip.z, float(width), std::max({ a.pos.x, b.pos.x, c.pos.x }) });
  const float maxY = std::min({ clip.w, float(height), std::max({ a.pos.y, b.pos.y, c.pos.y }) });
  if (minX >= maxX || minY >= maxY) return;
  const int x0 = std::max(0, int(std::floor(minX))), x1 = std::min(width, int(std::ceil(maxX)));
  const int y0 = std::max(0, int(std::floor(minY))), y1 = std::min(height, int(std::ceil(maxY)));
  const bool ownBC = ownsEdge(b.pos, c.pos), ownCA = ownsEdge(c.pos, a.pos), ownAB = ownsEdge(a.pos, b.pos);

  // A texture-free triangle (ImGui's white pixel) samples one color:
  // skip the lookups when all three vertices share one UV.
  const bool flatUv = a.uv.x == b.uv.x && a.uv.x == c.uv.x && a.uv.y == b.uv.y && a.uv.y == c.uv.y;
  float flatTex[4];
  sample(tex, a.uv.x, a.uv.y, flatTex);

  const ImDrawVert* v[3] = { &a, &b, &c };
  float col[3][4];
  for (int i = 0; i < 3; ++i) {
    col[i][0] = channel(v[i]->col, IM_COL32_R_SHIFT);
    col[i][1] = channel(v[i]->col, IM_COL32_G_SHIFT);
    col[i][2] = channel(v[i]->col, IM_COL32_B_SHIFT);
    col[i][3] = channel(v[i]->col, IM_COL32_A_SHIFT);
  }
  const float inv = 1.0f / area;
  for (int y = y0; y < y1; ++y) {
    const float py = float(y) + 0.5f;
    if (py < clip.y || py >= clip.w) continue;
    for (int x = x0; x < x1; ++x) {
      const float px = float(x) + 0.5f;
      if (px < clip.x || px >= clip.z) continue;
      const float e0 = edge(b.pos, c.pos, px, py);  // weight of a
      const float e1 = edge(c.pos, a.pos, px, py);  // weight of b
      const float e2 = edge(a.pos, b.pos, px, py);  // weight of c
      if (e0 < 0.0f || e1 < 0.0f || e2 < 0.0f) continue;
      if ((e0 == 0.0f && !ownBC) || (e1 == 0.0f && !ownCA) || (e2 == 0.0f && !ownAB)) continue;
      const float w0 = e0 * inv, w1 = e1 * inv, w2 = e2 * inv;
      float t[4];
      if (flatUv) {
        t[0] = flatTex[0]; t[1] = flatTex[1]; t[2] = flatTex[2]; t[3] = flatTex[3];
      } else {
        sample(tex, w0 * a.uv.x + w1 * b.uv.x + w2 * c.uv.x, w0 * a.uv.y + w1 * b.uv.y + w2 * c.uv.y, t);
      }
      float s[4];
      for (int k = 0; k < 4; ++k) s[k] = (w0 * col[0][k] + w1 * col[1][k] + w2 * col[2][k]) * t[k];
      const float alpha = std::clamp(s[3], 0.0f, 1.0f);
      if (alpha <= 0.0f) continue;
      uint8_t* d = rgb.data() + (size_t(y) * size_t(width) + size_t(x)) * 3;
      for (int k = 0; k < 3; ++k) {
        const float val = std::clamp(s[k], 0.0f, 1.0f) * alpha + float(d[k]) / 255.0f * (1.0f - alpha);
        d[k] = uint8_t(std::lround(val * 255.0f));
      }
    }
  }
}

} // namespace

void rasterizeDrawList(const ImDrawList& dl, std::vector<uint8_t>& rgb, int width, int height) {
  for (const ImDrawCmd& cmd : dl.CmdBuffer) {
    if (cmd.UserCallback || cmd.ElemCount == 0) continue;
    const Texture tex = textureOf(cmd);
    for (unsigned i = 0; i + 2 < cmd.ElemCount; i += 3) {
      const ImDrawIdx* idx = dl.IdxBuffer.Data + cmd.IdxOffset + i;
      const ImDrawVert& a = dl.VtxBuffer[int(cmd.VtxOffset + idx[0])];
      const ImDrawVert& b = dl.VtxBuffer[int(cmd.VtxOffset + idx[1])];
      const ImDrawVert& c = dl.VtxBuffer[int(cmd.VtxOffset + idx[2])];
      fillTriangle(a, b, c, tex, cmd.ClipRect, rgb, width, height);
    }
  }
}

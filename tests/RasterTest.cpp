// The PNG export's pieces: the deflate/PNG encoder (decoded back here with a
// minimal inflate for fixed-Huffman blocks) and the software rasterizer of
// ImGui draw lists (RasterPainter).
#include <gtest/gtest.h>

#include <cstring>
#include <imgui.h>

#include "PngWriter.h"
#include "RasterPainter.h"

namespace {

// --- a minimal inflate: stored and fixed-Huffman blocks (what PngWriter emits)
class BitReader {
 public:
  BitReader(const uint8_t* d, size_t n) : d_(d), n_(n) {}
  uint32_t bits(int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; ++i, ++pos_) {
      if (pos_ / 8 >= n_) throw std::runtime_error("past end");
      v |= uint32_t((d_[pos_ / 8] >> (pos_ % 8)) & 1u) << i;
    }
    return v;
  }
  uint32_t msb(int n) {  // a Huffman code, most significant bit first
    uint32_t v = 0;
    for (int i = 0; i < n; ++i) v = (v << 1) | bits(1);
    return v;
  }
 private:
  const uint8_t* d_;
  size_t n_, pos_ = 0;
};

int fixedSymbol(BitReader& r) {
  uint32_t c = r.msb(7);
  if (c <= 0x17) return int(c) + 256;
  c = (c << 1) | r.bits(1);
  if (c >= 0x30 && c <= 0xBF) return int(c - 0x30);
  if (c >= 0xC0 && c <= 0xC7) return int(c - 0xC0) + 280;
  c = (c << 1) | r.bits(1);
  return int(c - 0x190) + 144;
}

std::vector<uint8_t> inflateZlib(const std::string& z) {
  static const int lenBase[] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
  static const int lenExtra[] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                  3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
  static const int distBase[] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385,
                                  513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
  static const int distExtra[] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                   7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
  const auto* d = reinterpret_cast<const uint8_t*>(z.data());
  EXPECT_EQ((d[0] * 256 + d[1]) % 31, 0);
  BitReader r(d + 2, z.size() - 6);
  std::vector<uint8_t> out;
  for (bool last = false; !last;) {
    last = r.bits(1);
    const uint32_t type = r.bits(2);
    EXPECT_EQ(type, 1u);
    for (;;) {
      const int s = fixedSymbol(r);
      if (s < 256) { out.push_back(uint8_t(s)); continue; }
      if (s == 256) break;
      const int li = s - 257;
      const int len = lenBase[li] + int(r.bits(lenExtra[li]));
      const int di = int(r.msb(5));
      const int dist = distBase[di] + int(r.bits(distExtra[di]));
      for (int k = 0; k < len; ++k) out.push_back(out[out.size() - size_t(dist)]);
    }
  }
  uint32_t a = 1, b = 0;
  for (uint8_t v : out) { a = (a + v) % 65521; b = (b + a) % 65521; }
  const size_t n = z.size();
  EXPECT_EQ((b << 16) | a, (uint32_t(d[n - 4]) << 24) | (uint32_t(d[n - 3]) << 16) |
                               (uint32_t(d[n - 2]) << 8) | uint32_t(d[n - 1]));
  return out;
}

uint32_t be32(const std::string& s, size_t at) {
  return (uint32_t(uint8_t(s[at])) << 24) | (uint32_t(uint8_t(s[at + 1])) << 16) |
         (uint32_t(uint8_t(s[at + 2])) << 8) | uint32_t(uint8_t(s[at + 3]));
}

// Decodes an RGB8 PNG as PngWriter writes it (one IDAT).
std::vector<uint8_t> decodePng(const std::string& png, int& w, int& h) {
  EXPECT_EQ(png.substr(0, 8), std::string("\x89PNG\r\n\x1a\n", 8));
  size_t at = 8;
  std::string idat;
  while (at + 8 <= png.size()) {
    const uint32_t len = be32(png, at);
    const std::string type = png.substr(at + 4, 4);
    if (type == "IHDR") { w = int(be32(png, at + 8)); h = int(be32(png, at + 12)); }
    if (type == "IDAT") idat += png.substr(at + 8, len);
    at += 12 + len;
  }
  const auto filtered = inflateZlib(idat);
  const size_t stride = size_t(w) * 3;
  EXPECT_EQ(filtered.size(), (stride + 1) * size_t(h));
  std::vector<uint8_t> rgb(stride * size_t(h));
  for (int y = 0; y < h; ++y) {
    const uint8_t type = filtered[size_t(y) * (stride + 1)];
    const uint8_t* in = &filtered[size_t(y) * (stride + 1) + 1];
    uint8_t* row = &rgb[size_t(y) * stride];
    const uint8_t* up = y ? row - stride : nullptr;
    for (size_t x = 0; x < stride; ++x) {
      const int a = x >= 3 ? row[x - 3] : 0, b = up ? up[x] : 0, c = (up && x >= 3) ? up[x - 3] : 0;
      int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
      const int pred = type == 1 ? a : type == 2 ? b : type == 3 ? (a + b) / 2
                     : type == 4 ? (pa <= pb && pa <= pc ? a : pb <= pc ? b : c) : 0;
      row[x] = uint8_t(in[x] + pred);
    }
  }
  return rgb;
}

} // namespace

TEST(PngWriter, RoundTripsThroughInflate) {
  std::vector<uint8_t> data;
  for (int i = 0; i < 70000; ++i) data.push_back(uint8_t((i % 300 < 200) ? 0 : (i * 7) & 0xFF));
  const std::string z = PngWriter::zlibCompress(data.data(), data.size());
  EXPECT_LT(z.size(), data.size() / 2);
  EXPECT_EQ(inflateZlib(z), data);
}

TEST(PngWriter, EncodesRgbImages) {
  const int w = 37, h = 23;
  std::vector<uint8_t> rgb(size_t(w) * h * 3);
  for (size_t i = 0; i < rgb.size(); ++i) rgb[i] = uint8_t((i * 31) ^ (i >> 5));
  const std::string png = PngWriter::encodeRgb(rgb, w, h);
  int dw = 0, dh = 0;
  EXPECT_EQ(decodePng(png, dw, dh), rgb);
  EXPECT_EQ(dw, w);
  EXPECT_EQ(dh, h);
  EXPECT_EQ(PngWriter::encodeRgb(rgb, w + 1, h), "");
}

TEST(PngWriter, Base64) {
  EXPECT_EQ(PngWriter::base64(""), "");
  EXPECT_EQ(PngWriter::base64("f"), "Zg==");
  EXPECT_EQ(PngWriter::base64("fo"), "Zm8=");
  EXPECT_EQ(PngWriter::base64("foobar"), "Zm9vYmFy");
}

namespace {
class Raster : public ::testing::Test {
 protected:
  void SetUp() override {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(400.f, 300.f);
    io.DeltaTime   = 1.f / 60.f;
    io.IniFilename = nullptr;
    unsigned char* px; int w, h;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    ImGui::NewFrame();
    ImGui::Render();
  }
  void TearDown() override { ImGui::DestroyContext(); }

  static const uint8_t* at(const std::vector<uint8_t>& rgb, int w, int x, int y) {
    return &rgb[(size_t(y) * size_t(w) + size_t(x)) * 3];
  }
};
} // namespace

TEST_F(Raster, FillsShapesScaled) {
  RasterPainter p(ImVec2(50.f, 40.f), 2.0f);
  EXPECT_EQ(p.width(), 100);
  EXPECT_EQ(p.height(), 80);
  p.AddRectFilled(ImVec2(10.f, 10.f), ImVec2(20.f, 20.f), IM_COL32(255, 0, 0, 255), 0.f);
  const auto rgb = p.rasterize();
  const uint8_t* inside = at(rgb, 100, 30, 30);   // world (15, 15)
  EXPECT_EQ(inside[0], 255);
  EXPECT_EQ(inside[1], 0);
  const uint8_t* outside = at(rgb, 100, 60, 60);  // world (30, 30): background
  EXPECT_EQ(outside[0], 255);
  EXPECT_EQ(outside[1], 255);
  EXPECT_EQ(outside[2], 255);
}

TEST_F(Raster, StrokesLinesAndOutlines) {
  RasterPainter p(ImVec2(60.f, 40.f), 1.0f);
  p.AddLine(ImVec2(5.f, 10.f), ImVec2(55.f, 10.f), IM_COL32(0, 0, 255, 255), 2.0f);
  p.AddRect(ImVec2(10.f, 20.f), ImVec2(40.f, 35.f), IM_COL32(0, 0, 0, 255), 0.f, 0, 1.25f);
  const auto rgb = p.rasterize();
  // The line: ImGui centers it on y + 0.5, so it covers rows 9 and 10.
  const uint8_t* onLine = at(rgb, 60, 30, 10);
  EXPECT_LT(onLine[0], 80);
  EXPECT_GT(onLine[2], 200);
  // The outline's left edge, and the untouched inside.
  const uint8_t* edge = at(rgb, 60, 10, 27);
  EXPECT_LT(edge[0], 128);
  const uint8_t* inside = at(rgb, 60, 25, 27);
  EXPECT_EQ(inside[0], 255);
}

TEST_F(Raster, DrawsTextFromTheAtlas) {
  RasterPainter p(ImVec2(80.f, 30.f), 1.0f);
  p.AddText(ImGui::GetFont(), 13.f, ImVec2(4.f, 4.f), IM_COL32(0, 0, 0, 255), "MMMM");
  const auto rgb = p.rasterize();
  int dark = 0;
  for (int y = 0; y < 30; ++y)
    for (int x = 0; x < 80; ++x)
      if (at(rgb, 80, x, y)[0] < 100) ++dark;
  EXPECT_GT(dark, 40);
}

TEST_F(Raster, PngDecodesToTheRaster) {
  RasterPainter p(ImVec2(30.f, 20.f), 1.5f);
  p.AddCircleFilled(ImVec2(15.f, 10.f), 6.f, IM_COL32(20, 140, 60, 255));
  int w = 0, h = 0;
  EXPECT_EQ(decodePng(p.png(), w, h), p.rasterize());
  EXPECT_EQ(w, 45);
  EXPECT_EQ(h, 30);
}

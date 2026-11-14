#include "PngWriter.h"

#include <array>
#include <cstdlib>

namespace PngWriter {
namespace {

// ---------------------------------------------------------------------------
// Deflate (RFC 1951), one block with the fixed Huffman codes
// ---------------------------------------------------------------------------
class BitWriter {
  public:
    explicit BitWriter(std::string& out) : out_(out) {}
    // `n` bits of `value`, least significant first (deflate's bit order).
    void bits(uint32_t value, int n) {
      acc_ |= uint64_t(value) << count_;
      count_ += n;
      while (count_ >= 8) {
        out_.push_back(char(acc_ & 0xFF));
        acc_ >>= 8;
        count_ -= 8;
      }
    }
    // A Huffman code: written most significant bit first.
    void code(uint32_t code, int n) {
      uint32_t reversed = 0;
      for (int i = 0; i < n; ++i) reversed |= ((code >> i) & 1u) << (n - 1 - i);
      bits(reversed, n);
    }
    void flush() {
      if (count_ > 0) out_.push_back(char(acc_ & 0xFF));
      acc_ = 0;
      count_ = 0;
    }

  private:
    std::string& out_;
    uint64_t     acc_   = 0;
    int          count_ = 0;
};

void literal(BitWriter& w, int v) {  // 0..287, fixed literal/length code
  if (v < 144)      w.code(0x30 + v, 8);
  else if (v < 256) w.code(0x190 + (v - 144), 9);
  else if (v < 280) w.code(v - 256, 7);
  else              w.code(0xC0 + (v - 280), 8);
}

constexpr std::array<int, 29> kLenBase  = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                            35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
constexpr std::array<int, 29> kLenExtra = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                            3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
constexpr std::array<int, 30> kDistBase = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                            257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
                                            8193, 12289, 16385, 24577 };
constexpr std::array<int, 30> kDistExtra = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                             7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

void match(BitWriter& w, int length, int distance) {
  int li = 28;
  while (kLenBase[li] > length) --li;
  literal(w, 257 + li);
  if (kLenExtra[li]) w.bits(uint32_t(length - kLenBase[li]), kLenExtra[li]);
  int di = 29;
  while (kDistBase[di] > distance) --di;
  w.code(uint32_t(di), 5);
  if (kDistExtra[di]) w.bits(uint32_t(distance - kDistBase[di]), kDistExtra[di]);
}

void deflateFixed(const uint8_t* data, size_t size, std::string& out) {
  constexpr int    kHashBits  = 15;
  constexpr size_t kWindow    = 32768;
  constexpr int    kMaxChain  = 48;
  constexpr int    kMinMatch  = 3;
  constexpr int    kMaxMatch  = 258;
  std::vector<int64_t> head(size_t(1) << kHashBits, -1);
  std::vector<int64_t> prev(kWindow, -1);
  auto hash = [&](size_t i) {
    uint32_t h = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | data[i + 2];
    return (h * 2654435761u) >> (32 - kHashBits);
  };
  auto insert = [&](size_t i) {
    if (i + kMinMatch > size) return;
    const uint32_t h = hash(i);
    prev[i % kWindow] = head[h];
    head[h] = int64_t(i);
  };

  BitWriter w(out);
  w.bits(1, 1);  // BFINAL
  w.bits(1, 2);  // BTYPE = 01, fixed Huffman codes
  size_t i = 0;
  while (i < size) {
    int bestLen = 0, bestDist = 0;
    if (i + kMinMatch <= size) {
      const size_t maxLen = std::min<size_t>(kMaxMatch, size - i);
      int64_t cand = head[hash(i)];
      for (int chain = 0; cand >= 0 && chain < kMaxChain; ++chain) {
        const size_t c = size_t(cand);
        if (i - c > kWindow - 1) break;
        if (data[c + size_t(bestLen)] == data[i + size_t(bestLen)]) {
          size_t len = 0;
          while (len < maxLen && data[c + len] == data[i + len]) ++len;
          if (int(len) > bestLen) {
            bestLen  = int(len);
            bestDist = int(i - c);
            if (len == maxLen) break;
          }
        }
        const int64_t next = prev[c % kWindow];
        if (next >= cand) break;  // a slot reused by a newer position
        cand = next;
      }
    }
    if (bestLen >= kMinMatch) {
      match(w, bestLen, bestDist);
      for (int k = 0; k < bestLen; ++k) insert(i + size_t(k));
      i += size_t(bestLen);
    } else {
      literal(w, data[i]);
      insert(i);
      ++i;
    }
  }
  literal(w, 256);  // end of block
  w.flush();
}

uint32_t adler32(const uint8_t* data, size_t size) {
  uint32_t a = 1, b = 0;
  for (size_t i = 0; i < size; ++i) {
    a = (a + data[i]) % 65521u;
    b = (b + a) % 65521u;
  }
  return (b << 16) | a;
}

// ---------------------------------------------------------------------------
// PNG
// ---------------------------------------------------------------------------
uint32_t crc32(const std::string& bytes, size_t from) {
  static const std::array<uint32_t, 256> table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t n = 0; n < 256; ++n) {
      uint32_t c = n;
      for (int k = 0; k < 8; ++k) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t[n] = c;
    }
    return t;
  }();
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = from; i < bytes.size(); ++i) c = table[(c ^ uint8_t(bytes[i])) & 0xFFu] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

void be32(std::string& out, uint32_t v) {
  out.push_back(char(v >> 24));
  out.push_back(char(v >> 16));
  out.push_back(char(v >> 8));
  out.push_back(char(v));
}

void chunk(std::string& out, const char* type, const std::string& data) {
  be32(out, uint32_t(data.size()));
  const size_t start = out.size();
  out.append(type, 4);
  out += data;
  be32(out, crc32(out, start));
}

uint8_t paeth(int a, int b, int c) {
  const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
  if (pa <= pb && pa <= pc) return uint8_t(a);
  return uint8_t(pb <= pc ? b : c);
}

} // namespace

std::string base64(const std::string& bytes) {
  static const char* kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((bytes.size() + 2) / 3 * 4);
  for (size_t i = 0; i < bytes.size(); i += 3) {
    uint32_t v = uint32_t(uint8_t(bytes[i])) << 16;
    if (i + 1 < bytes.size()) v |= uint32_t(uint8_t(bytes[i + 1])) << 8;
    if (i + 2 < bytes.size()) v |= uint32_t(uint8_t(bytes[i + 2]));
    out.push_back(kAlphabet[(v >> 18) & 63]);
    out.push_back(kAlphabet[(v >> 12) & 63]);
    out.push_back(i + 1 < bytes.size() ? kAlphabet[(v >> 6) & 63] : '=');
    out.push_back(i + 2 < bytes.size() ? kAlphabet[v & 63] : '=');
  }
  return out;
}

std::string zlibCompress(const uint8_t* data, size_t size) {
  std::string out;
  out.push_back(char(0x78));  // deflate, 32K window
  out.push_back(char(0x01));  // no dictionary; (0x7801 % 31 == 0)
  deflateFixed(data, size, out);
  be32(out, adler32(data, size));
  return out;
}

std::string encodeRgb(const std::vector<uint8_t>& rgb, int width, int height) {
  if (width <= 0 || height <= 0 || rgb.size() != size_t(width) * size_t(height) * 3) return "";
  const size_t stride = size_t(width) * 3;
  // Each row filtered (filter-type byte first) with whichever of the five
  // filters gives the smallest sum of absolute values: libpng's heuristic.
  std::vector<uint8_t> filtered;
  filtered.reserve((stride + 1) * size_t(height));
  std::vector<uint8_t> candidate(stride), best(stride);
  const std::vector<uint8_t> zeros(stride, 0);
  for (int y = 0; y < height; ++y) {
    const uint8_t* row = rgb.data() + size_t(y) * stride;
    const uint8_t* up  = y > 0 ? row - stride : zeros.data();
    long bestScore = -1;
    uint8_t bestType = 0;
    for (uint8_t type = 0; type < 5; ++type) {
      long score = 0;
      for (size_t x = 0; x < stride; ++x) {
        const int a = x >= 3 ? row[x - 3] : 0, b = up[x], c = x >= 3 ? up[x - 3] : 0;
        uint8_t v = row[x];
        switch (type) {
          case 1: v = uint8_t(v - a); break;
          case 2: v = uint8_t(v - b); break;
          case 3: v = uint8_t(v - ((a + b) >> 1)); break;
          case 4: v = uint8_t(v - paeth(a, b, c)); break;
          default: break;
        }
        candidate[x] = v;
        score += v < 128 ? v : 256 - v;
      }
      if (bestScore < 0 || score < bestScore) {
        bestScore = score;
        bestType  = type;
        best.swap(candidate);
      }
    }
    filtered.push_back(bestType);
    filtered.insert(filtered.end(), best.begin(), best.end());
  }

  std::string png("\x89PNG\r\n\x1a\n", 8);
  std::string ihdr;
  be32(ihdr, uint32_t(width));
  be32(ihdr, uint32_t(height));
  ihdr.push_back(char(8));  // bit depth
  ihdr.push_back(char(2));  // color type: RGB
  ihdr.push_back(char(0));  // compression
  ihdr.push_back(char(0));  // filter method
  ihdr.push_back(char(0));  // no interlace
  chunk(png, "IHDR", ihdr);
  chunk(png, "IDAT", zlibCompress(filtered.data(), filtered.size()));
  chunk(png, "IEND", "");
  return png;
}

} // namespace PngWriter

#pragma once

#include <cstdint>
#include <string>
#include <vector>

// A PNG encoder with no dependency (no zlib, no libpng): 8-bit RGB, each row
// with the PNG filter that best suits it, deflated with fixed Huffman codes
// and LZ77 matches -- a schematic is mostly flat white, which that compresses
// well. What the PNG export of the schematic (RasterPainter) writes.
namespace PngWriter {

// `rgb` holds width*height pixels, 3 bytes each, row by row from the top.
// Returns the PNG file's bytes ("" if the size is 0 or doesn't match).
std::string encodeRgb(const std::vector<uint8_t>& rgb, int width, int height);

// `bytes` in base64 (RFC 4648, padded): how PNG bytes travel in JSON.
std::string base64(const std::string& bytes);

// The zlib stream (RFC 1950) of `data`, as stored in the PNG's IDAT. Exposed
// for tests.
std::string zlibCompress(const uint8_t* data, size_t size);

} // namespace PngWriter

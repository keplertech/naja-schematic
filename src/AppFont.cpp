// AppFont.cpp — the app's default font (see AppFont.h).
#include "AppFont.h"

#include <imgui.h>

#include "DroidSansFont.h"
#include "SymbolFont.h"

// ImGui's built-in fallback (Proggy Clean, ~13px bitmap font) reads as
// tiny/blurry on most displays; DroidSans at kBaseFontSizePx is crisp and
// legible for both chrome (menus, panels) and schematic labels.
ImFont* addAppFont(ImFontAtlas* atlas) {
  ImFont* font = atlas->AddFontFromMemoryCompressedTTF(
      DroidSans_compressed_data, DroidSans_compressed_size, kBaseFontSizePx);
  ImFontConfig symbols;
  symbols.MergeMode = true;
  atlas->AddFontFromMemoryCompressedTTF(
      DejaVuSansSymbols_compressed_data, DejaVuSansSymbols_compressed_size, kBaseFontSizePx, &symbols);
  return font;
}

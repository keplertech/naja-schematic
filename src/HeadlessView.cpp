#ifndef __EMSCRIPTEN__

#include "HeadlessView.h"

#include <imgui.h>

#include "EquipotentialView.h"
#include "GUIData.h"
#include "ShowNetStore.h"

// The headless view is its own renderer backend: it supports dynamic font
// textures (ImGuiBackendFlags_RendererHasTextures), as the GUI backends do,
// so text is measured at the exact size it's drawn at -- like on screen --
// and the PNG export can bake the font at its scaled size. The pixels stay
// in memory (RasterPainter reads them there): honoring a texture request is
// just acknowledging it.
static void serviceTextures() {
  for (ImTextureData* tex : ImGui::GetPlatformIO().Textures) {
    if (tex->Status == ImTextureStatus_WantCreate || tex->Status == ImTextureStatus_WantUpdates) {
      tex->SetTexID(ImTextureID(intptr_t(tex->UniqueID) + 1));
      tex->SetStatus(ImTextureStatus_OK);
    } else if (tex->Status == ImTextureStatus_WantDestroy) {
      tex->SetTexID(ImTextureID_Invalid);
      tex->SetStatus(ImTextureStatus_Destroyed);
    }
  }
}

HeadlessView::HeadlessView(INetlistProvider* provider) {
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.DisplaySize = ImVec2(1600.0f, 1000.0f);
  io.DeltaTime   = 1.0f / 60.0f;
  io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
  setupFonts();

  state_.guiData  = new GUIData();
  state_.provider = provider;
  setupProvider(state_);   // a synchronous provider answers load_root here
}

HeadlessView::~HeadlessView() {
  EquipotentialView::setProvider(nullptr);
  ImGui::DestroyContext();
}

void HeadlessView::settle(int frames) {
  GUIData& gui = *state_.guiData;
  for (int i = 0; i < frames; ++i) {
    // The schematic panel alone, filling the display, as in appFrame().
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("Schematic", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings);
    if (EquipotentialView::takePendingClear()) {
      ShowNetStore::forget();
      gui.clearEquipotentials();
    }
    EquipotentialView::renderSchematic(gui.equipotentials_);
    ImGui::End();
    ImGui::Render();
    serviceTextures();
  }
}

std::string HeadlessView::exportSvg() const { return EquipotentialView::exportSvg(); }
std::string HeadlessView::exportPng(float scale) const {
  const std::string png = EquipotentialView::exportPng(scale);
  serviceTextures();  // the export may have baked glyphs at new sizes
  return png;
}

#endif // __EMSCRIPTEN__

#ifndef __EMSCRIPTEN__

#include "HeadlessView.h"

#include <imgui.h>

#include "EquipotentialView.h"
#include "GUIData.h"
#include "ShowNetStore.h"

HeadlessView::HeadlessView(INetlistProvider* provider) {
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.DisplaySize = ImVec2(1600.0f, 1000.0f);
  io.DeltaTime   = 1.0f / 60.0f;
  setupFonts();
  unsigned char* pixels; int w, h;
  io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);

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
  }
}

std::string HeadlessView::exportSvg() const { return EquipotentialView::exportSvg(); }

#endif // __EMSCRIPTEN__

#include "DiagnosisView.h"

#include <imgui.h>

#include "DiagnosisStore.h"
#include "Types.h"

void DiagnosisView::render() {
  ImGui::Text("Diagnostics");
  ImGui::Separator();

  const auto& items = DiagnosisStore::all();
  if (items.empty()) {
    ImGui::TextDisabled("No diagnosis loaded.");
    ImGui::TextWrapped(
      "Load a kepler-formal/naja-scope diagnosis JSON (File > Load Diagnosis "
      "JSON...) or wait for a diagnosis_response from the provider.");
    return;
  }

  ImGui::BeginChild("##DiagList", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
  for (const auto& item : items) {
    ImU32 col = DiagnosisStore::colorForSeverity(item.severity);
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::Bullet();
    ImGui::SameLine();
    ImGui::TextUnformatted(toString(item.severity));
    ImGui::PopStyleColor();

    std::string where = item.kind == DiagnosisKind::Instance
      ? (item.path.empty() ? std::string("<top>") : displayPath(item.path))
      : (item.path.empty() ? item.terminal : displayPath(item.path) + "/" + item.terminal);
    ImGui::SameLine();
    ImGui::TextDisabled("%s", where.c_str());

    if (!item.source.empty()) {
      ImGui::SameLine();
      ImGui::TextDisabled("[%s]", item.source.c_str());
    }

    ImGui::TextWrapped("%s", item.message.c_str());
    ImGui::Separator();
  }
  ImGui::EndChild();
}

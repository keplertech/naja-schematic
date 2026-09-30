#include "DiagnosisView.h"

#include <imgui.h>

#include "DiagnosisStore.h"
#include "Types.h"

// "u1/u2" for the item's instance: its names, with the id standing in for an
// anonymous level ("<#3>", as displayName()) when the item gives id_path.
static std::string itemDisplayPath(const DiagnosisItem& item) {
  const size_t n = item.idPath ? item.idPath->size() : item.path.size();
  InstancePath path(n);
  for (size_t i = 0; i < n; ++i) {
    if (item.idPath) path[i].id = (*item.idPath)[i];
    if (i < item.path.size()) path[i].name = item.path[i];
  }
  if (!item.idPath) {
    // No ids to show: print the names as given.
    std::string out;
    for (size_t i = 0; i < n; ++i) out += (i ? "/" : "") + item.path[i];
    return out;
  }
  return displayPath(path);
}

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

    const std::string at = itemDisplayPath(item);
    std::string where = item.kind == DiagnosisKind::Instance
      ? (at.empty() ? std::string("<top>") : at)
      : (at.empty() ? item.terminal : at + "/" + item.terminal);
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

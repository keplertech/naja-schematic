#include "PropertiesView.h"

#include <imgui.h>

#include "PropertiesStore.h"

void PropertiesView::render() {
  ImGui::Text("Properties");
  ImGui::Separator();

  if (!PropertiesStore::hasProperties()) {
    ImGui::TextDisabled("No object selected.");
    ImGui::TextWrapped(
      "Select an instance in the tree or on the schematic, or right-click "
      "a pin or net row and choose \"Show Properties\".");
    return;
  }

  if (!PropertiesStore::subject().empty()) {
    ImGui::TextUnformatted(PropertiesStore::subject().c_str());
    ImGui::Separator();
  }

  const auto& properties = PropertiesStore::properties();
  if (properties.empty()) {
    ImGui::TextDisabled("No properties.");
    return;
  }

  if (ImGui::BeginTable("##PropTable", 2,
      ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
    ImGui::TableSetupColumn("Name");
    ImGui::TableSetupColumn("Value");
    ImGui::TableHeadersRow();
    for (const auto& p : properties) {
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::TextUnformatted(p.name.c_str());
      ImGui::TableSetColumnIndex(1);
      ImGui::TextUnformatted(p.value.c_str());
    }
    ImGui::EndTable();
  }
}

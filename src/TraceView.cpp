#include "TraceView.h"

#include <algorithm>
#include <string>

#include <imgui.h>

#include "GUIData.h"
#include "TraceStore.h"
#include "Types.h"

void TraceView::render(GUIData& data) {
  const auto& traces = TraceStore::traces();
  if (traces.empty()) {
    ImGui::TextDisabled("No trace in the schematic.");
    ImGui::TextWrapped(
      "Right-click a pin in the tree and choose \"Add Trace to View\", or "
      "right-click a pin on the schematic and choose \"Trace to Driver\": "
      "each trace is drawn in its own color, and wires and gates two traces "
      "share are drawn white.");
    return;
  }

  int toRemove = 0;
  if (ImGui::BeginTable("##TraceTable", 4,
      ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
    ImGui::TableSetupColumn("Shown", ImGuiTableColumnFlags_WidthFixed, 60);
    ImGui::TableSetupColumn("Trace from", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Nets", ImGuiTableColumnFlags_WidthFixed, 50);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 70);
    ImGui::TableHeadersRow();
    for (const auto& t : traces) {
      ImGui::PushID(t.id);
      ImGui::TableNextRow();

      ImGui::TableSetColumnIndex(0);
      bool visible = t.visible;
      if (ImGui::Checkbox("##shown", &visible)) TraceStore::setVisible(t.id, visible);
      ImGui::SameLine();
      if (ImGui::ColorButton("##color", ImColor(TraceStore::color(t.id)).Value,
                             ImGuiColorEditFlags_NoTooltip, ImVec2(16, 16)))
        TraceStore::cycleStyle(t.id);
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click: next color");

      ImGui::TableSetColumnIndex(1);
      ImGui::TextUnformatted(t.label.c_str());

      ImGui::TableSetColumnIndex(2);
      size_t nets = std::count_if(data.equipotentials_.begin(), data.equipotentials_.end(),
        [&](const Equipotential* eq) {
          return std::find(eq->traceIds.begin(), eq->traceIds.end(), t.id) != eq->traceIds.end();
        });
      if (nets) ImGui::Text("%zu", nets);
      else      ImGui::TextDisabled("...");

      ImGui::TableSetColumnIndex(3);
      if (ImGui::SmallButton("Remove")) toRemove = t.id;
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  // After the loop: removing changes the list being iterated.
  if (toRemove) data.removeTrace(toRemove);
}

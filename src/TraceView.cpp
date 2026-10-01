#include "TraceView.h"

#include <algorithm>
#include <string>

#include <imgui.h>

#include "GUIData.h"
#include "TraceStore.h"
#include "Types.h"

namespace {

// What the picker shows. It can hold a color TraceStore refuses (near the
// convergence color or the canvas): the trace then keeps its last accepted color.
ImVec4 g_pickerColor;

void renderColorPopup(const Trace& t) {
  if (!ImGui::BeginPopup("##pick")) return;
  ImGui::TextUnformatted(t.label.c_str());
  ImGui::Separator();

  // The palette first: one click, and the colors are known to read well.
  for (int s = 0; s < TraceStore::kStyleCount; ++s) {
    ImGui::PushID(s);
    if (s) ImGui::SameLine();
    ImVec4 c = ImColor(TraceStore::styleColor(s));
    if (ImGui::ColorButton("##palette", c, ImGuiColorEditFlags_NoTooltip, ImVec2(20, 20))) {
      TraceStore::setColor(t.id, TraceStore::styleColor(s));
      g_pickerColor = c;
    }
    ImGui::PopID();
  }

  if (ImGui::ColorPicker3("##picker", &g_pickerColor.x,
                          ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoSmallPreview))
    TraceStore::setColor(t.id, ImGui::ColorConvertFloat4ToU32(g_pickerColor));
  const ImU32 picked = ImGui::ColorConvertFloat4ToU32(g_pickerColor);
  if (TraceStore::tooCloseToConvergence(picked))
    ImGui::TextDisabled("Too close to black, which marks gates two traces share.");
  else if (TraceStore::tooCloseToCanvas(picked))
    ImGui::TextDisabled("Too close to white: it wouldn't show on the schematic.");

  if (ImGui::Button("Reset")) {
    TraceStore::resetColor(t.id);
    g_pickerColor = ImColor(TraceStore::color(t.id));
  }
  ImGui::SameLine();
  if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
}

}  // namespace

void TraceView::render(GUIData& data) {
  const auto& traces = TraceStore::traces();
  if (traces.empty()) {
    ImGui::TextDisabled("No trace in the schematic.");
    ImGui::TextWrapped(
      "Right-click a pin, in the tree or on the schematic, and choose "
      "\"Trace to Driver (add to view)\": "
      "each trace is drawn in its own color, wires two traces share are "
      "striped in both colors, and gates they share are outlined in black.");
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
                             ImGuiColorEditFlags_NoTooltip, ImVec2(16, 16))) {
        g_pickerColor = ImColor(TraceStore::color(t.id));
        ImGui::OpenPopup("##pick");
      }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click: change color");
      renderColorPopup(t);

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

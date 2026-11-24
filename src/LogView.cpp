#include "LogView.h"

#include <imgui.h>

#include <vector>

#include "Console.h"
#include "Theme.h"

namespace {

struct State {
  std::vector<LogEntry> entries;   // Console::entries() at `revision`
  uint64_t revision {~uint64_t(0)};
  std::vector<int> shown;          // indices into entries passing the filter
  bool info {true}, warnings {true}, errors {true};
  bool filterChanged {true};
  size_t seenErrors {0};
};

State& state() {
  static State s;
  return s;
}

bool passes(const State& s, LogLevel level) {
  switch (level) {
    case LogLevel::Info:    return s.info;
    case LogLevel::Warning: return s.warnings;
    case LogLevel::Error:   return s.errors;
  }
  return true;
}

}  // namespace

size_t LogView::unseenErrors() {
  const size_t n = Console::errorCount();
  State& s = state();
  if (n < s.seenErrors) s.seenErrors = 0;   // cleared since
  return n - s.seenErrors;
}

std::string LogView::format(const LogEntry& e) {
  char time[16] = "";
  std::tm tm {};
#ifdef _WIN32
  localtime_s(&tm, &e.time);
#else
  localtime_r(&e.time, &tm);
#endif
  std::strftime(time, sizeof(time), "%H:%M:%S", &tm);
  std::string line = time;
  line += ' ';
  if (!e.source.empty()) line += "[" + e.source + "] ";
  return line + e.text;
}

void LogView::render() {
  State& s = state();
  s.seenErrors = Console::errorCount();

  const uint64_t revision = Console::revision();
  const bool changed = revision != s.revision;
  if (changed) {
    s.entries = Console::entries();
    s.revision = revision;
  }
  if (changed || s.filterChanged) {
    s.shown.clear();
    for (int i = 0; i < int(s.entries.size()); ++i)
      if (passes(s, s.entries[i].level)) s.shown.push_back(i);
    s.filterChanged = false;
  }

  s.filterChanged |= ImGui::Checkbox("Info", &s.info);
  ImGui::SameLine();
  s.filterChanged |= ImGui::Checkbox("Warnings", &s.warnings);
  ImGui::SameLine();
  s.filterChanged |= ImGui::Checkbox("Errors", &s.errors);
  ImGui::SameLine();
  if (ImGui::Button("Copy")) {
    std::string all;
    for (int i : s.shown) all += format(s.entries[i]) + "\n";
    ImGui::SetClipboardText(all.c_str());
  }
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Copy the lines shown to the clipboard");
  ImGui::SameLine();
  if (ImGui::Button("Clear")) Console::clear();
  ImGui::Separator();

  ImGui::BeginChild("##LogLines", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
  // Follow new lines while scrolled to the bottom, as a terminal does.
  const bool atBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;
  if (s.shown.empty()) ImGui::TextDisabled("No messages.");
  ImGuiListClipper clipper;
  clipper.Begin(int(s.shown.size()));
  while (clipper.Step()) {
    for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
      const LogEntry& e = s.entries[s.shown[row]];
      const std::string line = format(e);
      if (e.level == LogLevel::Info) {
        ImGui::TextUnformatted(line.c_str());
      } else {
        ImGui::PushStyleColor(ImGuiCol_Text,
            e.level == LogLevel::Error ? theme().errorText : theme().warningText);
        ImGui::TextUnformatted(line.c_str());
        ImGui::PopStyleColor();
      }
    }
  }
  if (changed && atBottom) ImGui::SetScrollHereY(1.0f);
  ImGui::EndChild();
}

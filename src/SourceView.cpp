#include "SourceView.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#ifndef __EMSCRIPTEN__
#include <mutex>
#include <thread>
#include <unistd.h>
#endif

#include <imgui.h>

#include "ImGuiColorTextEdit/TextEditor.h"

#include "Console.h"
#include "INetlistProvider.h"
#include "SourceStore.h"
#include "VerilogLanguage.h"

namespace {

ImFont* g_font = nullptr;

// Squiggle category of the shown reference's range (see TextEditor::AddSquiggle).
constexpr size_t kReferenceSquiggle = 1;
constexpr ImU32  kReferenceColor    = IM_COL32(230, 170, 40, 110);  // the shown range
constexpr ImU32  kReferenceLine     = IM_COL32(230, 170, 40, 200);  // its line numbers
constexpr ImU32  kOtherLine         = IM_COL32(120, 160, 230, 170); // the object's other references here

std::string roleLabel(const SourceRef& ref) {
  if (ref.role == "declaration")       return "Declaration";
  if (ref.role == "model_declaration") return "Model definition";
  if (ref.role == "origin")            return "Origin " + std::to_string(ref.index.value_or(0) + 1);
  if (ref.role == "legacy")            return "Location";
  return ref.role;
}

// "" (unknown) shows as a dash: naja never guesses these.
const char* orDash(const std::string& s) { return s.empty() ? "-" : s.c_str(); }

// The lines to highlight in the line-number gutter (document line -> color,
// tooltip): the shown reference's and the object's other references'.
struct LineMark { ImU32 color; std::string tooltip; };
std::unordered_map<size_t, LineMark> g_lineMarks;

TextEditor& editor() {
  static TextEditor e = [] {
    TextEditor t;
    t.SetReadOnlyEnabled(true);
    t.SetLanguage(verilogLanguage());
    t.SetPalette(TextEditor::GetDarkPalette());
    t.SetShowCurrentLineHighlightEnabled(false);
    t.SetShowWhitespacesEnabled(false);
    t.SetShowMatchingBrackets(true);
    // A big file comes as a window of its lines: number them as in the file.
    // Marked lines are highlighted here too, behind their number: the
    // editor's own line-number markers are offset from these numbers by the
    // window padding (it places them from the text origin, the numbers from
    // the window edge), so the box is drawn from the same position and
    // width as the digits instead.
    t.SetCustomLineNumberRenderer([](const TextEditor::CustomLineNumber& n) {
      std::string text = std::to_string(SourceStore::firstLine() + n.lineNumber);
      const float w = ImGui::CalcTextSize(text.c_str()).x;
      const float right = n.pos.x + n.size.x;
      if (auto mark = g_lineMarks.find(n.lineNumber); mark != g_lineMarks.end()) {
        const float pad = ImGui::CalcTextSize("0").x * 0.25f;
        const ImVec2 min(right - std::max(n.size.x, w) - pad, n.pos.y);
        const ImVec2 max(right + pad, n.pos.y + n.size.y);
        n.drawList->AddRectFilled(min, max, mark->second.color);
        if (ImGui::IsMouseHoveringRect(min, max)) ImGui::SetTooltip("%s", mark->second.tooltip.c_str());
      }
      n.drawList->AddText(ImVec2(right - w, n.pos.y), n.color, text.c_str());
    });
    return t;
  }();
  return e;
}

size_t digits(std::uint64_t n) { return std::to_string(n).size(); }

// The shown file just arrived: load it into the editor, mark the reference,
// and scroll to it.
void loadIntoEditor() {
  auto& e = editor();
  const auto& lines = SourceStore::lines();
  const auto first  = SourceStore::firstLine();
  e.SetText(std::vector<std::string_view>(lines.begin(), lines.end()));
  g_lineMarks.clear();
  e.ClearSquiggles();
  // Room for the file's line numbers, not just the window's.
  const auto last = first + lines.size() - 1;
  e.SetLineNumberLeftMargin(1 + digits(last) - std::min(digits(last), digits(lines.size())));

  const auto& shown = SourceStore::shown();
  const std::string where = roleLabel(shown) + ": " + sourceRefLocation(shown);
  auto inWindow = [&](std::uint64_t lineNo) { return lineNo >= first && lineNo <= last; };

  // The object's other references into this file: their first line, marked.
  const auto shownIndex = SourceStore::shownReference();
  const auto& refs = SourceStore::references();
  for (size_t i = 0; i < refs.size(); ++i) {
    if (shownIndex == i || refs[i].file != shown.file || !inWindow(refs[i].line)) continue;
    const std::string tip = roleLabel(refs[i]) + ": " + sourceRefLocation(refs[i]);
    g_lineMarks[refs[i].line - first] = {kOtherLine, tip};
  }
  // The shown reference: its byte columns as a highlighted range, its lines marked.
  for (size_t i = 0; i < lines.size(); ++i) {
    auto span = SourceStore::highlightBytes(i);
    if (!span) continue;
    g_lineMarks[i] = {kReferenceLine, where};
    const size_t g0 = SourceStore::glyphIndex(lines[i], span->first);
    const size_t g1 = SourceStore::glyphIndex(lines[i], span->second);
    if (g1 > g0)
      e.AddSquiggle(TextEditor::DocPos(i, g0), TextEditor::DocPos(i, g1), kReferenceSquiggle,
                    kReferenceColor, where, TextEditor::SquiggleStyle::background);
  }
  const auto target = SourceStore::targetLine();
  if (inWindow(target)) {
    const size_t row = target - first;
    const size_t col = shown.column ? SourceStore::glyphIndex(lines[row], shown.column - 1) : 0;
    e.SetCursor(TextEditor::DocPos(row, col));
    e.ScrollToLine(row, TextEditor::Scroll::alignMiddle);
  }
}

void renderReferences(INetlistProvider* provider) {
  ImGui::TextUnformatted(SourceStore::subject().empty() ? "<top>" : SourceStore::subject().c_str());
  const auto& refs = SourceStore::references();
  if (refs.empty()) {
    ImGui::TextDisabled("No source information for this object.");
    return;
  }
  const float rowH = ImGui::GetTextLineHeightWithSpacing();
  const float height = rowH * (std::min<size_t>(refs.size(), 6) + 1) + 4.0f;
  const ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg |
                                ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
  if (!ImGui::BeginTable("##SourceRefs", 5, flags, ImVec2(0, height))) return;
  ImGui::TableSetupScrollFreeze(0, 1);
  ImGui::TableSetupColumn("Role", ImGuiTableColumnFlags_WidthFixed);
  ImGui::TableSetupColumn("Location", ImGuiTableColumnFlags_WidthStretch);
  ImGui::TableSetupColumn("Representation", ImGuiTableColumnFlags_WidthFixed);
  ImGui::TableSetupColumn("Language", ImGuiTableColumnFlags_WidthFixed);
  ImGui::TableSetupColumn("Provider", ImGuiTableColumnFlags_WidthFixed);
  ImGui::TableHeadersRow();
  const auto shown = SourceStore::shownReference();
  for (size_t i = 0; i < refs.size(); ++i) {
    const auto& ref = refs[i];
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::PushID(static_cast<int>(i));
    if (ImGui::Selectable(roleLabel(ref).c_str(), shown == i,
                          ImGuiSelectableFlags_SpanAllColumns) && provider)
      provider->send(SourceStore::loadRequest(i).dump());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show this reference's file");
    ImGui::PopID();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(sourceRefLocation(ref).c_str());
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(orDash(ref.representation));
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(orDash(ref.language));
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(orDash(ref.provider));
  }
  ImGui::EndTable();
}

#ifndef __EMSCRIPTEN__
// What the last "Open in Editor" did, written by its worker thread: shown
// next to the button, so a missing editor doesn't fail silently.
std::mutex  g_editorMutex;
std::string g_editorStatus;
bool        g_editorFailed = false;

bool isExecutable(const std::string& path) { return ::access(path.c_str(), X_OK) == 0; }

bool onPath(const std::string& name) {
  const char* path = std::getenv("PATH");
  std::string dirs = path ? path : "";
  for (size_t start = 0; start <= dirs.size();) {
    size_t end = dirs.find(':', start);
    if (end == std::string::npos) end = dirs.size();
    if (end > start && isExecutable(dirs.substr(start, end - start) + "/" + name)) return true;
    start = end + 1;
  }
  return false;
}

// The `code` CLI inside an installed VS Code app, "" if none (macOS: VS Code
// ships it there, and only links it onto PATH on request).
std::string bundledVSCode() {
#ifdef __APPLE__
  const std::string inApp = "/Visual Studio Code.app/Contents/Resources/app/bin/code";
  const char* home = std::getenv("HOME");
  for (const std::string& dir : {std::string("/Applications"),
                                 std::string(home ? home : "") + "/Applications"})
    if (isExecutable(dir + inApp)) return dir + inApp;
#endif
  return "";
}

// Opens the shown file in an external editor ($NAJA_SCHEMATIC_EDITOR, else
// VS Code, else the system's opener; see SourceStore::editorPattern()), on a
// worker thread whose exit status and output end up in g_editorStatus.
// Native only: the file is on this machine (the host read it), the
// browser's isn't.
void openInEditor() {
  const char* env = std::getenv("NAJA_SCHEMATIC_EDITOR");
#ifdef __APPLE__
  const char* opener = "open";
#else
  const char* opener = "xdg-open";
#endif
  const std::string pattern = SourceStore::editorPattern(env ? env : "", onPath("code"),
                                                         bundledVSCode(), opener);
  const auto& ref = SourceStore::shown();
  const std::string cmd = SourceStore::editorCommand(pattern, SourceStore::hostPath(),
                                                     ref.line, ref.column);
  Console::Log("Opening in editor: " + cmd);
  {
    std::lock_guard<std::mutex> lock(g_editorMutex);
    g_editorStatus = "Opening...";
    g_editorFailed = false;
  }
  std::thread([cmd] {
    std::string output;
    int status = -1;
    if (FILE* pipe = ::popen((cmd + " 2>&1").c_str(), "r")) {
      char buf[256];
      while (std::fgets(buf, sizeof buf, pipe)) output += buf;
      status = ::pclose(pipe);
    }
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) output.pop_back();
    std::lock_guard<std::mutex> lock(g_editorMutex);
    g_editorFailed = status != 0;
    g_editorStatus = g_editorFailed
      ? "Editor command failed: " + cmd + (output.empty() ? "" : "\n" + output) +
        "\nSet NAJA_SCHEMATIC_EDITOR, e.g. \"subl {file}:{line}:{col}\"."
      : "Opened in the editor.";
  }).detach();
}
#endif

void renderFile() {
  const auto& ref = SourceStore::shown();
  if (SourceStore::missing()) {
    ImGui::TextWrapped(
      "%s is not readable on the netlist host. Relative paths are looked up "
      "in the --source-root directories, then the current directory.",
      ref.file.empty() ? "<unknown file>" : ref.file.c_str());
    return;
  }
  if (!SourceStore::hasSource()) return;

  if (SourceStore::takePendingScroll()) loadIntoEditor();

  ImGui::TextUnformatted(sourceRefLocation(ref).c_str());
  if (SourceStore::truncated()) {
    ImGui::SameLine();
    ImGui::TextDisabled("(large file: only the lines around the reference)");
  }
#ifndef __EMSCRIPTEN__
  if (!SourceStore::hostPath().empty()) {
    ImGui::SameLine();
    if (ImGui::SmallButton("Open in Editor")) openInEditor();
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Runs $NAJA_SCHEMATIC_EDITOR ({file}, {line}, {col}),\n"
                        "else VS Code (code -g), else the system's default app.\n"
                        "Edits there reach this view only once the design is\n"
                        "loaded again: until then its source lines may be off.");
    std::lock_guard<std::mutex> lock(g_editorMutex);
    if (!g_editorStatus.empty()) {
      ImGui::SameLine();
      const auto firstLine = g_editorStatus.substr(0, g_editorStatus.find('\n'));
      if (g_editorFailed) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", firstLine.c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", g_editorStatus.c_str());
      } else {
        ImGui::TextDisabled("%s", firstLine.c_str());
      }
    }
  }
#endif

  if (SourceStore::stale()) {
    // naja recorded the ranges when the design was loaded: the file changed
    // since, so they may point at the wrong lines.
    ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                       "This file changed after the design was loaded: the highlighted "
                       "lines may be off. Load the design again to update them.");
  }
  if (g_font) ImGui::PushFont(g_font, 0.0f);
  editor().Render("##SourceText");
  if (g_font) ImGui::PopFont();
}

} // namespace

void SourceView::setFont(ImFont* font) { g_font = font; }

void SourceView::render(INetlistProvider* provider) {
  if (!SourceStore::hasReferences() && !SourceStore::hasSource() && !SourceStore::missing()) {
    ImGui::TextDisabled("No source shown.");
    ImGui::TextWrapped(
      "Right-click an instance, term or net in the tree, or an instance box "
      "in the schematic, and choose \"Show Sources\" (offered when the "
      "netlist records where the object comes from: its declaration, and "
      "for synthesized logic the RTL it came from).");
    return;
  }
  if (SourceStore::hasReferences()) {
    renderReferences(provider);
    ImGui::Separator();
  }
  renderFile();
}

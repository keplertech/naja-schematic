#include "ReviewView.h"

#include <imgui.h>

#include "DiagnosisStore.h"
#include "INetlistProvider.h"
#include "ReviewStore.h"
#include "SelectionStore.h"

namespace {
// What the Review tab lists.
enum class Filter { Open, All };
Filter g_filter = Filter::Open;
std::string g_selected;     // thread id
std::string g_replyFor;     // thread the reply box was typed for
char g_reply[4096] = "";

// The Add Comment dialog.
bool g_composerRequested = false;
json g_composerAnchor;
std::string g_composerLabel;
char g_composerText[4096] = "";
char g_composerTitle[256] = "";
int g_composerSeverity = 0;  // DiagnosisSeverity

const char* kComposerTitle = "Add Comment";
std::optional<ImVec2> g_commentButton;

ImVec4 severityColor(DiagnosisSeverity severity) {
  return ImColor(DiagnosisStore::colorForSeverity(severity)).Value;
}

const ReviewThread* findThread(const std::string& id) {
  for (const auto& t : ReviewStore::threads())
    if (t.id == id) return &t;
  return nullptr;
}

std::string firstLine(const std::string& s) {
  auto end = s.find('\n');
  return end == std::string::npos ? s : s.substr(0, end) + " ...";
}

std::string summary(const ReviewThread& t) {
  if (!t.title.empty()) return t.title;
  return t.comments.empty() ? std::string() : firstLine(t.comments.front().text);
}

void sendEvent(INetlistProvider* provider, const ReviewThread& t, const std::string& text,
               const std::string& status) {
  provider->send(ReviewStore::addEventRequest(t.id, text, status).dump());
}

void renderDetail(INetlistProvider* provider, const ReviewThread& t) {
  ImGui::SeparatorText(anchorLabel(t.anchor).c_str());
  ImGui::TextColored(severityColor(t.severity), "%s", toString(t.severity));
  ImGui::SameLine();
  ImGui::Text("%s", t.status.c_str());
  for (const auto& label : t.labels) {
    ImGui::SameLine();
    ImGui::TextDisabled("#%s", label.c_str());
  }
  if (t.state == ReviewState::OtherSnapshot) {
    ImGui::TextWrapped("Written on another netlist: not shown on this design.%s",
                       t.snapshotStored ? "" : " Its snapshot isn't in the review.");
    if (t.snapshotStored) {
      ImGui::TextDisabled("naja-schematic --review DIR --snapshot %s", t.snapshot.c_str());
      ImGui::SameLine();
      if (ImGui::SmallButton("Copy Snapshot ID")) ImGui::SetClipboardText(t.snapshot.c_str());
    }
  } else if (t.state == ReviewState::Unresolved) {
    ImGui::TextWrapped("Its ids don't resolve in this netlist (an edited review file?).");
  } else if (t.resolvedPath && t.anchor.kind != ReviewAnchorKind::View) {
    if (ImGui::SmallButton("Select")) SelectionStore::select(*t.resolvedPath,
                                                             SelectionStore::Origin::Host);
    if (!t.resolvedPath->empty()) {
      ImGui::SameLine();
      if (ImGui::SmallButton("Show in Schematic")) {
        json req = {{"request", "resolve_instance"}};
        writePath(req, *t.resolvedPath);
        provider->send(req.dump());
      }
    }
  }

  for (const auto& c : t.comments) {
    ImGui::Spacing();
    ImGui::TextDisabled("%s  %s", c.author.c_str(), c.date.c_str());
    if (!c.text.empty()) ImGui::TextWrapped("%s", c.text.c_str());
  }

  if (!ReviewStore::writable()) return;
  ImGui::Spacing();
  if (g_replyFor != t.id) {
    g_replyFor = t.id;
    g_reply[0] = '\0';
  }
  ImGui::InputTextMultiline("##reply", g_reply, sizeof(g_reply),
                            ImVec2(-1, ImGui::GetTextLineHeight() * 4));
  const bool hasText = g_reply[0] != '\0';
  ImGui::BeginDisabled(!hasText);
  if (ImGui::Button("Reply")) {
    sendEvent(provider, t, g_reply, "");
    g_reply[0] = '\0';
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  // A status change carries the typed text, if any, as its comment.
  if (t.isOpen()) {
    if (ImGui::Button("Resolve")) { sendEvent(provider, t, g_reply, "resolved"); g_reply[0] = '\0'; }
    ImGui::SameLine();
    if (ImGui::Button("Won't Fix")) { sendEvent(provider, t, g_reply, "wontfix"); g_reply[0] = '\0'; }
  } else if (ImGui::Button("Reopen")) {
    sendEvent(provider, t, g_reply, "open");
    g_reply[0] = '\0';
  }
}
}  // namespace

void ReviewView::render(INetlistProvider* provider) {
  if (!ReviewStore::hasReview()) {
    ImGui::TextWrapped("No review loaded. Start the host with --review DIR to read and "
                       "write review threads (docs/review_format.md).");
    return;
  }
  if (ReviewStore::writable())
    ImGui::Text("Commenting as %s", ReviewStore::author().c_str());
  else
    ImGui::TextDisabled("Read-only: the host has no author (--author, NAJA_SCHEMATIC_AUTHOR "
                        "or git config user.name)");
  ImGui::SameLine();
  int filter = static_cast<int>(g_filter);
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7);
  if (ImGui::Combo("##filter", &filter, "Open\0All\0")) g_filter = static_cast<Filter>(filter);
  if (!ReviewStore::error().empty())
    ImGui::TextColored(severityColor(DiagnosisSeverity::Error), "%s",
                       ReviewStore::error().c_str());

  const auto& threads = ReviewStore::threads();
  const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY;
  const float listH = ImGui::GetTextLineHeightWithSpacing() * 8;
  if (ImGui::BeginTable("##threads", 5, flags, ImVec2(0, listH))) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Severity", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Object");
    ImGui::TableSetupColumn("Comment", ImGuiTableColumnFlags_WidthStretch, 2.0f);
    ImGui::TableSetupColumn("Author");
    ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableHeadersRow();
    for (const auto& t : threads) {
      if (g_filter == Filter::Open && !t.isOpen()) continue;
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::PushID(t.id.c_str());
      if (ImGui::Selectable("##row", g_selected == t.id,
                            ImGuiSelectableFlags_SpanAllColumns)) {
        g_selected = t.id;
        if (t.resolvedPath) SelectionStore::select(*t.resolvedPath, SelectionStore::Origin::Host);
      }
      ImGui::PopID();
      ImGui::SameLine();
      ImGui::TextColored(severityColor(t.severity), "%s", toString(t.severity));
      ImGui::TableNextColumn();
      if (t.state == ReviewState::Exact) ImGui::TextUnformatted(anchorLabel(t.anchor).c_str());
      else ImGui::TextDisabled("%s (%s)", anchorLabel(t.anchor).c_str(), toString(t.state));
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(summary(t).c_str());
      if (t.comments.size() > 1) {
        ImGui::SameLine();
        ImGui::TextDisabled("+%zu", t.comments.size() - 1);
      }
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(t.author.c_str());
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(t.status.c_str());
    }
    ImGui::EndTable();
  }
  if (const auto* t = findThread(g_selected)) renderDetail(provider, *t);
}

void ReviewView::openComposer(const json& anchor, const std::string& label) {
  g_composerRequested = true;
  g_composerAnchor = anchor;
  g_composerLabel = label;
  g_composerText[0] = '\0';
  g_composerTitle[0] = '\0';
  g_composerSeverity = static_cast<int>(DiagnosisSeverity::Info);
}

void ReviewView::renderComposer(INetlistProvider* provider) {
  if (g_composerRequested) {
    ImGui::OpenPopup(kComposerTitle);
    g_composerRequested = false;
  }
  g_commentButton.reset();
  ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 30, 0), ImGuiCond_Appearing);
  if (!ImGui::BeginPopupModal(kComposerTitle, nullptr)) return;
  ImGui::TextWrapped("On %s", g_composerLabel.c_str());
  ImGui::InputText("Title", g_composerTitle, sizeof(g_composerTitle));
  ImGui::Combo("Severity", &g_composerSeverity, "info\0warning\0error\0");
  if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
  ImGui::InputTextMultiline("##text", g_composerText, sizeof(g_composerText),
                            ImVec2(-1, ImGui::GetTextLineHeight() * 8));
  ImGui::BeginDisabled(g_composerText[0] == '\0');
  const bool comment = ImGui::Button("Comment");
  const ImVec2 bmin = ImGui::GetItemRectMin(), bmax = ImGui::GetItemRectMax();
  g_commentButton = ImVec2((bmin.x + bmax.x) * 0.5f, (bmin.y + bmax.y) * 0.5f);
  if (comment) {
    provider->send(ReviewStore::addThreadRequest(
        g_composerAnchor, g_composerText,
        static_cast<DiagnosisSeverity>(g_composerSeverity), g_composerTitle).dump());
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
}

std::optional<ImVec2> ReviewView::commentButtonForTesting() { return g_commentButton; }

void ReviewView::commentMenuItem(const json& anchor, const std::string& label) {
  if (!ReviewStore::writable()) return;
  if (ImGui::MenuItem("Add Comment...")) openComposer(anchor, label);
}

void ReviewView::threadsTooltip(const std::vector<const ReviewThread*>& threads) {
  for (const auto* t : threads) {
    ImGui::TextColored(severityColor(t->severity), "[%s] %s", t->status.c_str(),
                       summary(*t).c_str());
    ImGui::TextDisabled("%s, %zu comment(s)", t->author.c_str(), t->comments.size());
  }
}

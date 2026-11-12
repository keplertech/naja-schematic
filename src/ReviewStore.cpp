#include "ReviewStore.h"

#include <utility>

#include "DiagnosisStore.h"

namespace {
bool g_hasReview = false;
bool g_writable = false;
std::string g_author;
std::string g_error;
std::vector<ReviewThread> g_threads;
unsigned g_revision = 0;

std::string text(const json& j, const char* key) {
  auto it = j.find(key);
  return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

std::optional<unsigned> unsignedField(const json& j, const char* key) {
  auto it = j.find(key);
  if (it == j.end() || !it->is_number_unsigned()) return std::nullopt;
  return it->get<unsigned>();
}

DiagnosisSeverity severityFrom(const std::string& s) {
  if (s == "error")   return DiagnosisSeverity::Error;
  if (s == "warning") return DiagnosisSeverity::Warning;
  return DiagnosisSeverity::Info;
}

// The wire's severity names (toString() gives the display form).
const char* severityName(DiagnosisSeverity s) {
  switch (s) {
    case DiagnosisSeverity::Error:   return "error";
    case DiagnosisSeverity::Warning: return "warning";
    case DiagnosisSeverity::Info:    return "info";
  }
  return "info";
}

ReviewAnchorKind kindFrom(const std::string& s) {
  if (s == "term") return ReviewAnchorKind::Term;
  if (s == "net")  return ReviewAnchorKind::Net;
  if (s == "view") return ReviewAnchorKind::View;
  return ReviewAnchorKind::Instance;
}

ReviewState stateFrom(const std::string& s) {
  if (s == "exact")          return ReviewState::Exact;
  if (s == "other_snapshot") return ReviewState::OtherSnapshot;
  return ReviewState::Unresolved;
}

ReviewAnchor parseAnchor(const json& j) {
  ReviewAnchor a;
  if (!j.is_object()) return a;
  a.kind = kindFrom(text(j, "kind"));
  if (j.contains("id_path") && j["id_path"].is_array())
    for (const auto& id : j["id_path"])
      if (id.is_number_unsigned()) a.idPath.push_back(id.get<unsigned>());
  if (j.contains("path") && j["path"].is_array())
    for (const auto& name : j["path"])
      a.path.push_back(name.is_string() ? name.get<std::string>() : std::string());
  a.termId = unsignedField(j, "term_id");
  a.netId = unsignedField(j, "net_id");
  if (j.contains("bit") && j["bit"].is_number_integer()) a.bit = j["bit"].get<int>();
  a.view = text(j, "view");
  a.terminal = text(j, "terminal");
  a.net = text(j, "net");
  return a;
}

ReviewThread parseThread(const json& j) {
  ReviewThread t;
  t.id = text(j, "id");
  t.title = text(j, "title");
  t.author = text(j, "author");
  t.source = text(j, "source");
  t.created = text(j, "created");
  t.snapshot = text(j, "snapshot");
  t.snapshotStored = j.value("snapshot_stored", false);
  if (j.contains("anchor")) t.anchor = parseAnchor(j["anchor"]);
  if (j.contains("resolution") && j["resolution"].is_object()) {
    const auto& r = j["resolution"];
    t.state = stateFrom(text(r, "state"));
    if (t.state == ReviewState::Exact) t.resolvedPath = readPath(r);
  }
  if (j.contains("status") && j["status"].is_string()) t.status = j["status"].get<std::string>();
  t.severity = severityFrom(text(j, "severity"));
  if (j.contains("labels") && j["labels"].is_array())
    for (const auto& label : j["labels"])
      if (label.is_string()) t.labels.push_back(label.get<std::string>());
  if (j.contains("comments") && j["comments"].is_array())
    for (const auto& c : j["comments"])
      t.comments.push_back({text(c, "id"), text(c, "author"), text(c, "date"), text(c, "text")});
  return t;
}

// Exact threads of one kind on one object: same instance ids and, for a
// term/net, same id; `bit` nullopt matches every bit (and the whole bus).
std::vector<const ReviewThread*> find(ReviewAnchorKind kind, const InstancePath& path,
                                      std::optional<unsigned> objectId, std::optional<int> bit) {
  std::vector<const ReviewThread*> found;
  const auto ids = pathIds(path);
  for (const auto& t : g_threads) {
    if (t.state != ReviewState::Exact || t.anchor.kind != kind) continue;
    if (t.anchor.idPath != ids) continue;
    if (kind == ReviewAnchorKind::Term && t.anchor.termId != objectId) continue;
    if (kind == ReviewAnchorKind::Net && t.anchor.netId != objectId) continue;
    if (bit && t.anchor.bit != bit) continue;
    found.push_back(&t);
  }
  return found;
}

json withBit(json anchor, std::optional<int> bit) {
  if (bit) anchor["bit"] = *bit;
  return anchor;
}
}  // namespace

void ReviewStore::setResponse(const json& response) {
  g_hasReview = true;
  g_writable = response.value("writable", false);
  g_author = text(response, "author");
  g_error.clear();
  g_threads.clear();
  if (response.contains("threads") && response["threads"].is_array())
    for (const auto& t : response["threads"]) {
      if (!t.is_object()) continue;
      g_threads.push_back(parseThread(t));
    }
  ++g_revision;
}

void ReviewStore::clear() {
  g_hasReview = false;
  g_writable = false;
  g_author.clear();
  g_error.clear();
  g_threads.clear();
  ++g_revision;
}

bool ReviewStore::hasReview() { return g_hasReview; }
bool ReviewStore::writable() { return g_hasReview && g_writable; }
const std::string& ReviewStore::author() { return g_author; }
const std::vector<ReviewThread>& ReviewStore::threads() { return g_threads; }
unsigned ReviewStore::revision() { return g_revision; }

std::vector<const ReviewThread*> ReviewStore::instanceThreads(const InstancePath& path) {
  return find(ReviewAnchorKind::Instance, path, std::nullopt, std::nullopt);
}

std::vector<const ReviewThread*> ReviewStore::termThreads(const InstancePath& path,
                                                          unsigned termId,
                                                          std::optional<int> bit) {
  return find(ReviewAnchorKind::Term, path, termId, bit);
}

std::vector<const ReviewThread*> ReviewStore::netThreads(const InstancePath& path,
                                                         unsigned netId,
                                                         std::optional<int> bit) {
  return find(ReviewAnchorKind::Net, path, netId, bit);
}

std::vector<const ReviewThread*> ReviewStore::pinThreads(const std::vector<unsigned>& idPath,
                                                         unsigned termId, std::optional<int> bit) {
  std::vector<const ReviewThread*> found;
  for (const auto& t : g_threads) {
    if (t.state != ReviewState::Exact || t.anchor.kind != ReviewAnchorKind::Term) continue;
    if (t.anchor.idPath != idPath || t.anchor.termId != termId) continue;
    if (bit && t.anchor.bit && t.anchor.bit != bit) continue;
    found.push_back(&t);
  }
  return found;
}

size_t ReviewStore::openCount(const std::vector<const ReviewThread*>& threads) {
  size_t n = 0;
  for (const auto* t : threads)
    if (t->isOpen()) ++n;
  return n;
}

ImU32 ReviewStore::openColor(const std::vector<const ReviewThread*>& threads) {
  std::optional<DiagnosisSeverity> worst;
  for (const auto* t : threads)
    if (t->isOpen() && (!worst || t->severity > *worst)) worst = t->severity;
  return worst ? DiagnosisStore::colorForSeverity(*worst) : 0;
}

void ReviewStore::setError(const std::string& message) {
  g_error = message;
  ++g_revision;
}

const std::string& ReviewStore::error() { return g_error; }

json ReviewStore::addThreadRequest(const json& anchor, const std::string& text,
                                   DiagnosisSeverity severity, const std::string& title) {
  json req = {{"request", "review_add_thread"}, {"anchor", anchor}, {"text", text},
              {"severity", severityName(severity)}};
  if (!title.empty()) req["title"] = title;
  return req;
}

json ReviewStore::addEventRequest(const std::string& thread, const std::string& text,
                                  const std::string& status) {
  json req = {{"request", "review_add_event"}, {"thread", thread}};
  if (!text.empty()) req["text"] = text;
  if (!status.empty()) req["status"] = status;
  return req;
}

json ReviewStore::instanceAnchor(const InstancePath& path) {
  json anchor = {{"kind", "instance"}};
  writePath(anchor, path);
  return anchor;
}

json ReviewStore::termAnchor(const InstancePath& path, unsigned termId,
                             const std::string& terminal, std::optional<int> bit) {
  json anchor = {{"kind", "term"}, {"term_id", termId}, {"terminal", terminal}};
  writePath(anchor, path);
  return withBit(std::move(anchor), bit);
}

json ReviewStore::netAnchor(const InstancePath& path, unsigned netId, const std::string& net,
                            std::optional<int> bit) {
  json anchor = {{"kind", "net"}, {"net_id", netId}, {"net", net}};
  writePath(anchor, path);
  return withBit(std::move(anchor), bit);
}

const char* toString(ReviewState state) {
  switch (state) {
    case ReviewState::Exact:         return "exact";
    case ReviewState::OtherSnapshot: return "other netlist";
    case ReviewState::Unresolved:    return "unresolved";
  }
  return "";
}

std::string anchorLabel(const ReviewAnchor& anchor) {
  if (anchor.kind == ReviewAnchorKind::View) return "view " + anchor.view;
  std::string label;
  for (size_t i = 0; i < anchor.path.size(); ++i) {
    const auto& name = anchor.path[i];
    if (i) label += "/";
    label += name.empty() && i < anchor.idPath.size()
                 ? "<#" + std::to_string(anchor.idPath[i]) + ">" : name;
  }
  std::string object;
  if (anchor.kind == ReviewAnchorKind::Term) object = anchor.terminal;
  if (anchor.kind == ReviewAnchorKind::Net)
    object = anchor.net.empty() && anchor.netId
                 ? "<net #" + std::to_string(*anchor.netId) + ">" : anchor.net;
  if (!object.empty()) {
    if (anchor.bit) object += "[" + std::to_string(*anchor.bit) + "]";
    label += label.empty() ? object : "/" + object;
  }
  return label.empty() ? std::string("(top)") : label;
}

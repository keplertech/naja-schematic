#include "StyleStore.h"

#include <algorithm>
#include <map>
#include <tuple>
#include <utility>

namespace {
using IdPath   = std::vector<unsigned>;
using NamePath = std::vector<std::string>;
using Indices  = std::vector<size_t>;

std::vector<StyleItem> g_items;
// Per kind, items by the instance they name: those giving an id_path by it,
// the others by their name path (an anonymous level can't be named, so
// those are dropped). A query, an InstancePath, checks both.
std::map<std::tuple<StyleKind, IdPath>, Indices>   g_byId;
std::map<std::tuple<StyleKind, NamePath>, Indices> g_byName;

bool isAmbiguous(const NamePath& path) {
  for (const auto& name : path)
    if (name.empty()) return true;
  return false;
}

void rebuildIndex() {
  g_byId.clear();
  g_byName.clear();
  for (size_t i = 0; i < g_items.size(); ++i) {
    const auto& item = g_items[i];
    if (item.idPath)                    g_byId[{item.kind, *item.idPath}].push_back(i);
    else if (!isAmbiguous(item.path))   g_byName[{item.kind, item.path}].push_back(i);
  }
}

// The items of `kind` on the instance at `path`, in push order (the order
// they merge in).
Indices itemsAt(StyleKind kind, const InstancePath& path) {
  Indices found;
  auto add = [&](const auto& index, const auto& key) {
    auto it = index.find(key);
    if (it != index.end()) found.insert(found.end(), it->second.begin(), it->second.end());
  };
  add(g_byId, std::tuple{kind, pathIds(path)});
  add(g_byName, std::tuple{kind, pathNames(path)});
  std::sort(found.begin(), found.end());
  return found;
}

std::string baseName(const std::string& name) {
  auto pos = name.rfind('[');
  return pos == std::string::npos ? name : name.substr(0, pos);
}

bool pinMatches(const StyleItem& item, const std::string& terminal,
                std::optional<unsigned> termId, std::optional<int> bit) {
  if (item.termId) {
    if (!termId || *item.termId != *termId) return false;
  } else if (item.terminal.empty() || item.terminal != baseName(terminal)) {
    return false;
  }
  return !item.bit || (bit && *item.bit == *bit);
}

StyleAttrs mergedAt(StyleKind kind, const InstancePath& path, const std::string& terminal,
                    std::optional<unsigned> termId, std::optional<int> bit) {
  StyleAttrs out;
  for (size_t i : itemsAt(kind, path))
    if (pinMatches(g_items[i], terminal, termId, bit)) out.merge(g_items[i].attrs);
  return out;
}

bool isId(const nlohmann::json& v) {
  return v.is_number_unsigned() || (v.is_number_integer() && v.get<long long>() >= 0);
}
} // namespace

void StyleAttrs::merge(const StyleAttrs& over) {
  if (over.color)          color = over.color;
  if (over.width > 0)      width = over.width;
  if (over.fill)           fill  = over.fill;
  if (!over.label.empty()) label = over.label;
}

void StyleStore::setStyles(std::vector<StyleItem> items) {
  g_items = std::move(items);
  rebuildIndex();
}

void StyleStore::clear() {
  g_items.clear();
  rebuildIndex();
}

const std::vector<StyleItem>& StyleStore::all() { return g_items; }

std::optional<ImU32> StyleStore::parseColor(const std::string& text) {
  if ((text.size() != 7 && text.size() != 9) || text[0] != '#') return std::nullopt;
  unsigned v[4] = {0, 0, 0, 255};
  for (size_t i = 0; i * 2 + 1 < text.size(); ++i) {
    unsigned byte = 0;
    for (char c : text.substr(1 + i * 2, 2)) {
      byte <<= 4;
      if (c >= '0' && c <= '9')      byte |= unsigned(c - '0');
      else if (c >= 'a' && c <= 'f') byte |= unsigned(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') byte |= unsigned(c - 'A' + 10);
      else return std::nullopt;
    }
    v[i] = byte;
  }
  return IM_COL32(v[0], v[1], v[2], v[3]);
}

std::vector<StyleItem> StyleStore::parse(const nlohmann::json& j, std::vector<std::string>* errors) {
  std::vector<StyleItem> items;
  const nlohmann::json* list = &j;
  if (j.is_object()) {
    if (!j.contains("items")) return items;
    list = &j["items"];
  }
  if (!list->is_array()) return items;
  auto fail = [&](size_t i, const std::string& why) {
    if (errors) errors->push_back("style item " + std::to_string(i) + ": " + why);
  };
  for (size_t i = 0; i < list->size(); ++i) {
    const auto& raw = (*list)[i];
    if (!raw.is_object()) { fail(i, "not an object"); continue; }
    StyleItem item;
    const std::string kind = raw.value("kind", std::string("instance"));
    if (kind == "instance")  item.kind = StyleKind::Instance;
    else if (kind == "net")  item.kind = StyleKind::Net;
    else if (kind == "term") item.kind = StyleKind::Term;
    else { fail(i, "unknown kind \"" + kind + "\""); continue; }

    if (raw.contains("path") && raw["path"].is_array())
      for (const auto& seg : raw["path"])
        if (seg.is_string()) item.path.push_back(seg.get<std::string>());
    if (raw.contains("id_path") && raw["id_path"].is_array()) {
      IdPath ids;
      for (const auto& seg : raw["id_path"]) {
        if (!isId(seg)) break;
        ids.push_back(seg.get<unsigned>());
      }
      // Malformed: falls back to `path`, never truncated to another instance.
      if (ids.size() == raw["id_path"].size()) item.idPath = std::move(ids);
    }
    if (!item.idPath && isAmbiguous(item.path)) {
      fail(i, "an anonymous instance needs an id_path");
      continue;
    }
    if (item.kind != StyleKind::Instance) {
      item.terminal = baseName(raw.value("terminal", std::string()));
      if (raw.contains("term_id") && isId(raw["term_id"])) item.termId = raw["term_id"].get<unsigned>();
      if (raw.contains("bit") && raw["bit"].is_number_integer()) item.bit = raw["bit"].get<int>();
      if (item.terminal.empty() && !item.termId) { fail(i, "no terminal or term_id"); continue; }
    }

    bool bad = false;
    auto color = [&](const char* key, ImU32& out) {
      if (!raw.contains(key) || raw[key].is_null()) return;
      auto c = raw[key].is_string() ? parseColor(raw[key].get<std::string>()) : std::nullopt;
      if (!c) { fail(i, std::string(key) + " is not \"#rrggbb\" or \"#rrggbbaa\""); bad = true; return; }
      out = *c;
    };
    color("color", item.attrs.color);
    color("fill", item.attrs.fill);
    if (bad) continue;
    if (raw.contains("width") && raw["width"].is_number() && raw["width"].get<float>() > 0)
      item.attrs.width = std::min(raw["width"].get<float>(), 40.0f);
    if (raw.contains("label") && raw["label"].is_string()) item.attrs.label = raw["label"].get<std::string>();
    items.push_back(std::move(item));
  }
  return items;
}

StyleAttrs StyleStore::instanceStyle(const InstancePath& path) {
  StyleAttrs out;
  for (size_t i : itemsAt(StyleKind::Instance, path)) out.merge(g_items[i].attrs);
  return out;
}

StyleAttrs StyleStore::termStyle(const InstancePath& path, const std::string& terminal,
                                 std::optional<unsigned> termId, std::optional<int> bit) {
  return mergedAt(StyleKind::Term, path, terminal, termId, bit);
}

StyleAttrs StyleStore::netStyle(const Equipotential& eq) {
  StyleAttrs out;
  if (g_items.empty()) return out;
  // Merged in push order across the net's ends, so the result doesn't
  // depend on the order the host listed the pins in.
  std::vector<size_t> hits;
  auto collect = [&](const InstancePath& path, const BitTerm& term) {
    for (size_t i : itemsAt(StyleKind::Net, path))
      if (pinMatches(g_items[i], term.name, term.child_id, term.bit)) hits.push_back(i);
  };
  for (const auto& term : eq.terms) collect({}, term);
  for (const auto& occ : eq.occurrences) collect(occ.path, occ.term);
  std::sort(hits.begin(), hits.end());
  hits.erase(std::unique(hits.begin(), hits.end()), hits.end());
  for (size_t i : hits) out.merge(g_items[i].attrs);
  return out;
}

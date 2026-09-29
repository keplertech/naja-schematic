#include "DiagnosisStore.h"

#include <map>
#include <tuple>
#include <utility>

namespace {
// Diagnosis UI is temporarily hidden (tree/schematic tinting, tooltips, and
// the Diagnostics tab/menu items in AppLogic.cpp) without removing the
// underlying data/plumbing. Flip to false to re-enable.
constexpr bool kDiagnosisUIHidden = true;

using Items = std::vector<const DiagnosisItem*>;
using IdPath = std::vector<unsigned>;
using NamePath = std::vector<std::string>;

std::vector<DiagnosisItem> g_items;
// Items that give an id_path are indexed by it; the others by their name
// path. A query (an InstancePath, which has both) checks the two.
std::map<IdPath, Items>   g_instanceById;
std::map<NamePath, Items> g_instanceByName;
// (containing instance, terminal) -> items.
std::map<std::tuple<IdPath, std::string>, Items>   g_netById;
std::map<std::tuple<NamePath, std::string>, Items> g_netByName;

// A name path with an anonymous (empty) segment matches every anonymous
// sibling there, so it can't identify anything: such an item needs an
// id_path to be shown on an instance.
bool isAmbiguous(const NamePath& path) {
  for (const auto& name : path)
    if (name.empty()) return true;
  return false;
}

void rebuildIndex() {
  g_instanceById.clear();
  g_instanceByName.clear();
  g_netById.clear();
  g_netByName.clear();
  for (const auto& item : g_items) {
    const bool isInstance = item.kind == DiagnosisKind::Instance;
    if (item.idPath) {
      if (isInstance) g_instanceById[*item.idPath].push_back(&item);
      else            g_netById[{*item.idPath, item.terminal}].push_back(&item);
    } else if (!isAmbiguous(item.path)) {
      if (isInstance) g_instanceByName[item.path].push_back(&item);
      else            g_netByName[{item.path, item.terminal}].push_back(&item);
    }
  }
}

template <typename Map, typename Key>
void appendFound(Items& out, const Map& index, const Key& key) {
  auto it = index.find(key);
  if (it != index.end()) out.insert(out.end(), it->second.begin(), it->second.end());
}

DiagnosisSeverity worstOf(const std::vector<const DiagnosisItem*>& items) {
  DiagnosisSeverity worst = DiagnosisSeverity::Info;
  for (const auto* item : items) {
    if (item->severity > worst) worst = item->severity;
  }
  return worst;
}
} // namespace

void DiagnosisStore::setDiagnostics(std::vector<DiagnosisItem> items) {
  g_items = std::move(items);
  rebuildIndex();
}

void DiagnosisStore::clear() {
  g_items.clear();
  rebuildIndex();
}

const std::vector<DiagnosisItem>& DiagnosisStore::all() { return g_items; }

std::vector<const DiagnosisItem*> DiagnosisStore::instanceDiagnostics(const InstancePath& path) {
  if (kDiagnosisUIHidden) return {};
  return findInstanceItems(path);
}

std::vector<const DiagnosisItem*> DiagnosisStore::netDiagnostics(const InstancePath& path,
                                                                  const std::string& terminal) {
  if (kDiagnosisUIHidden) return {};
  return findNetItems(path, terminal);
}

std::vector<const DiagnosisItem*> DiagnosisStore::findInstanceItems(const InstancePath& path) {
  Items found;
  appendFound(found, g_instanceById, pathIds(path));
  appendFound(found, g_instanceByName, pathNames(path));
  return found;
}

std::vector<const DiagnosisItem*> DiagnosisStore::findNetItems(const InstancePath& path,
                                                               const std::string& terminal) {
  Items found;
  appendFound(found, g_netById, std::tuple{pathIds(path), terminal});
  appendFound(found, g_netByName, std::tuple{pathNames(path), terminal});
  return found;
}

ImU32 DiagnosisStore::colorForSeverity(DiagnosisSeverity sev) {
  switch (sev) {
    case DiagnosisSeverity::Error:   return IM_COL32(230,  60,  60, 255);
    case DiagnosisSeverity::Warning: return IM_COL32(230, 170,  40, 255);
    case DiagnosisSeverity::Info:    return IM_COL32( 70, 160, 230, 255);
  }
  return 0;
}

ImU32 DiagnosisStore::instanceColor(const InstancePath& path) {
  auto items = instanceDiagnostics(path);
  if (items.empty()) return 0;
  return colorForSeverity(worstOf(items));
}

ImU32 DiagnosisStore::netColor(const InstancePath& path, const std::string& terminal) {
  auto items = netDiagnostics(path, terminal);
  if (items.empty()) return 0;
  return colorForSeverity(worstOf(items));
}

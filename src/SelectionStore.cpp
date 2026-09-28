#include "SelectionStore.h"

namespace {
InstancePath             g_selected;
bool                     g_hasSelection = false;
SelectionStore::Origin   g_origin       = SelectionStore::Origin::Tree;
unsigned                 g_revision     = 0;
std::function<void(const InstancePath&)> g_listener;
} // namespace

void SelectionStore::select(const InstancePath& path, Origin origin) {
  if (g_hasSelection && g_selected == path) return;
  g_selected     = path;
  g_hasSelection = true;
  g_origin       = origin;
  ++g_revision;
  if (g_listener) g_listener(path);
}

void SelectionStore::clear() {
  if (!g_hasSelection) return;
  g_selected.clear();
  g_hasSelection = false;
  ++g_revision;
}

bool SelectionStore::hasSelection() { return g_hasSelection; }
const InstancePath& SelectionStore::selected() { return g_selected; }
bool SelectionStore::isSelected(const InstancePath& path) {
  return g_hasSelection && g_selected == path;
}
SelectionStore::Origin SelectionStore::origin() { return g_origin; }
unsigned SelectionStore::revision() { return g_revision; }

void SelectionStore::setListener(std::function<void(const InstancePath&)> listener) {
  g_listener = std::move(listener);
}

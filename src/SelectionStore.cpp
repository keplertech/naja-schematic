#include "SelectionStore.h"

namespace {
InstancePath             g_selected;
bool                     g_hasSelection = false;
SchematicNetRef          g_selectedNet;
bool                     g_hasNetSelection = false;
SelectionStore::Origin   g_origin       = SelectionStore::Origin::Tree;
unsigned                 g_revision     = 0;
std::function<void(const InstancePath&)> g_listener;
} // namespace

void SelectionStore::select(const InstancePath& path, Origin origin) {
  if (g_hasSelection && g_selected == path) return;
  g_selected        = path;
  g_hasSelection    = true;
  g_selectedNet     = {};
  g_hasNetSelection = false;
  g_origin          = origin;
  ++g_revision;
  if (g_listener) g_listener(path);
}

void SelectionStore::selectNet(const SchematicNetRef& net) {
  if (g_hasNetSelection && g_selectedNet == net) return;
  g_selectedNet     = net;
  g_hasNetSelection = true;
  g_selected.clear();
  g_hasSelection    = false;
  g_origin          = Origin::Schematic;
  ++g_revision;
}

void SelectionStore::clear() {
  if (!g_hasSelection && !g_hasNetSelection) return;
  g_selected.clear();
  g_hasSelection    = false;
  g_selectedNet     = {};
  g_hasNetSelection = false;
  ++g_revision;
}

bool SelectionStore::hasSelection() { return g_hasSelection; }
const InstancePath& SelectionStore::selected() { return g_selected; }
bool SelectionStore::isSelected(const InstancePath& path) {
  return g_hasSelection && g_selected == path;
}
bool SelectionStore::hasNetSelection() { return g_hasNetSelection; }
const SchematicNetRef& SelectionStore::selectedNet() { return g_selectedNet; }
bool SelectionStore::isNetSelected(const SchematicNetRef& net) {
  return g_hasNetSelection && g_selectedNet == net;
}
SelectionStore::Origin SelectionStore::origin() { return g_origin; }
unsigned SelectionStore::revision() { return g_revision; }

void SelectionStore::setListener(std::function<void(const InstancePath&)> listener) {
  g_listener = std::move(listener);
}

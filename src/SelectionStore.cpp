#include "SelectionStore.h"

namespace {
std::string              g_selected;
bool                     g_hasSelection = false;
SelectionStore::Origin   g_origin       = SelectionStore::Origin::Tree;
unsigned                 g_revision     = 0;
std::function<void(const std::string&)> g_listener;
} // namespace

void SelectionStore::select(const std::string& pathKey, Origin origin) {
  if (g_hasSelection && g_selected == pathKey) return;
  g_selected     = pathKey;
  g_hasSelection = true;
  g_origin       = origin;
  ++g_revision;
  if (g_listener) g_listener(pathKey);
}

void SelectionStore::clear() {
  if (!g_hasSelection) return;
  g_selected.clear();
  g_hasSelection = false;
  ++g_revision;
}

bool SelectionStore::hasSelection() { return g_hasSelection; }
const std::string& SelectionStore::selected() { return g_selected; }
bool SelectionStore::isSelected(const std::string& pathKey) {
  return g_hasSelection && g_selected == pathKey;
}
SelectionStore::Origin SelectionStore::origin() { return g_origin; }
unsigned SelectionStore::revision() { return g_revision; }

void SelectionStore::setListener(std::function<void(const std::string&)> listener) {
  g_listener = std::move(listener);
}

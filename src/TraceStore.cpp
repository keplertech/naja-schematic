#include "TraceStore.h"

#include <algorithm>

namespace {
std::vector<Trace> g_traces;
int                g_nextId = 1;

// Teal, magenta, lime, cyan: bright enough to read on the dark canvas and
// distinct from the diagnosis severities and the selection blue.
constexpr ImU32 kPalette[TraceStore::kStyleCount] = {
  IM_COL32( 60, 200, 150, 255),
  IM_COL32(225,  90, 205, 255),
  IM_COL32(175, 215,  60, 255),
  IM_COL32( 60, 215, 235, 255),
};
constexpr ImU32 kConvergence = IM_COL32(245, 245, 245, 255);

Trace* findMutable(int id) {
  for (auto& t : g_traces)
    if (t.id == id) return &t;
  return nullptr;
}
} // namespace

std::string traceLabel(const InstancePath& instance, const std::string& pin) {
  return instance.empty() ? pin : displayPath(instance) + "/" + pin;
}

int TraceStore::begin(const std::string& label) {
  Trace t;
  t.id    = g_nextId++;
  t.label = label;
  // The first free style, so a new trace never repeats a color on screen
  // while one is still available.
  t.style = int(g_traces.size()) % kStyleCount;
  for (int s = 0; s < kStyleCount; ++s) {
    bool used = std::any_of(g_traces.begin(), g_traces.end(),
                            [s](const Trace& o) { return o.style == s; });
    if (!used) { t.style = s; break; }
  }
  g_traces.push_back(t);
  return t.id;
}

void TraceStore::remove(int id) {
  g_traces.erase(std::remove_if(g_traces.begin(), g_traces.end(),
                                [id](const Trace& t) { return t.id == id; }),
                 g_traces.end());
}

void TraceStore::clear() { g_traces.clear(); }

const std::vector<Trace>& TraceStore::traces() { return g_traces; }

const Trace* TraceStore::find(int id) { return findMutable(id); }

bool TraceStore::isVisible(int id) {
  const Trace* t = find(id);
  return t && t->visible;
}

void TraceStore::setVisible(int id, bool visible) {
  if (Trace* t = findMutable(id)) t->visible = visible;
}

void TraceStore::cycleStyle(int id) {
  if (Trace* t = findMutable(id)) t->style = (t->style + 1) % kStyleCount;
}

ImU32 TraceStore::color(int id) {
  const Trace* t = find(id);
  return t ? styleColor(t->style) : 0;
}

ImU32 TraceStore::styleColor(int style) {
  return kPalette[((style % kStyleCount) + kStyleCount) % kStyleCount];
}

ImU32 TraceStore::convergenceColor() { return kConvergence; }

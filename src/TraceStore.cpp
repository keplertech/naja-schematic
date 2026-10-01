#include "TraceStore.h"

#include <algorithm>

namespace {
std::vector<Trace> g_traces;
int                g_nextId = 1;

// Teal, magenta, lime, cyan: distinct from the diagnosis severities and the
// selection blue.
constexpr ImU32 kPalette[TraceStore::kStyleCount] = {
  IM_COL32( 60, 200, 150, 255),
  IM_COL32(225,  90, 205, 255),
  IM_COL32(175, 215,  60, 255),
  IM_COL32( 60, 215, 235, 255),
};
// Near-black: stands out on the white "paper" canvas (and its SVG export)
// next to the gray of untraced wires.
constexpr ImU32 kConvergence = IM_COL32(30, 30, 30, 255);

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

bool TraceStore::setColor(int id, ImU32 color) {
  color |= IM_COL32_A_MASK;
  if (tooCloseToConvergence(color) || tooCloseToCanvas(color)) return false;
  Trace* t = findMutable(id);
  if (!t) return false;
  t->customColor = color;
  return true;
}

void TraceStore::resetColor(int id) {
  if (Trace* t = findMutable(id)) t->customColor.reset();
}

// Near-black: every channel dark. A deep but clearly tinted color (e.g. a
// dark blue, with a high blue) is still fine.
bool TraceStore::tooCloseToConvergence(ImU32 color) {
  constexpr unsigned kMax = 80;
  return (color >> IM_COL32_R_SHIFT & 0xFF) <= kMax &&
         (color >> IM_COL32_G_SHIFT & 0xFF) <= kMax &&
         (color >> IM_COL32_B_SHIFT & 0xFF) <= kMax;
}

// Near-white: every channel bright, unreadable on the white canvas. A pale
// but clearly tinted color (e.g. a light yellow, with a low blue) is fine.
bool TraceStore::tooCloseToCanvas(ImU32 color) {
  constexpr unsigned kMin = 190;
  return (color >> IM_COL32_R_SHIFT & 0xFF) >= kMin &&
         (color >> IM_COL32_G_SHIFT & 0xFF) >= kMin &&
         (color >> IM_COL32_B_SHIFT & 0xFF) >= kMin;
}

ImU32 TraceStore::color(int id) {
  const Trace* t = find(id);
  if (!t) return 0;
  return t->customColor ? *t->customColor : styleColor(t->style);
}

ImU32 TraceStore::styleColor(int style) {
  return kPalette[((style % kStyleCount) + kStyleCount) % kStyleCount];
}

ImU32 TraceStore::convergenceColor() { return kConvergence; }

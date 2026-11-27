#include "DemoRecorder.h"

// Script format (JSON):
// {
//   "output": "demo.mp4",           // H.264 through ffmpeg, no audio
//   "size": [1920, 980], "fps": 30,
//   "ui_scale": 1.5, "theme": "dark",
//   "left_width": 360,               // the hierarchy column, pixels
//   "schematic_share": 0.8,          // of the right column's height
//   "caption_size": 34,              // pixels
//   "stills": [{"step": 4, "path": "still.png"}],   // a step's last frame
//   "steps": [
//     { "do": [ ...actions ],        // run first; the view then settles,
//                                    // not recorded
//       "camera": "fit" | {"instance": [ids], "margin": 0.1}
//                 | {"instances": [[ids], ...], "margin": 0.1},
//       "transition": "move" | "crossfade" | "cut",   // default cut
//       "duration": 1.0,             // seconds recorded for this step
//       "caption": "text" }          // "" hides it; absent keeps it
//   ]
// }
// Actions: {"open": [ids]}, {"close": [ids]},
// {"trace": {"path": [ids], "term_id": n, "bits": [..], "label": "..",
//            "color": "#rrggbb"}},
// {"equipotential": {"path", "term_id", "bit", "label", "color"}} (a one-net
// trace), {"select": [ids] | null}, {"style": [style items]},
// {"clear_traces": true}, {"clear": true} (View > Clear nets),
// {"top_view": true} (the top design's sheet), {"sources": [ids]} (Show
// Sources on an instance), {"panel": share} (the schematic's share of the
// height, the tabs taking the rest).
// "move" goes from the previous step's camera to this one's (zoom on a log
// scale, eased); "crossfade" blends the previous step's last frame into
// this one's, with the camera already on its rect.

#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <optional>
#include <vector>

#include <imgui.h>
#include <SDL.h>
#include <SDL_opengl.h>
#include <nlohmann/json.hpp>

#include "AppLogic.h"
#include "EquipotentialView.h"
#include "GUIData.h"
#include "INetlistProvider.h"
#include "PngWriter.h"
#include "SchematicView.h"
#include "SelectionStore.h"
#include "ShowNetStore.h"
#include "StyleStore.h"
#include "Theme.h"
#include "TraceStore.h"
#include "UiScale.h"

#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#define GL_RENDERBUFFER 0x8D41
#define GL_COLOR_ATTACHMENT0 0x8CE0
#endif

using json = nlohmann::json;

namespace {

using Rect = std::pair<ImVec2, ImVec2>;

struct Step {
  json actions = json::array();
  json camera;                 // null: keep the previous step's
  std::string transition = "cut";
  float duration = 1.0f;
  std::optional<std::string> caption;
};

struct Still { int step; std::string path; };

struct Recorder {
  std::string output;
  int w = 1920, h = 980, fps = 30;
  float uiScale = 1.0f;
  std::optional<ThemeId> theme;
  float leftWidth = 300.0f, schematicShare = 0.7f, captionSize = 34.0f;
  std::vector<Step> steps;
  std::vector<Still> stills;

  AppState* app = nullptr;
  FILE* ffmpeg = nullptr;
  unsigned fbo = 0, rbo = 0;

  enum class Phase { Loading, Settle, Play, Done } phase = Phase::Loading;
  int stepIndex = -1;
  int stableFrames = 0, settleFrames = 0;
  size_t lastSignature = 0;
  int playFrame = 0, playFrames = 0;
  std::optional<Rect> prevCamera, camera;
  std::vector<uint8_t> frame, fadeFrom;   // RGB, top row first
  std::string caption, prevCaption;
  int captionAge = 1 << 20;               // frames since it changed
  int exitStatus = 1;
};

Recorder g;

using GlGenFn   = void (*)(int, unsigned*);
using GlBindFn  = void (*)(unsigned, unsigned);
using GlStoreFn = void (*)(unsigned, unsigned, int, int);
using GlAttFn   = void (*)(unsigned, unsigned, unsigned, unsigned);

void setupFramebuffer() {
  auto genF  = (GlGenFn)SDL_GL_GetProcAddress("glGenFramebuffers");
  auto bindF = (GlBindFn)SDL_GL_GetProcAddress("glBindFramebuffer");
  auto genR  = (GlGenFn)SDL_GL_GetProcAddress("glGenRenderbuffers");
  auto bindR = (GlBindFn)SDL_GL_GetProcAddress("glBindRenderbuffer");
  auto store = (GlStoreFn)SDL_GL_GetProcAddress("glRenderbufferStorage");
  auto att   = (GlAttFn)SDL_GL_GetProcAddress("glFramebufferRenderbuffer");
  genF(1, &g.fbo);
  bindF(GL_FRAMEBUFFER, g.fbo);
  genR(1, &g.rbo);
  bindR(GL_RENDERBUFFER, g.rbo);
  store(GL_RENDERBUFFER, GL_RGBA8, g.w, g.h);
  att(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, g.rbo);
}

void bindFramebuffer() {
  static auto bindF = (GlBindFn)SDL_GL_GetProcAddress("glBindFramebuffer");
  bindF(GL_FRAMEBUFFER, g.fbo);
}

std::vector<unsigned> idsOf(const json& j) {
  std::vector<unsigned> ids;
  if (j.is_array())
    for (const auto& v : j) ids.push_back(v.get<unsigned>());
  return ids;
}

// The shape (box, frame or flag) at these ids, as drawn now.
const InstanceShape* findShape(const std::vector<unsigned>& ids) {
  const auto& view = EquipotentialView::schematicForTesting();
  const InstanceShape* found = nullptr;
  for (const auto& inst : view.instances) {
    if (inst.path.size() != ids.size()) continue;
    bool same = true;
    for (size_t i = 0; i < ids.size() && same; ++i) same = inst.path[i].id == ids[i];
    if (!same) continue;
    if (!found || (found->isHierGroup && !inst.isHierGroup)) found = &inst;
  }
  return found;
}

InstancePath pathOf(const std::vector<unsigned>& ids) {
  // Names come from what's drawn when they can (display only: paths
  // compare by id).
  InstancePath path;
  for (size_t i = 0; i < ids.size(); ++i) path.push_back({ids[i], {}});
  for (size_t n = ids.size(); n > 0; --n) {
    std::vector<unsigned> prefix(ids.begin(), ids.begin() + n);
    if (const InstanceShape* s = findShape(prefix)) {
      for (size_t i = 0; i < n; ++i) path[i].name = s->path[i].name;
      break;
    }
  }
  return path;
}

std::optional<Rect> resolveCamera(const json& spec) {
  const auto& view = EquipotentialView::schematicForTesting();
  if (spec.is_string() && spec == "fit") {
    ImVec2 a, b;
    if (!view.computeWorldBounds(a, b)) return std::nullopt;
    return Rect{a, b};
  }
  if (!spec.is_object()) return std::nullopt;
  json list = json::array();
  if (spec.contains("instance"))  list.push_back(spec["instance"]);
  if (spec.contains("instances")) for (const auto& p : spec["instances"]) list.push_back(p);
  std::optional<Rect> r;
  for (const auto& p : list) {
    const InstanceShape* s = findShape(idsOf(p));
    if (!s) return std::nullopt;
    ImVec2 a, b;
    shapeWorldExtent(*s, a, b);
    if (!r) r = Rect{a, b};
    else    r = Rect{ImVec2(std::min(r->first.x, a.x), std::min(r->first.y, a.y)),
                     ImVec2(std::max(r->second.x, b.x), std::max(r->second.y, b.y))};
  }
  if (!r) return std::nullopt;
  const float m = spec.value("margin", 0.0f);
  const float dx = (r->second.x - r->first.x) * m, dy = (r->second.y - r->first.y) * m;
  return Rect{ImVec2(r->first.x - dx, r->first.y - dy), ImVec2(r->second.x + dx, r->second.y + dy)};
}

// A rect as the view fits it: centre and scale on the canvas.
struct Cam { float cx, cy, scale; };
constexpr float kPad = 60.0f;   // EquipotentialView's fit padding

Cam toCam(const Rect& r) {
  const ImVec2 c = EquipotentialView::canvasSizeForTesting();
  const float w = std::max(1.0f, r.second.x - r.first.x), h = std::max(1.0f, r.second.y - r.first.y);
  const float s = std::min((c.x - 2 * kPad) / w, (c.y - 2 * kPad) / h);
  return {(r.first.x + r.second.x) / 2, (r.first.y + r.second.y) / 2, s};
}

Rect fromCam(const Cam& cam) {
  const ImVec2 c = EquipotentialView::canvasSizeForTesting();
  const float hw = (c.x - 2 * kPad) / cam.scale / 2, hh = (c.y - 2 * kPad) / cam.scale / 2;
  return {ImVec2(cam.cx - hw, cam.cy - hh), ImVec2(cam.cx + hw, cam.cy + hh)};
}

float ease(float t) { return t * t * (3 - 2 * t); }

Rect interpolate(const Rect& a, const Rect& b, float t) {
  const Cam ca = toCam(a), cb = toCam(b);
  const float e = ease(t);
  // Zoom on a log scale; the centre follows the zoom so the move reads as
  // one motion (the screen point moving towards the target stays steady).
  const float ls = std::log(ca.scale) + (std::log(cb.scale) - std::log(ca.scale)) * e;
  const float s = std::exp(ls);
  float k = e;
  if (std::fabs(cb.scale - ca.scale) > 1e-4f * ca.scale)
    k = (1.0f / ca.scale - 1.0f / s) / (1.0f / ca.scale - 1.0f / cb.scale);
  return fromCam({ca.cx + (cb.cx - ca.cx) * k, ca.cy + (cb.cy - ca.cy) * k, s});
}

size_t signature() {
  const auto& view = EquipotentialView::schematicForTesting();
  size_t h = view.instances.size() * 1315423911u + view.routes.size();
  auto mix = [&h](float v) { h = h * 31 + std::hash<float>{}(std::round(v * 4) / 4); };
  for (const auto& i : view.instances) { mix(i.x); mix(i.y); mix(i.w); mix(i.h); }
  for (const auto& r : view.routes) h = h * 31 + r.segments.size() + (size_t)r.color;
  return h;
}

void runActions(const json& actions) {
  for (const auto& a : actions) {
    if (a.contains("open")) {
      EquipotentialView::openInstance(pathOf(idsOf(a["open"])));
    } else if (a.contains("close")) {
      EquipotentialView::closeInstance(pathOf(idsOf(a["close"])));
    } else if (a.contains("trace")) {
      const json& t = a["trace"];
      json req;
      req["request"] = "trace_driver";
      req["path"]    = t.value("path", json::array());
      req["term_id"] = t.value("term_id", 0u);
      if (t.contains("bits")) {
        if (t["bits"].size() == 1) req["bit"] = t["bits"][0];
        else                       req["bits"] = t["bits"];
      }
      const int id = TraceStore::begin(t.value("label", std::string("trace")));
      if (t.contains("color"))
        if (auto c = StyleStore::parseColor(t["color"].get<std::string>())) TraceStore::setColor(id, *c);
      req["trace_id"] = id;
      g.app->provider->send(req.dump());
    } else if (a.contains("select")) {
      if (a["select"].is_null()) SelectionStore::clear();
      else SelectionStore::select(pathOf(idsOf(a["select"])), SelectionStore::Origin::Host);
    } else if (a.contains("style")) {
      std::vector<std::string> errors;
      auto items = StyleStore::parse(a["style"], &errors);
      for (const auto& e : errors) std::cerr << "style: " << e << "\n";
      StyleStore::setStyles(std::move(items));
    } else if (a.contains("equipotential")) {
      const json& t = a["equipotential"];
      json req;
      req["request"] = "load_equipotential";
      req["path"]    = t.value("path", json::array());
      req["term_id"] = t.value("term_id", 0u);
      if (t.contains("bit")) req["bit"] = t["bit"];
      const int id = TraceStore::begin(t.value("label", std::string("net")));
      if (t.contains("color"))
        if (auto c = StyleStore::parseColor(t["color"].get<std::string>())) TraceStore::setColor(id, *c);
      req["trace_id"] = id;
      g.app->provider->send(req.dump());
    } else if (a.contains("sources")) {
      // Show Sources on an instance: the Source tab, grown to show the code.
      json req;
      req["request"] = "get_sources";
      req["kind"]    = "instance";
      writePath(req, pathOf(idsOf(a["sources"])));
      // A selection's Properties tab, still pending while the tabs were
      // hidden, would otherwise take the panel.
      g.app->focusPropertiesTab = false;
      g.app->provider->send(req.dump());
    } else if (a.contains("panel")) {
      g.app->requestedSchematicShare = a["panel"].get<float>();
    } else if (a.contains("clear")) {
      // View > Clear nets.
      ShowNetStore::forget();
      g.app->guiData->clearEquipotentials();
      EquipotentialView::resetLayout();   // the top view's port flags too
    } else if (a.contains("top_view")) {
      EquipotentialView::requestTopView(true, false);
    } else if (a.contains("clear_traces")) {
      std::vector<int> ids;
      for (const auto& t : TraceStore::traces()) ids.push_back(t.id);
      for (int id : ids) g.app->guiData->removeTrace(id);
    } else {
      std::cerr << "unknown action: " << a.dump() << "\n";
    }
  }
}

void startStep(int index) {
  g.stepIndex = index;
  if (index >= (int)g.steps.size()) { g.phase = Recorder::Phase::Done; return; }
  const Step& s = g.steps[index];
  g.prevCamera = g.camera;
  g.fadeFrom = g.frame;
  runActions(s.actions);
  g.phase = Recorder::Phase::Settle;
  g.stableFrames = 0;
  g.settleFrames = 0;
  if (s.caption && *s.caption != g.caption) {
    g.prevCaption = g.caption;
    g.caption = *s.caption;
    // The first step's caption is shown from its first frame: the video
    // loops back to it from the last one.
    g.captionAge = index == 0 ? 1 << 20 : 0;
  }
}

// Each frame, at the start of the UI: the step's actions and the camera.
void afterNewFrame() {
  ImGuiIO& io = ImGui::GetIO();
  io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
  if (g.phase == Recorder::Phase::Loading) {
    if (designLoading() || !EquipotentialView::hasSchematic()) return;
    const size_t sig = signature();
    g.stableFrames = sig == g.lastSignature ? g.stableFrames + 1 : 0;
    g.lastSignature = sig;
    if (g.stableFrames < 5) return;
    startStep(0);
  }
  if (g.phase == Recorder::Phase::Settle) {
    const Step& s = g.steps[g.stepIndex];
    if (!s.camera.is_null())
      if (auto r = resolveCamera(s.camera)) g.camera = r;
    if (g.camera) EquipotentialView::setCameraRect(g.camera);
    const size_t sig = signature();
    g.stableFrames = sig == g.lastSignature ? g.stableFrames + 1 : 0;
    g.lastSignature = sig;
    if (++g.settleFrames > 300) std::cerr << "step " << g.stepIndex << ": view never settled\n";
    if (g.stableFrames < 4 && g.settleFrames <= 300) return;
    g.phase = Recorder::Phase::Play;
    g.playFrame = 0;
    g.playFrames = std::max(1, (int)std::lround(s.duration * g.fps));
  }
  if (g.phase == Recorder::Phase::Play) {
    const Step& s = g.steps[g.stepIndex];
    std::optional<Rect> cam = g.camera;
    if (s.transition == "move" && g.prevCamera && g.camera)
      cam = interpolate(*g.prevCamera, *g.camera, std::min(1.0f, (g.playFrame + 1) / (float)g.playFrames));
    if (cam) EquipotentialView::setCameraRect(cam);
  }
}

void drawCaption(const std::string& text, float alpha) {
  if (text.empty() || alpha <= 0.0f) return;
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  ImFont* font = ImGui::GetFont();
  const float size = g.captionSize;
  const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str());
  const ImVec2 origin = EquipotentialView::canvasOriginForTesting();
  const ImVec2 canvas = EquipotentialView::canvasSizeForTesting();
  const float padX = size * 0.6f, padY = size * 0.35f, margin = size * 0.6f;
  const ImVec2 a(origin.x + margin, origin.y + canvas.y - margin - ts.y - 2 * padY);
  const ImVec2 b(a.x + ts.x + 2 * padX, a.y + ts.y + 2 * padY);
  const int al = (int)(alpha * 255);
  dl->AddRectFilled(a, b, IM_COL32(20, 22, 28, al * 220 / 255), size * 0.3f);
  dl->AddRect(a, b, IM_COL32(255, 255, 255, al * 60 / 255), size * 0.3f, 0, 1.5f);
  dl->AddText(font, size, ImVec2(a.x + padX, a.y + padY), IM_COL32(245, 245, 245, al), text.c_str());
}

void beforeRender() {
  if (g.phase != Recorder::Phase::Play) return;
  const int fade = std::max(1, g.fps * 4 / 10);
  const float t = std::min(1.0f, g.captionAge / (float)fade);
  drawCaption(g.prevCaption, 1.0f - t);
  drawCaption(g.caption, t);
  bindFramebuffer();
}

void writeFrame(const std::vector<uint8_t>& rgb) {
  if (g.ffmpeg) fwrite(rgb.data(), 1, rgb.size(), g.ffmpeg);
}

void afterDraw() {
  if (g.phase != Recorder::Phase::Play) return;
  // Read back, top row first.
  std::vector<uint8_t> raw((size_t)g.w * g.h * 3);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, g.w, g.h, GL_RGB, GL_UNSIGNED_BYTE, raw.data());
  g.frame.resize(raw.size());
  const size_t row = (size_t)g.w * 3;
  for (int y = 0; y < g.h; ++y)
    std::copy(raw.begin() + (g.h - 1 - y) * row, raw.begin() + (g.h - y) * row, g.frame.begin() + y * row);

  const Step& s = g.steps[g.stepIndex];
  std::vector<uint8_t> out = g.frame;
  if (s.transition == "crossfade" && g.fadeFrom.size() == out.size()) {
    const float t = ease(std::min(1.0f, (g.playFrame + 1) / (float)g.playFrames));
    for (size_t i = 0; i < out.size(); ++i)
      out[i] = (uint8_t)std::lround(g.fadeFrom[i] * (1 - t) + out[i] * t);
  }
  writeFrame(out);
  g.frame = out;
  ++g.captionAge;

  if (++g.playFrame >= g.playFrames) {
    for (const auto& still : g.stills) {
      if (still.step != g.stepIndex) continue;
      std::ofstream f(still.path, std::ios::binary);
      f << PngWriter::encodeRgb(g.frame, g.w, g.h);
      std::cerr << "Still written to " << still.path << "\n";
    }
    startStep(g.stepIndex + 1);
    if (g.phase == Recorder::Phase::Done && g.ffmpeg) {
      g.exitStatus = pclose(g.ffmpeg) == 0 ? 0 : 1;
      g.ffmpeg = nullptr;
      std::cerr << (g.exitStatus == 0 ? "Video written to " : "ffmpeg failed for ") << g.output << "\n";
    }
  }
}

void beforeNewFrame() {
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2((float)g.w, (float)g.h);
  io.DisplayFramebufferScale = ImVec2(1, 1);
  io.DeltaTime = 1.0f / g.fps;
}

} // namespace

namespace DemoRecorder {

bool load(const std::string& scriptPath) {
  std::ifstream f(scriptPath);
  if (!f) { std::cerr << "--record: cannot read " << scriptPath << "\n"; return false; }
  try {
    json j; f >> j;
    g.output = j.value("output", std::string("demo.mp4"));
    if (j.contains("size")) { g.w = j["size"][0]; g.h = j["size"][1]; }
    g.fps = j.value("fps", 30);
    g.uiScale = j.value("ui_scale", 1.0f);
    if (j.contains("theme")) g.theme = parseTheme(j["theme"].get<std::string>());
    g.leftWidth = j.value("left_width", 300.0f);
    g.schematicShare = j.value("schematic_share", 0.7f);
    g.captionSize = j.value("caption_size", 34.0f);
    for (const auto& s : j.value("stills", json::array()))
      g.stills.push_back({s.value("step", 0), s.value("path", std::string())});
    for (const auto& s : j.at("steps")) {
      Step step;
      step.actions = s.value("do", json::array());
      if (s.contains("camera")) step.camera = s["camera"];
      step.transition = s.value("transition", std::string("cut"));
      step.duration = s.value("duration", 1.0f);
      if (s.contains("caption")) step.caption = s["caption"].get<std::string>();
      g.steps.push_back(std::move(step));
    }
  } catch (const std::exception& e) {
    std::cerr << "--record: " << scriptPath << ": " << e.what() << "\n";
    return false;
  }
  return !g.steps.empty();
}

int width()  { return g.w; }
int height() { return g.h; }

void install(AppState& state) {
  g.app = &state;
  if (g.theme) setTheme(*g.theme);
  setUiScale(g.uiScale);
  state.initialLeftWidth = g.leftWidth;
  state.showLeftColumn = g.leftWidth > 0.0f;
  state.initialSchematicShare = g.schematicShare;
  setupFramebuffer();
  const std::string cmd =
      "ffmpeg -loglevel error -y -f rawvideo -pix_fmt rgb24 -s " + std::to_string(g.w) + "x" +
      std::to_string(g.h) + " -r " + std::to_string(g.fps) +
      " -i - -an -c:v libx264 -preset slow -crf 20 -pix_fmt yuv420p -movflags +faststart '" +
      g.output + "'";
  g.ffmpeg = popen(cmd.c_str(), "w");
  if (!g.ffmpeg) { std::cerr << "--record: cannot run ffmpeg\n"; g.phase = Recorder::Phase::Done; }
  state.hooks.beforeNewFrame = beforeNewFrame;
  state.hooks.afterNewFrame  = afterNewFrame;
  state.hooks.beforeRender   = beforeRender;
  state.hooks.afterDraw      = afterDraw;
}

bool done() { return g.phase == Recorder::Phase::Done; }
int status() { return g.exitStatus; }

} // namespace DemoRecorder

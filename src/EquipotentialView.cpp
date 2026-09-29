// EquipotentialView.cpp — merged instances, horizontal expansion
#include "EquipotentialView.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <tuple>
#include <imgui.h>

#include "Types.h"
#include "SchematicView.h"
#include "INetlistProvider.h"
#include "DiagnosisStore.h"
#include "SchematicLayout.h"
#include "SchematicInteraction.h"
#include "SelectionStore.h"

// Net/instance placement and hierarchy frames live in SchematicLayout
// (pure geometry, unit-tested); this file turns them into drawn shapes.
using namespace SchematicLayout;

// ---------------------------------------------------------------------------
// Layout geometry constants (see SchematicLayout.h for the placement ones)
// ---------------------------------------------------------------------------
// Hierarchy embedding (nested boxes) geometry.
static constexpr float kHierChildW    = 110.0f;
static constexpr float kHierChildH    = 44.0f;
static constexpr float kHierGap       = 14.0f;
static constexpr float kHierMargin    = 16.0f;
static constexpr float kHierHeaderGap = 26.0f; // room below the box's own label/ports

// ---------------------------------------------------------------------------
// Static view state
// ---------------------------------------------------------------------------
static SchematicView      g_schematic;
static int                g_pendingZoomSteps = 0;
static bool               g_pendingFit       = false;
static bool               g_pendingClear     = false;
// Set when a pin click requests a net: the view it extends keeps the
// user's current pan/zoom instead of re-fitting to the whole drawing once
// the net arrives. Consumed by the next item-count change.
static bool               g_skipNextAutoFit  = false;
// The instance whose pin was clicked, and where it was: once the net arrives
// the view is panned so that box stays put on screen, even though adding a
// net can re-lay out everything (hierarchy frames grow and shift).
static InstancePath       g_keepInPlacePath;
static ImVec2             g_keepInPlaceWorld{};
// Pin nets requested by a click and not arrived yet (keyed by
// SchematicInteraction::pinRequestKey), so a second click doesn't re-send.
static SchematicInteraction::PendingRequests g_pendingPinNets;
// Instance box whose "double-click to show all pins" hint is due, and since
// when the cursor has rested on it.
static int                g_hintShapeId      = -1;
static double             g_hintSince        = 0.0;
// Screen position of the canvas's top-left corner, last frame.
static ImVec2             g_canvasOrigin{};
static INetlistProvider*  g_provider         = nullptr;
// Which instance box (if any) the canvas right-click popup currently
// targets; -1 = the canvas-level menu (Clear all nets / Fit view).
static int                g_ctxInstanceId    = -1;
// Which pin (if any) the popup targets; takes precedence over the instance
// box the pin sits on. -1 = none.
static int                g_ctxPortId        = -1;

struct OccurrenceInfo { InstancePath path; DesignRef designRef; std::optional<SourceLoc> sourceLoc; };
struct PortEquiRequest {
    std::vector<unsigned> pathIds;
    unsigned              termId = 0;
    std::optional<int>    bit;
};
static std::map<int, OccurrenceInfo>  g_occInfoByShapeId;
static std::map<int, PortEquiRequest> g_portEquiByPortId;
// One bus pin group: (instance, bus base name, is input).
using BusGroupKey = std::tuple<InstancePath, std::string, bool>;
// Merged bus-pin port id -> the bus group it expands on click.
// Checked before g_portEquiByPortId. Rebuilt every frame.
static std::map<int, BusGroupKey> g_busGroupByPortId;
// Bit port id of a bus shown expanded -> its bus group, for the pin
// context menu's "Collapse Bus". Clicking an open bit loads its net like any
// scalar pin. Rebuilt every frame.
static std::map<int, BusGroupKey> g_expandedBusGroupByPortId;

static std::map<InstancePath, std::vector<EquipotentialView::ExpandedPort>> g_expandedInstances;
static std::set<InstancePath> g_pendingExpansions;
// Bus groups currently shown expanded (individual bit pins/wires) instead of
// merged into one pin/wire.
static std::set<BusGroupKey> g_expandedBuses;

// --- Hierarchy embedding (nested boxes) ---
// Instances (== InstanceShape::path) currently toggled open to show their
// internal sub-instances nested inside their box.
static std::set<InstancePath> g_hierExpanded;
// Instances with an in-flight load_instance_internals request.
static std::set<InstancePath> g_hierPending;
// Instances whose internals have been loaded (children + internal nets).
static std::map<InstancePath, EquipotentialView::InstanceInternals> g_instanceInternals;

// Hierarchy grouping: when on, the leaf instances shown are laid out inside
// nested frames standing for the hierarchical modules that contain them
// (see SchematicLayout::layoutHierarchyGroups) instead of the flat column layout.
static bool g_showHierarchy = true;
// A "Zoom to Module" request from the canvas context menu: the module path
// of the frame to fit the view to once this frame's layout is known.
static std::optional<InstancePath> g_pendingZoomGroup;

// Instances shown on their own (showInstance), by instance path.
static std::map<InstancePath, EquipotentialView::StartInstance> g_startInstances;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Strip a trailing bus-bit suffix ("Q[3]" -> "Q") so a pin label can be
// matched against DiagnosisItem::terminal, which is always the base name.
static std::string stripBusIndex(const std::string& label) {
    auto pos = label.rfind('[');
    return pos == std::string::npos ? label : label.substr(0, pos);
}


// Summarized label for a collapsed bus pin, e.g. "A[7:0]" for a contiguous
// range or "A (*3)" for a sparse/partial set of loaded bits.
static std::string busLabel(const std::string& base, std::vector<int> bits) {
    if (bits.empty()) return base;
    std::sort(bits.begin(), bits.end());
    bool contiguous = true;
    for (size_t i = 1; i < bits.size(); ++i)
        if (bits[i] != bits[i - 1] + 1) { contiguous = false; break; }
    if (contiguous)
        return base + "[" + std::to_string(bits.back()) + ":" + std::to_string(bits.front()) + "]";
    return base + " (*" + std::to_string(bits.size()) + ")";
}

static float portLy(int i, int n) {
    return n > 1 ? -0.4f + 0.8f * float(i) / float(n - 1) : 0.0f;
}

// Sizes `inst` as its symbol (SchematicLayout::symbolGeometry) and puts its
// pins on the pin grid: inputs down the left side, the rest down the right,
// each in their current order. `extraH` grows the box below its pins, to
// make room for nested internals.
static void applySymbolGeometry(InstanceShape& inst, float extraH) {
    std::vector<std::string> left, right;
    for (const auto& p : inst.ports) (p.lx < 0.f ? left : right).push_back(p.name);
    const SymbolGeometry g = symbolGeometry(inst.primitiveType, left, right);
    inst.w = g.w;
    inst.h = g.h + extraH;
    size_t li = 0, ri = 0;
    for (auto& p : inst.ports) {
        const float y = p.lx < 0.f ? g.leftY[li++] : g.rightY[ri++];
        p.ly = y / inst.h - 0.5f;
    }
}

// Screen-space mouse position converted to schematic world coordinates.
static ImVec2 mouseWorldPos(const SchematicView& sv, const ImVec2& cpos) {
    float s = sv.transform.scale;
    ImVec2 m = ImGui::GetMousePos();
    return ImVec2(
        (m.x - cpos.x) / s + sv.transform.offset.x - sv.transform.screenOrigin.x / s,
        (m.y - cpos.y) / s + sv.transform.offset.y - sv.transform.screenOrigin.y / s);
}

// ---------------------------------------------------------------------------
// Pin interactions. A left click on a pin does the one thing that pin
// offers -- the same thing its hover tooltip announces:
//   - merged bus pin  -> show its individual bits (no request needed)
//   - open pin        -> add its net to the view (load_equipotential)
// A pin whose net is already shown has no click action (its right-click menu
// still offers Trace to Driver). Pins are hit-tested nearest-first with
// SchematicInteraction::pickPin against what was drawn last frame.
// ---------------------------------------------------------------------------
enum class PinAction { None, ExpandBus, LoadNet, Loading };

static PinAction pinAction(const Port& p) {
    if (g_busGroupByPortId.count(p.id)) return PinAction::ExpandBus;
    if (p.open) return p.pending ? PinAction::Loading : PinAction::LoadNet;
    return PinAction::None;
}

static SchematicInteraction::PinRequestKey pinRequestKey(const PortEquiRequest& r) {
    return SchematicInteraction::pinRequestKey(r.pathIds, r.termId, r.bit);
}

// Sends load_equipotential for `port` (sitting on `inst`) unless it's already
// in flight, and arranges for `inst` to stay where it is on screen when the
// net arrives.
static void requestPinNet(const InstanceShape& inst, const Port& port) {
    auto it = g_portEquiByPortId.find(port.id);
    if (!g_provider || it == g_portEquiByPortId.end()) return;
    if (!g_pendingPinNets.begin(pinRequestKey(it->second), ImGui::GetTime())) return;
    json j;
    j["request"] = "load_equipotential";
    j["path"]    = it->second.pathIds;
    j["term_id"] = it->second.termId;
    if (it->second.bit.has_value()) j["bit"] = it->second.bit.value();
    g_provider->send(j.dump());
    g_skipNextAutoFit  = true;
    g_keepInPlacePath  = inst.path;
    g_keepInPlaceWorld = ImVec2(inst.x, inst.y);
}

// Runs the click action of the pin under `wp`, if any. Returns true when a
// pin was hit (even one with no action), so the click isn't also treated as
// a click on the box body underneath.
static bool clickPin(const ImVec2& wp) {
    auto hit = SchematicInteraction::pickPin(g_schematic.instances, wp, g_schematic.transform.scale);
    if (!hit) return false;
    InstanceShape* inst = g_schematic.findInstanceById(hit->instanceId);
    Port* port = inst ? g_schematic.findPortById(*inst, hit->portId) : nullptr;
    if (!port) return true;
    switch (pinAction(*port)) {
        case PinAction::ExpandBus: g_expandedBuses.insert(g_busGroupByPortId[port->id]); break;
        case PinAction::LoadNet:   requestPinNet(*inst, *port); break;
        default: break;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Hierarchy embedding: lay out and wire one expanded instance's internals
// (its direct sub-instances plus the nets connecting them) nested inside its
// box, recursing into any of those children that are themselves expanded and
// already loaded. Positions/sizes are written directly as world coordinates
// (nested inside `parent`'s current box) rather than a local frame, so
// SchematicView's existing flat draw/hit-test code needs no changes.
// ---------------------------------------------------------------------------
struct HierEmitResult {
    std::vector<InstanceShape> shapes;
    std::vector<NetWire>       nets;
};

static Port makeHierPort(int& nextPortId, const std::string& name, Direction dir) {
    Port p;
    p.id        = nextPortId++;
    p.name      = name;
    p.direction = dir;
    bool isIn   = (dir == Direction::Input);
    p.lx        = isIn ? -0.5f : 0.5f;
    p.isInput   = !isIn;
    return p;
}

static HierEmitResult emitInstanceInternals(InstanceShape& parent, int& nextInstId, int& nextPortId) {
    HierEmitResult result;
    auto dataIt = g_instanceInternals.find(parent.path);
    if (dataIt == g_instanceInternals.end()) return result;
    const auto& data = dataIt->second;

    // Sub-pass 1: create each child shape (id + identity) up front so the
    // wiring pass below has a complete child_id -> shape id map, regardless
    // of vertical order.
    std::vector<InstanceShape> children;
    children.reserve(data.children.size());
    std::map<unsigned, int> childIdToShapeId;
    for (const auto& c : data.children) {
        InstanceShape cs;
        cs.id            = nextInstId++;
        cs.path          = parent.path;
        cs.path.push_back({c.childId, c.name});
        cs.name          = displayPath(cs.path);
        cs.primitiveType = c.primitiveType;
        cs.w             = kHierChildW;
        cs.h             = kHierChildH;
        cs.parentShapeId = parent.id;
        cs.hasChildren   = c.hasInstances;
        cs.hierExpanded  = c.hasInstances && g_hierExpanded.count(cs.path) > 0;
        cs.diagOutline   = DiagnosisStore::instanceColor(cs.path);
        childIdToShapeId[c.childId] = cs.id;
        g_occInfoByShapeId[cs.id] = { cs.path, c.designRef };
        children.push_back(std::move(cs));
    }

    // Sub-pass 2: wire internal nets. Each pin resolves to either a child's
    // port (allocated lazily here) or one of the parent's own already-built
    // boundary ports (matched by stripped name + direction).
    std::map<int, std::vector<Port>> portsByShapeId;
    auto findOrAddChildPort = [&](unsigned instChildId, const std::string& name,
                                  Direction dir) -> std::pair<int, int> {
        auto sIt = childIdToShapeId.find(instChildId);
        if (sIt == childIdToShapeId.end()) return {-1, -1};
        int shapeId = sIt->second;
        auto& ports = portsByShapeId[shapeId];
        for (const auto& p : ports)
            if (p.name == name && p.direction == dir) return {shapeId, p.id};
        ports.push_back(makeHierPort(nextPortId, name, dir));
        return {shapeId, ports.back().id};
    };

    for (const auto& net : data.nets) {
        std::vector<std::pair<int, int>> ends;
        for (const auto& pin : net.pins) {
            if (pin.instChildId.has_value()) {
                auto e = findOrAddChildPort(*pin.instChildId, pin.name, pin.direction);
                if (e.first >= 0) ends.push_back(e);
            } else {
                for (auto& pp : parent.ports) {
                    if (stripBusIndex(pp.name) == stripBusIndex(pin.name) && pp.direction == pin.direction) {
                        ends.push_back({parent.id, pp.id});
                        break;
                    }
                }
            }
        }
        if (ends.size() < 2) continue;
        std::string netLabel = net.name;
        if (net.bit.has_value()) netLabel += "[" + std::to_string(*net.bit) + "]";
        for (size_t i = 1; i < ends.size(); ++i) {
            NetWire nw;
            nw.id               = int(result.nets.size()) + 1;
            nw.srcInstance      = ends[0].first;
            nw.srcPortId        = ends[0].second;
            nw.dstInstance      = ends[i].first;
            nw.dstPortId        = ends[i].second;
            nw.containerShapeId = parent.id;
            nw.netName          = netLabel;
            result.nets.push_back(nw);
        }
    }

    // Sub-pass 3: assign each child's port rows (left = inputs, right =
    // outputs, same convention as every other InstanceShape).
    for (auto& cs : children) {
        auto pit = portsByShapeId.find(cs.id);
        if (pit == portsByShapeId.end()) continue;
        auto& ports = pit->second;
        int nL = 0, nR = 0;
        for (const auto& p : ports) (p.lx < 0.f ? nL : nR)++;
        int li = 0, ri = 0;
        for (auto& p : ports) {
            p.ly    = (p.lx < 0.f) ? portLy(li++, nL) : portLy(ri++, nR);
            p.color = DiagnosisStore::netColor(cs.path, stripBusIndex(p.name));
        }
        cs.ports = std::move(ports);
    }

    // Sub-pass 4: position children top-to-bottom in a single column,
    // recursing into an already-expanded child *before* moving on to the
    // next sibling so a grown height pushes later siblings down correctly.
    // Below the parent's own pin rows (applySymbolGeometry reserved the room).
    float header = kHierHeaderGap;
    for (const auto& p : parent.ports)
        header = std::max(header, (p.ly + 0.5f) * parent.h + kPinPitch);
    std::vector<InstanceShape> descendants;
    float x = parent.x + kHierMargin;
    float y = parent.y + header;
    for (auto& cs : children) {
        cs.x = x;
        cs.y = y;
        if (cs.hierExpanded && g_instanceInternals.count(cs.path)) {
            auto sub = emitInstanceInternals(cs, nextInstId, nextPortId);
            descendants.insert(descendants.end(), sub.shapes.begin(), sub.shapes.end());
            result.nets.insert(result.nets.end(), sub.nets.begin(), sub.nets.end());
        }
        y += cs.h + kHierGap;
    }

    float bottom = children.empty() ? (parent.y + header) : (y - kHierGap);
    parent.h = std::max(parent.h, (bottom - parent.y) + kHierMargin);
    parent.w = std::max(parent.w, kHierChildW + 2.0f * kHierMargin);

    result.shapes = std::move(children);
    result.shapes.insert(result.shapes.end(), descendants.begin(), descendants.end());
    return result;
}


// Sends expand_instance_terms for an instance, unless one is in flight.
static void requestExpansion(const InstancePath& path, const DesignRef& designRef) {
    if (!g_provider || g_pendingExpansions.count(path)) return;
    g_pendingExpansions.insert(path);
    json req;
    req["request"]                  = "expand_instance_terms";
    writePath(req, path, "instance_path", "instance_id_path");
    req["design_ref"]["db_id"]      = designRef.db_id;
    req["design_ref"]["library_id"] = designRef.library_id;
    req["design_ref"]["design_id"]  = designRef.design_id;
    g_provider->send(req.dump());
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void EquipotentialView::zoomIn()    { g_pendingZoomSteps++; }
void EquipotentialView::zoomOut()   { g_pendingZoomSteps--; }
void EquipotentialView::fitView()   { g_pendingFit = true; }
void EquipotentialView::clearNets() { g_pendingClear = true; }

void EquipotentialView::resetLayout() {
    g_startInstances.clear();
    g_expandedInstances.clear();
    g_pendingExpansions.clear();
    g_expandedBuses.clear();
    g_hierExpanded.clear();
    g_hierPending.clear();
    g_instanceInternals.clear();
    g_skipNextAutoFit = false;
    g_keepInPlacePath.clear();
    g_pendingPinNets.clear();
    g_pendingFit   = true;
}

bool EquipotentialView::takePendingClear() {
    if (!g_pendingClear) return false;
    g_pendingClear = false;
    resetLayout();
    return true;
}

void EquipotentialView::setProvider(INetlistProvider* p) { g_provider = p; }

bool EquipotentialView::showHierarchy() { return g_showHierarchy; }

const SchematicView& EquipotentialView::schematicForTesting() { return g_schematic; }
ImVec2 EquipotentialView::canvasOriginForTesting() { return g_canvasOrigin; }

void EquipotentialView::setShowHierarchy(bool on) {
    if (g_showHierarchy == on) return;
    g_showHierarchy = on;
    g_pendingFit    = true;
}

void EquipotentialView::showInstance(const StartInstance& start) {
    if (start.path.empty()) return;  // the top design has no box of its own
    g_startInstances[start.path]    = start;
    g_expandedInstances[start.path] = start.ports;
}

std::optional<EquipotentialView::StartInstance>
EquipotentialView::startInstanceFromResolved(const json& reply) {
    if (!reply.value("found", false) || !reply.contains("instance") || !reply["instance"].is_object())
        return std::nullopt;
    const auto& inst = reply["instance"];
    StartInstance start;
    for (const auto& level : inst.value("path", json::array())) {
        if (!level.is_array() || level.size() < 2) continue;
        start.path.push_back({level[1].get<unsigned>(), level[0].get<std::string>()});
        start.pathIds.push_back(level[1].get<unsigned>());
        start.pathModels.push_back(level.size() > 2 ? level[2].get<std::string>() : "");
    }
    if (start.path.empty()) return std::nullopt;
    if (inst.contains("design_ref")) start.designRef = inst["design_ref"].get<DesignRef>();
    start.primitiveType = primitiveTypeFromString(inst.value("primitive_type", std::string("unknown")));
    start.hasInstances = inst.value("has_instances", false);
    if (inst.contains("source_loc") && inst["source_loc"].is_object()) {
        const auto& loc = inst["source_loc"];
        start.sourceLoc = SourceLoc{loc.value("file", std::string("")), loc.value("line", 0),
                                    loc.value("end_line", 0), loc.value("column", 0),
                                    loc.value("end_column", 0)};
    }
    for (const auto& t : inst.value("terms", json::array())) {
        ExpandedPort ep;
        ep.name    = t.value("name", std::string(""));
        ep.childId = t.value("child_id", 0u);
        if (t.contains("bit") && !t["bit"].is_null()) ep.bit = t["bit"].get<int>();
        ep.clock   = t.value("clock", false);
        int dirInt   = t.value("direction", 0);
        ep.direction = dirInt == 1 ? Direction::Output
                     : dirInt == 2 ? Direction::Inout : Direction::Input;
        start.ports.push_back(std::move(ep));
    }
    return start;
}

void EquipotentialView::applyInstanceExpansion(
        const InstancePath& path,
        const std::vector<ExpandedPort>& ports) {
    g_expandedInstances[path] = ports;
    g_pendingExpansions.erase(path);
}

void EquipotentialView::applyInstanceInternals(
        const InstancePath& path,
        const InstanceInternals& data) {
    g_instanceInternals[path] = data;
    g_hierPending.erase(path);
}

// ---------------------------------------------------------------------------
// Color helpers
// ---------------------------------------------------------------------------
namespace {
ImColor topTermColor(Direction d) {
    switch (d) {
        case Direction::Input:  return ImColor(255,   0,   0);
        case Direction::Output: return ImColor(  0, 255,   0);
        default:                return ImColor(255, 255,   0);
    }
}
ImColor occTermColor(Direction d) {
    switch (d) {
        case Direction::Input:  return ImColor(  0, 255,   0);
        case Direction::Output: return ImColor(255,   0,   0);
        default:                return ImColor(255, 255,   0);
    }
}
} // namespace

// ---------------------------------------------------------------------------
// renderSchematic
// ---------------------------------------------------------------------------
void EquipotentialView::renderSchematic(const std::vector<Equipotential*>& equipotentials) {
    const float scrollSz = ImGui::GetFrameHeight();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImVec2 csz   = ImVec2(std::max(10.f, avail.x - scrollSz),
                          std::max(10.f, avail.y - scrollSz));
    ImVec2 cur0  = ImGui::GetCursorPos();

    ImGui::BeginChild("##SC", csz, false,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImVec2 inner = ImGui::GetContentRegionAvail();
    ImGui::InvisibleButton("cv", inner,
        ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
        ImGuiButtonFlags_MouseButtonMiddle);
    ImDrawList* dl   = ImGui::GetWindowDrawList();
    ImVec2      cpos = ImGui::GetItemRectMin();
    g_canvasOrigin   = cpos;

    g_schematic.handleInteraction(cpos, inner);

    if (g_pendingZoomSteps) {
        float f = g_pendingZoomSteps > 0 ? 1.1f : 0.9f;
        for (int i = 0; i < std::abs(g_pendingZoomSteps); ++i) g_schematic.zoomBy(f);
        g_pendingZoomSteps = 0;
    }
    if (g_pendingFit) { g_schematic.requestFit(true); g_pendingFit = false; }

    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        // Right-clicking a known instance box opens a per-instance menu
        // instead of the canvas-level one below. Reverse scan: a nested
        // child's rect sits inside its parent's, so the innermost box under
        // the cursor is whichever was appended last.
        ImVec2 wp = mouseWorldPos(g_schematic, cpos);
        g_ctxInstanceId = -1;
        g_ctxPortId     = -1;
        if (auto hit = SchematicInteraction::pickPin(g_schematic.instances, wp, g_schematic.transform.scale))
            if (g_portEquiByPortId.count(hit->portId)) g_ctxPortId = hit->portId;
        for (auto rit = g_schematic.instances.rbegin(); rit != g_schematic.instances.rend(); ++rit) {
            if (rit->w <= 0.f || rit->h <= 0.f) continue;
            if (wp.x < rit->x || wp.x > rit->x + rit->w) continue;
            if (wp.y < rit->y || wp.y > rit->y + rit->h) continue;
            if (g_occInfoByShapeId.count(rit->id)) g_ctxInstanceId = rit->id;
            break;
        }
        if (g_ctxPortId >= 0) g_ctxInstanceId = -1;
        ImGui::OpenPopup("##ctx");
    }
    auto canvasMenuItems = [] {
        if (ImGui::MenuItem("Clear all nets")) g_pendingClear = true;
        if (ImGui::MenuItem("Fit view"))       g_schematic.requestFit(true);
        ImGui::Separator();
        if (ImGui::MenuItem("Show Hierarchy", nullptr, g_showHierarchy)) setShowHierarchy(!g_showHierarchy);
    };
    if (ImGui::BeginPopup("##ctx")) {
        auto occIt = g_ctxInstanceId >= 0 ? g_occInfoByShapeId.find(g_ctxInstanceId)
                                          : g_occInfoByShapeId.end();
        auto portIt = g_ctxPortId >= 0 ? g_portEquiByPortId.find(g_ctxPortId)
                                       : g_portEquiByPortId.end();
        if (portIt != g_portEquiByPortId.end()) {
            if (ImGui::MenuItem("Trace to Driver") && g_provider) {
                // Adds to the view (like a pin double-click) instead of
                // clearing it, so the cone extends what's already shown.
                json req;
                req["request"] = "trace_driver";
                req["path"]    = portIt->second.pathIds;
                req["term_id"] = portIt->second.termId;
                if (portIt->second.bit.has_value()) req["bit"] = portIt->second.bit.value();
                g_provider->send(req.dump());
            }
            auto busIt = g_expandedBusGroupByPortId.find(g_ctxPortId);
            if (busIt != g_expandedBusGroupByPortId.end() && ImGui::MenuItem("Collapse Bus"))
                g_expandedBuses.erase(busIt->second);
        } else if (occIt != g_occInfoByShapeId.end()) {
            if (occIt->second.sourceLoc.has_value()) {
                if (ImGui::MenuItem("Show RTL Source") && g_provider) {
                    const auto& loc = *occIt->second.sourceLoc;
                    json req;
                    req["request"] = "load_source";
                    req["file"]    = loc.file;
                    req["line"]    = loc.line;
                    g_provider->send(req.dump());
                }
            }
            if (ImGui::MenuItem("Show Properties") && g_provider) {
                json req;
                req["request"] = "get_properties";
                req["kind"]    = "instance";
                writePath(req, occIt->second.path);
                g_provider->send(req.dump());
            }
            // A module frame covers a lot of canvas, so right-clicking its
            // empty area also offers the canvas-level entries.
            auto* shape = g_schematic.findInstanceById(g_ctxInstanceId);
            if (shape && shape->isHierGroup) {
                if (ImGui::MenuItem("Zoom to Module")) g_pendingZoomGroup = occIt->second.path;
                ImGui::Separator();
                canvasMenuItems();
            }
        } else {
            canvasMenuItems();
        }
        ImGui::EndPopup();
    }

    // Single click: an instance's hierarchy glyph expands/collapses its
    // sub-instances nested inside its box; otherwise a pin runs its click
    // action (see clickPin). The glyph is checked first since it's a small,
    // distinct hotspot (top-center of the box) that never overlaps a pin.
    // Only the first click of a double-click counts, so a double-click
    // doesn't act twice (e.g. expand a bus, then load the bit that lands
    // under the cursor).
    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        ImGui::GetMouseClickedCount(ImGuiMouseButton_Left) == 1) {
        ImVec2 wp = mouseWorldPos(g_schematic, cpos);
        bool glyphHit = false;
        for (const auto& inst : g_schematic.instances) {
            if (!canShowHierToggle(inst)) continue;
            float gx0, gy0, gx1, gy1;
            hierToggleGlyphRect(inst, gx0, gy0, gx1, gy1);
            if (wp.x < gx0 || wp.x > gx1 || wp.y < gy0 || wp.y > gy1) continue;
            glyphHit = true;
            if (!g_hierExpanded.erase(inst.path)) {
                g_hierExpanded.insert(inst.path);
                if (!g_instanceInternals.count(inst.path) && !g_hierPending.count(inst.path) && g_provider) {
                    auto occIt = g_occInfoByShapeId.find(inst.id);
                    if (occIt != g_occInfoByShapeId.end()) {
                        g_hierPending.insert(inst.path);
                        json req;
                        req["request"]                  = "load_instance_internals";
                        writePath(req, inst.path, "instance_path", "instance_id_path");
                        req["design_ref"]["db_id"]      = occIt->second.designRef.db_id;
                        req["design_ref"]["library_id"] = occIt->second.designRef.library_id;
                        req["design_ref"]["design_id"]  = occIt->second.designRef.design_id;
                        g_provider->send(req.dump());
                    }
                }
            }
            break;
        }
        if (!glyphHit && !clickPin(wp)) {
            // On a box body: select that instance (or module frame). Reverse
            // scan so the innermost box under the cursor wins.
            for (auto rit = g_schematic.instances.rbegin(); rit != g_schematic.instances.rend(); ++rit) {
                if (rit->w <= 0.f || rit->h <= 0.f) continue;
                if (wp.x < rit->x || wp.x > rit->x + rit->w) continue;
                if (wp.y < rit->y || wp.y > rit->y + rit->h) continue;
                auto occIt = g_occInfoByShapeId.find(rit->id);
                if (occIt != g_occInfoByShapeId.end())
                    SelectionStore::select(occIt->second.path, SelectionStore::Origin::Schematic);
                break;
            }
        }
    }

    // Double-click on a box body (not a pin -- those act on single click
    // above): a partial-interface box loads its full interface, revealing
    // the pins whose nets aren't shown yet.
    if (ImGui::IsItemHovered() &&
        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && g_provider) {
        ImVec2 wp = mouseWorldPos(g_schematic, cpos);
        const float wx = wp.x, wy = wp.y;
        const bool hit = SchematicInteraction::pickPin(g_schematic.instances, wp,
                                                       g_schematic.transform.scale).has_value();
        if (!hit) {
            // A hierarchy-expanded box's nested children sit *inside* its
            // own bounding rect, so a forward scan would always match the
            // outer (parent) box first. Scan in reverse instead -- children
            // are always appended after their parent -- so the innermost box
            // actually under the cursor is the one whose partialInterface
            // gets checked.
            const InstanceShape* target = nullptr;
            for (auto rit = g_schematic.instances.rbegin(); rit != g_schematic.instances.rend(); ++rit) {
                if (rit->w <= 0.f || rit->h <= 0.f) continue;
                if (wx < rit->x || wx > rit->x + rit->w) continue;
                if (wy < rit->y || wy > rit->y + rit->h) continue;
                target = &(*rit);
                break;
            }
            if (target && target->partialInterface) {
                auto it = g_occInfoByShapeId.find(target->id);
                if (it != g_occInfoByShapeId.end())
                    requestExpansion(it->second.path, it->second.designRef);
            }
        }
    }

    // -----------------------------------------------------------------------
    // Rebuild geometry: merged instances + per-equip wires
    // -----------------------------------------------------------------------
    g_schematic.instances.clear();
    g_schematic.nets.clear();
    g_occInfoByShapeId.clear();
    g_portEquiByPortId.clear();
    g_busGroupByPortId.clear();
    g_expandedBusGroupByPortId.clear();

    int nextInstId = 1, nextPortId = 1, totalItems = 0;

    // Pass 1: collect merged instance data across all equips
    struct PortSlot {
        int                   portId;
        std::string           name;
        Direction             direction;
        unsigned              termChildId;
        std::optional<int>    termBit;
        bool                  clock = false;
        std::vector<unsigned> pathIds;
    };
    struct MInst {
        bool                  initialized = false;
        DesignRef             designRef{};
        PrimitiveType         primitiveType = PrimitiveType::Unknown;
        bool                  hasInstances = false;
        std::optional<size_t> bitTermCount;
        std::optional<SourceLoc> sourceLoc;
        InstancePath             path;
        std::vector<std::string> pathModels;
        std::vector<unsigned>    pathIds;   // pathIds(path), for pin requests
        std::vector<PortSlot> ports;
    };
    std::map<InstancePath, MInst> minsts;

    // Per-equip wire endpoints: an instance pin (by instance path), or a
    // top-level term (by name, its stub shape id filled in by pass 3).
    struct WireEnd { bool isTerm; InstancePath path; std::string term; int portId; int termInstId = -1;
                     bool drives = false; };
    std::vector<std::vector<WireEnd>> equiEnds(equipotentials.size());
    // Best-effort net-name label per equipotential, shown next to a merged
    // bus wire's slash mark in pass 4 below -- the driving pin's name is the
    // closest thing this view has to a net name (nets aren't otherwise
    // identified in the equipotential wire format).
    std::vector<std::string> netLabelByEi(equipotentials.size());

    for (size_t ei = 0; ei < equipotentials.size(); ++ei) {
        Equipotential* eq = equipotentials[ei];
        if (!eq || (eq->terms.empty() && eq->occurrences.empty())) continue;

        std::vector<Item> drivers, receivers;
        buildItems(eq, drivers, receivers);
        totalItems += int(drivers.size() + receivers.size());
        if (!drivers.empty()) netLabelByEi[ei] = drivers[0].label;

        for (int pass = 0; pass < 2; ++pass) {
            const auto& items = pass == 0 ? drivers : receivers;
            for (const auto& item : items) {
                if (item.isTerm) {
                    int pid = nextPortId++;
                    equiEnds[ei].push_back({ true, {}, item.label, pid, -1, pass == 0 });
                    g_portEquiByPortId[pid] = { item.pathIds, item.termChildId, item.termBit };
                    continue;
                }
                auto& mi = minsts[item.path];
                if (!mi.initialized) {
                    mi.initialized   = true;
                    mi.designRef     = item.designRef;
                    mi.primitiveType = item.primitiveType;
                    mi.hasInstances  = item.hasInstances;
                    mi.bitTermCount  = item.bitTermCount;
                    mi.sourceLoc     = item.sourceLoc;
                    mi.path          = item.path;
                    mi.pathModels    = item.pathModels;
                    mi.pathIds       = item.pathIds;
                }
                // Find or create port slot for this port
                int pid = -1;
                for (const auto& ps : mi.ports) {
                    if (ps.name == item.label && ps.direction == item.direction)
                        { pid = ps.portId; break; }
                }
                if (pid < 0) {
                    pid = nextPortId++;
                    PortSlot ps;
                    ps.portId      = pid;
                    ps.name        = item.label;
                    ps.direction   = item.direction;
                    ps.termChildId = item.termChildId;
                    ps.termBit     = item.termBit;
                    ps.clock       = item.clock;
                    ps.pathIds     = item.pathIds;
                    mi.ports.push_back(std::move(ps));
                    g_portEquiByPortId[pid] = { item.pathIds, item.termChildId, item.termBit };
                    // This pin's net is in the view now: its request is done.
                    g_pendingPinNets.resolve(pinRequestKey(g_portEquiByPortId[pid]));
                }
                equiEnds[ei].push_back({ false, item.path, "", pid, -1, pass == 0 });
            }
        }
    }

    // Instances shown on their own (showInstance): a box even with no net
    // through it yet. Their pins come from g_expandedInstances, all open
    // until a net reaches them.
    for (const auto& [path, start] : g_startInstances) {
        ++totalItems;
        auto& mi = minsts[path];
        if (mi.initialized) continue;
        mi.initialized  = true;
        mi.designRef    = start.designRef;
        mi.primitiveType = start.primitiveType;
        mi.hasInstances = start.hasInstances;
        mi.bitTermCount = start.ports.size();
        mi.sourceLoc    = start.sourceLoc;
        mi.path         = start.path;
        mi.pathModels   = start.pathModels;
        mi.pathIds      = start.pathIds;
    }

    // Pass 2: build InstanceShapes from merged data
    std::map<InstancePath, int> pathToInstId;
    // Per-bit port id (as assigned in pass 1, or freshly allocated below for
    // an expanded-instance port not backed by a loaded equipotential) ->
    // the port id actually drawn, i.e. itself, or the id of the merged bus
    // pin it collapsed into. Used by pass 4 to redirect wire endpoints.
    std::map<int, int> logicalToRenderedPortId;

    for (auto& [key, mi] : minsts) {
        auto expIt = g_expandedInstances.find(key);
        bool isExp = expIt != g_expandedInstances.end() && !expIt->second.empty();

        InstanceShape inst;
        inst.id    = nextInstId++;
        inst.path      = key;
        inst.name      = displayPath(key);
        inst.primitiveType = mi.primitiveType;
        inst.diagOutline = DiagnosisStore::instanceColor(key);
        inst.selected    = SelectionStore::isSelected(key);
        g_occInfoByShapeId[inst.id] = { key, mi.designRef, mi.sourceLoc };
        pathToInstId[key] = inst.id;

        // Hierarchy embedding: this box's model has sub-instances, so it can
        // be expanded in place. If it's toggled open, either grow it now to
        // reserve room for its already-loaded internals (added after layout
        // finalizes positions, below) or kick off the request for them.
        inst.hasChildren  = mi.hasInstances;
        inst.hierExpanded = inst.hasChildren && g_hierExpanded.count(key) > 0;
        float internalsH = 0.f;
        if (inst.hierExpanded) {
            auto internalsIt = g_instanceInternals.find(key);
            if (internalsIt != g_instanceInternals.end()) {
                size_t n = internalsIt->second.children.size();
                float blockH = n ? float(n) * kHierChildH + float(n - 1) * kHierGap : 0.f;
                internalsH = blockH + kHierMargin;
            } else if (!g_hierPending.count(key) && g_provider) {
                g_hierPending.insert(key);
                json req;
                req["request"]                  = "load_instance_internals";
                writePath(req, key, "instance_path", "instance_id_path");
                req["design_ref"]["db_id"]      = mi.designRef.db_id;
                req["design_ref"]["library_id"] = mi.designRef.library_id;
                req["design_ref"]["design_id"]  = mi.designRef.design_id;
                g_provider->send(req.dump());
            }
        }

        if (isExp) {
            inst.partialInterface = false;

            // Resolve each ExpandedPort to the port id it should be drawn/
            // wired under (reusing a pass-1 id when this bit backs a loaded
            // equipotential, else allocating a display-only id), then group
            // by (direction, bus base name) exactly like the partial-
            // interface branch below so a bus collapses to one pin here too.
            // `open`: no loaded equipotential backs this pin, i.e. its net
            // isn't in the view yet -- clicking it will add it.
            struct ResolvedPort { const EquipotentialView::ExpandedPort* ep; int pid; bool open; };
            std::vector<ResolvedPort> resolved;
            resolved.reserve(expIt->second.size());
            for (const auto& ep : expIt->second) {
                int pid = nextPortId++;
                bool open = true;
                for (const auto& ps : mi.ports) {
                    if (ps.name == ep.name && ps.direction == ep.direction)
                        { pid = ps.portId; nextPortId--; open = false; break; }
                }
                g_portEquiByPortId[pid] = { mi.pathIds, ep.childId, ep.bit };
                resolved.push_back({ &ep, pid, open });
            }

            struct Row { std::vector<const ResolvedPort*> members; bool merged = false;
                         std::string busBase; bool isExpandedBusBit = false; BusGroupKey groupKey; };
            std::vector<Row> rows;
            std::set<BusGroupKey> seenGroup;
            for (const auto& rp : resolved) {
                std::string base = stripBusIndex(rp.ep->name);
                bool isBusBit = base != rp.ep->name;
                if (!isBusBit) { rows.push_back({ {&rp}, false, "", false, {} }); continue; }
                bool isIn = rp.ep->direction == Direction::Input;
                BusGroupKey gk{key, base, isIn};
                if (seenGroup.count(gk)) continue;
                seenGroup.insert(gk);
                std::vector<const ResolvedPort*> members;
                for (const auto& rp2 : resolved)
                    if (rp2.ep->direction == rp.ep->direction && stripBusIndex(rp2.ep->name) == base)
                        members.push_back(&rp2);
                bool expandedBus = members.size() == 1 || g_expandedBuses.count(gk);
                if (expandedBus) {
                    for (size_t i = 0; i < members.size(); ++i)
                        rows.push_back({ { members[i] }, false, "", members.size() > 1, gk });
                } else {
                    rows.push_back({ members, true, base, false, gk });
                }
            }

            for (const auto& row : rows) {
                bool isIn = row.members[0]->ep->direction == Direction::Input;
                Port p;
                if (row.merged) {
                    p.id = nextPortId++;
                    std::vector<int> bits;
                    for (auto* m : row.members) if (m->ep->bit.has_value()) bits.push_back(*m->ep->bit);
                    p.name  = busLabel(row.busBase, bits);
                    p.isBus = true;
                    for (auto* m : row.members) logicalToRenderedPortId[m->pid] = p.id;
                    g_busGroupByPortId[p.id] = row.groupKey;
                    p.color = DiagnosisStore::netColor(key, row.busBase);
                    for (auto* m : row.members) p.open = p.open || m->open;
                } else {
                    p.id   = row.members[0]->pid;
                    p.name = row.members[0]->ep->name;
                    p.clock = row.members[0]->ep->clock;
                    p.color = DiagnosisStore::netColor(key, stripBusIndex(row.members[0]->ep->name));
                    if (row.isExpandedBusBit) g_expandedBusGroupByPortId[p.id] = row.groupKey;
                    p.open    = row.members[0]->open;
                    p.pending = p.open && g_pendingPinNets.isPending(
                                    pinRequestKey(g_portEquiByPortId[p.id]), ImGui::GetTime());
                }
                p.lx = isIn ? -0.5f : 0.5f;
                p.direction = row.members[0]->ep->direction;
                p.isInput   = !isIn;
                inst.ports.push_back(p);
            }
        } else {
            // Only a subset of the interface is shown -- unless the loaded
            // equipotentials already happen to touch every bit term of the
            // model, in which case there's nothing left to expand. Unknown
            // count (older server) keeps the conservative "partial" look.
            inst.partialInterface = !mi.bitTermCount.has_value()
                                 || mi.ports.size() < *mi.bitTermCount;
            // A gate symbol has no dashed border and no pin names, so one
            // missing pins (e.g. an AND reached through its output only)
            // looks complete but wrong. Load its whole interface straight
            // away instead of waiting for a double-click.
            // Only once: an empty reply leaves it partial (isExp is false).
            if (inst.partialInterface && isGateSymbol(mi.primitiveType) &&
                !g_expandedInstances.count(key))
                requestExpansion(key, mi.designRef);

            // Group the per-bit PortSlots accumulated in pass 1 by (direction,
            // bus base name) so a bus with >=2 loaded bits collapses to one
            // pin instead of one row per bit.
            struct Row { std::vector<const PortSlot*> members; bool merged = false;
                         std::string busBase; bool isExpandedBusBit = false; BusGroupKey groupKey; };
            std::vector<Row> rows;
            std::set<BusGroupKey> seenGroup;
            for (const auto& ps : mi.ports) {
                std::string base = stripBusIndex(ps.name);
                bool isBusBit = base != ps.name;
                if (!isBusBit) { rows.push_back({ {&ps}, false, "", false, {} }); continue; }
                bool isIn = ps.direction == Direction::Input;
                BusGroupKey gk{key, base, isIn};
                if (seenGroup.count(gk)) continue;
                seenGroup.insert(gk);
                std::vector<const PortSlot*> members;
                for (const auto& ps2 : mi.ports)
                    if (ps2.direction == ps.direction && stripBusIndex(ps2.name) == base)
                        members.push_back(&ps2);
                bool expandedBus = members.size() == 1 || g_expandedBuses.count(gk);
                if (expandedBus) {
                    for (size_t i = 0; i < members.size(); ++i)
                        rows.push_back({ { members[i] }, false, "", members.size() > 1, gk });
                } else {
                    rows.push_back({ members, true, base, false, gk });
                }
            }

            for (const auto& row : rows) {
                bool isIn = row.members[0]->direction == Direction::Input;
                Port p;
                if (row.merged) {
                    p.id = nextPortId++;
                    std::vector<int> bits;
                    for (auto* m : row.members) if (m->termBit.has_value()) bits.push_back(*m->termBit);
                    p.name  = busLabel(row.busBase, bits);
                    p.isBus = true;
                    for (auto* m : row.members) logicalToRenderedPortId[m->portId] = p.id;
                    g_busGroupByPortId[p.id] = row.groupKey;
                    p.color = DiagnosisStore::netColor(key, row.busBase);
                } else {
                    p.id   = row.members[0]->portId;
                    p.name = row.members[0]->name;
                    p.clock = row.members[0]->clock;
                    p.color = DiagnosisStore::netColor(key, stripBusIndex(row.members[0]->name));
                    if (row.isExpandedBusBit) g_expandedBusGroupByPortId[p.id] = row.groupKey;
                }
                p.lx = isIn ? -0.5f : 0.5f;
                p.direction = row.members[0]->direction;
                p.isInput   = !isIn;
                inst.ports.push_back(p);
            }
        }
        applySymbolGeometry(inst, internalsH);
        if (internalsH > 0.f) inst.w = std::max(inst.w, kHierChildW + 2.0f * kHierMargin);
        g_schematic.instances.push_back(std::move(inst));
    }

    auto renderedPort = [&](int pid) {
        auto it = logicalToRenderedPortId.find(pid);
        return it != logicalToRenderedPortId.end() ? it->second : pid;
    };
    // World position of an instance end's (rendered) pin, if it's drawn.
    auto pinWorld = [&](const WireEnd& we) -> std::optional<ImVec2> {
        auto kit = pathToInstId.find(we.path);
        InstanceShape* s = kit != pathToInstId.end() ? g_schematic.findInstanceById(kit->second) : nullptr;
        const Port* p = s ? g_schematic.findPortById(*s, renderedPort(we.portId)) : nullptr;
        if (!p) return std::nullopt;
        return portAnchor(*s, *p);
    };

    // Layered placement of the instance boxes (see SchematicLayout): every
    // net between two instances is an edge from its driver pin to each
    // receiver pin.
    std::map<int, int> nodeOfShape;
    std::vector<PlaceNode> placeNodes;
    for (const auto& inst : g_schematic.instances) {
        nodeOfShape[inst.id] = int(placeNodes.size());
        placeNodes.push_back({ inst.w, inst.h });
    }
    std::vector<PlaceNet> placeNets;
    for (const auto& ends : equiEnds) {
        PlaceNet pn;
        for (const auto& we : ends) {
            if (we.isTerm) continue;
            auto kit = pathToInstId.find(we.path);
            if (kit == pathToInstId.end()) continue;
            InstanceShape* s = g_schematic.findInstanceById(kit->second);
            const Port* p = s ? g_schematic.findPortById(*s, renderedPort(we.portId)) : nullptr;
            if (!p) continue;
            PlacePin pin{ nodeOfShape[s->id], (p->ly + 0.5f) * s->h };
            (we.drives ? pn.drivers : pn.receivers).push_back(pin);
        }
        if (!pn.drivers.empty() && !pn.receivers.empty()) placeNets.push_back(std::move(pn));
    }
    const Placement placement = layeredPlacement(placeNodes, placeNets);
    for (auto& inst : g_schematic.instances) {
        const ImVec2 pos = placement.pos[nodeOfShape[inst.id]];
        inst.x = pos.x;
        inst.y = pos.y;
    }

    // With Show Hierarchy, wrap the leaves in module frames (which re-lays
    // them out, keeping each one's logic level).
    bool grouped = false;
    if (g_showHierarchy) {
        std::map<InstancePath, LeafHier> leafHier;
        for (const auto& [path, mi] : minsts)
            leafHier[path] = { mi.path, mi.pathModels, placement.level[nodeOfShape[pathToInstId[path]]] };
        auto frames = layoutHierarchyGroups(leafHier, g_schematic.instances, pathToInstId, nextInstId);
        grouped = !frames.empty();
        std::vector<InstanceShape> frameShapes;
        for (auto& f : frames) {
            f.shape.diagOutline = DiagnosisStore::instanceColor(f.shape.path);
            f.shape.selected    = SelectionStore::isSelected(f.shape.path);
            g_occInfoByShapeId[f.shape.id] = { f.shape.path, DesignRef{}, std::nullopt };
            frameShapes.push_back(std::move(f.shape));
        }
        // Parent-first order at the front: drawn under everything, and reverse
        // hit-test scans still find a leaf before the frame around it.
        g_schematic.instances.insert(g_schematic.instances.begin(),
                                     std::make_move_iterator(frameShapes.begin()),
                                     std::make_move_iterator(frameShapes.end()));
    }

    // Hierarchy embedding: now that top-level positions are final, lay out
    // and append the nested children/internal-nets of every expanded, loaded
    // instance. Collected as ids first (not pointers) since emission appends
    // to the very vectors we're about to iterate, which would invalidate
    // iterators held across a push_back.
    {
        std::vector<int> hierExpandIds;
        for (const auto& inst : g_schematic.instances)
            if (inst.hierExpanded && g_instanceInternals.count(inst.path))
                hierExpandIds.push_back(inst.id);

        for (int id : hierExpandIds) {
            auto* parent = g_schematic.findInstanceById(id);
            if (!parent) continue;
            auto result = emitInstanceInternals(*parent, nextInstId, nextPortId);
            for (auto& s : result.shapes) g_schematic.instances.push_back(std::move(s));
            for (auto& n : result.nets)   g_schematic.nets.push_back(std::move(n));
        }
    }

    // Pass 3: top-level ports, on the sheet's edges -- inputs
    // left of everything, outputs right of it -- each level with the pin it
    // connects to (a straight wire) unless that would crowd another port.
    float sheetX0 = 1e30f, sheetX1 = -1e30f;
    for (const auto& inst : g_schematic.instances) {
        if (inst.parentShapeId >= 0 || inst.w <= 0.f) continue;
        sheetX0 = std::min(sheetX0, inst.x);
        sheetX1 = std::max(sheetX1, inst.x + inst.w);
    }
    if (sheetX0 > sheetX1) { sheetX0 = kLeftMargin; sheetX1 = kLeftMargin; }
    struct PortStub { size_t ei; std::string label; Direction direction; int portId; float wantY; };
    std::vector<PortStub> leftStubs, rightStubs;
    for (size_t ei = 0; ei < equipotentials.size(); ++ei) {
        Equipotential* eq = equipotentials[ei];
        if (!eq) continue;

        std::vector<Item> drivers, receivers;
        buildItems(eq, drivers, receivers);
        for (int pass = 0; pass < 2; ++pass) {
            const auto& items = pass == 0 ? drivers : receivers;
            for (const auto& item : items) {
                if (!item.isTerm) continue;
                // The first end of this term not claimed by a stub yet (so a
                // term listed twice gets one stub).
                WireEnd* end = nullptr;
                for (auto& we : equiEnds[ei])
                    if (we.isTerm && we.termInstId < 0 && we.term == item.label) { end = &we; break; }
                if (!end) continue;
                end->termInstId = 0;  // claimed; the real stub id is set below
                // Level with the first instance pin across the net: a
                // receiver for an input port, the driver for an output.
                float wantY = 0.f;
                const bool isInput = item.direction == Direction::Input;
                for (int prefer = 0; prefer < 2; ++prefer) {
                    bool found = false;
                    for (const auto& we : equiEnds[ei]) {
                        if (we.isTerm || (prefer == 0 && we.drives == isInput)) continue;
                        if (auto at = pinWorld(we)) { wantY = at->y; found = true; break; }
                    }
                    if (found) break;
                }
                (isInput ? leftStubs : rightStubs).push_back({ ei, item.label, item.direction, end->portId, wantY });
            }
        }
    }
    for (auto* stubs : { &leftStubs, &rightStubs }) {
        std::vector<float> want;
        for (const auto& st : *stubs) want.push_back(st.wantY);
        const std::vector<float> ys = stackPorts(want);
        const bool isInput = stubs == &leftStubs;
        for (size_t i = 0; i < stubs->size(); ++i) {
            const PortStub& st = (*stubs)[i];
            InstanceShape inst;
            inst.id = nextInstId++;
            inst.x  = isInput ? sheetX0 - kPortGap : sheetX1 + kPortGap;
            inst.y  = ys[i];
            inst.w  = 0.f; inst.h = 0.f;
            inst.partialInterface = false;
            // Draws as a boundary-port flag (see
            // drawBoundaryPortInstance in SchematicView.cpp) rather than
            // a generic box -- this pseudo-instance only exists to give
            // the top-level design port a wire anchor point.
            inst.modelName = "port";

            Port p;
            p.id = st.portId; p.name = st.label;
            p.lx = isInput ? 1.0f : -1.0f;
            p.ly = 0.f;
            p.direction = st.direction;
            p.isInput   = isInput;
            p.color     = DiagnosisStore::netColor({}, stripBusIndex(st.label));
            inst.ports.push_back(p);
            for (auto& we : equiEnds[st.ei])
                if (we.isTerm && we.portId == st.portId) we.termInstId = inst.id;
            g_schematic.instances.push_back(std::move(inst));
        }
    }

    // Pass 4: wires per equip
    for (size_t ei = 0; ei < equipotentials.size(); ++ei) {
        const auto& ends = equiEnds[ei];
        if (ends.size() < 2) continue;

        struct WR { int instId; int portId; };
        std::vector<WR> wrs;
        for (const auto& we : ends) {
            int instId = we.termInstId;
            if (!we.isTerm) {
                auto it = pathToInstId.find(we.path);
                if (it != pathToInstId.end()) instId = it->second;
            }
            if (instId < 0) continue;
            wrs.push_back({ instId, renderedPort(we.portId) });
        }
        if (wrs.size() < 2) continue;

        for (size_t i = 1; i < wrs.size(); ++i) {
            NetWire n;
            n.id          = int(g_schematic.nets.size()) + 1;
            n.srcInstance = wrs[0].instId;
            n.srcPortId   = wrs[0].portId;
            n.dstInstance = wrs[i].instId;
            n.dstPortId   = wrs[i].portId;
            n.netName     = netLabelByEi[ei];

            // A flagged endpoint pin colors the whole wire so a diagnosed
            // net stands out even when the flagged pin is off-screen.
            auto* srcInst = g_schematic.findInstanceById(n.srcInstance);
            auto* srcPort = srcInst ? g_schematic.findPortById(*srcInst, n.srcPortId) : nullptr;
            auto* dstInst = g_schematic.findInstanceById(n.dstInstance);
            auto* dstPort = dstInst ? g_schematic.findPortById(*dstInst, n.dstPortId) : nullptr;
            if (srcPort && srcPort->color != 0)      n.color = srcPort->color;
            else if (dstPort && dstPort->color != 0) n.color = dstPort->color;

            g_schematic.nets.push_back(n);
        }
    }

    // Several bits of the same collapsed bus between the same two (merged)
    // pins now produce identical (src,srcPort,dst,dstPort) wires — collapse
    // those into one thicker "bus" wire instead of drawing them stacked.
    {
        std::map<std::tuple<int,int,int,int>, size_t> firstOf;
        auto portIsBus = [](int instId, int portId) {
            auto* inst = g_schematic.findInstanceById(instId);
            auto* port = inst ? g_schematic.findPortById(*inst, portId) : nullptr;
            return port && port->isBus;
        };
        std::vector<NetWire> merged;
        for (const auto& n : g_schematic.nets) {
            auto k = std::make_tuple(n.srcInstance, n.srcPortId, n.dstInstance, n.dstPortId);
            auto it = firstOf.find(k);
            if (it == firstOf.end()) {
                firstOf[k] = merged.size();
                merged.push_back(n);
            } else if (portIsBus(n.srcInstance, n.srcPortId) || portIsBus(n.dstInstance, n.dstPortId)) {
                merged[it->second].isBus = true;
            }
            // else: the same net drawn twice (e.g. overlapping trace nets) --
            // just one wire, not a bus.
        }
        g_schematic.nets = std::move(merged);
    }

    // Route the top-level wires as one orthogonal tree per driving pin (see
    // SchematicLayout::routeNets), around the boxes. Wiring nested inside an
    // expanded instance keeps its simple per-wire drawing.
    {
        std::vector<RouteRect> obstacles, keepOut;
        for (const auto& inst : g_schematic.instances) {
            if (inst.parentShapeId >= 0 || inst.w <= 0.f || inst.h <= 0.f) continue;
            if (inst.isHierGroup) {  // its label row
                keepOut.push_back({ inst.x, inst.y, inst.x + inst.w, inst.y + kGroupHeader - kPinPitch });
                continue;
            }
            obstacles.push_back({ inst.x, inst.y, inst.x + inst.w, inst.y + inst.h });
            keepOut.push_back({ inst.x, inst.y - kPinPitch, inst.x + inst.w, inst.y });  // its name
        }
        std::map<std::pair<int, int>, size_t> routeOfSource;
        std::vector<RouteNet> routeNetsIn;
        g_schematic.routes.clear();
        auto pinOf = [](const InstanceShape& inst, const Port& p) {
            return RoutePin{ portAnchor(inst, p), p.lx >= 0.f };
        };
        for (auto& n : g_schematic.nets) {
            if (n.containerShapeId >= 0) continue;
            auto* srcInst = g_schematic.findInstanceById(n.srcInstance);
            auto* dstInst = g_schematic.findInstanceById(n.dstInstance);
            auto* srcPort = srcInst ? g_schematic.findPortById(*srcInst, n.srcPortId) : nullptr;
            auto* dstPort = dstInst ? g_schematic.findPortById(*dstInst, n.dstPortId) : nullptr;
            if (!srcPort || !dstPort) continue;
            auto key = std::make_pair(n.srcInstance, n.srcPortId);
            auto it  = routeOfSource.find(key);
            if (it == routeOfSource.end()) {
                it = routeOfSource.emplace(key, routeNetsIn.size()).first;
                routeNetsIn.push_back({ pinOf(*srcInst, *srcPort), {} });
                SchematicView::NetRoute r;
                r.color   = n.color;
                r.netName = n.netName;
                g_schematic.routes.push_back(std::move(r));
            }
            routeNetsIn[it->second].receivers.push_back(pinOf(*dstInst, *dstPort));
            auto& r = g_schematic.routes[it->second];
            // A flagged wire colors the whole tree it belongs to.
            if (n.color != NetWire{}.color) r.color = n.color;
            r.isBus = r.isBus || n.isBus;
            n.routed = true;
        }
        auto routed = routeNets(routeNetsIn, obstacles, keepOut);
        for (size_t i = 0; i < routed.size(); ++i) {
            g_schematic.routes[i].segments  = std::move(routed[i].segments);
            g_schematic.routes[i].junctions = std::move(routed[i].junctions);
        }
    }

    static int lastTotal = -1;
    if (totalItems != lastTotal) {
        if (g_skipNextAutoFit) {
            g_skipNextAutoFit = false;
            // Pan so the box whose pin was clicked stays under the cursor,
            // wherever the re-layout moved it (screen = (world - offset) *
            // scale, so shifting offset by the world delta cancels it out).
            for (const auto& inst : g_schematic.instances) {
                if (inst.isHierGroup || inst.path != g_keepInPlacePath) continue;
                g_schematic.transform.offset.x += inst.x - g_keepInPlaceWorld.x;
                g_schematic.transform.offset.y += inst.y - g_keepInPlaceWorld.y;
                break;
            }
            g_keepInPlacePath.clear();
        } else {
            g_schematic.requestFit(true);
        }
        lastTotal = totalItems;
    }
    if (g_pendingZoomGroup) {
        for (const auto& inst : g_schematic.instances) {
            if (!inst.isHierGroup || inst.path != *g_pendingZoomGroup) continue;
            g_schematic.requestFitRect(ImVec2(inst.x, inst.y), ImVec2(inst.x + inst.w, inst.y + inst.h));
            break;
        }
        g_pendingZoomGroup.reset();
    }
    g_schematic.updateFitIfNeeded(cpos, inner, 60.f);

    // Hover feedback, against this frame's geometry (the one about to be
    // drawn): the pin under the cursor is highlighted, gets a hand cursor if
    // clicking it does something, and a tooltip saying what; otherwise the
    // box under the cursor shows its diagnostics and, after a short rest on
    // a partial-interface box, how to reveal its other pins.
    g_schematic.hoveredPortId = -1;
    const bool canvasHovered =
        ImGui::IsMouseHoveringRect(cpos, ImVec2(cpos.x + inner.x, cpos.y + inner.y)) &&
        !ImGui::IsPopupOpen("##ctx") && !ImGui::IsMouseDragging(ImGuiMouseButton_Right) &&
        !ImGui::IsMouseDragging(ImGuiMouseButton_Middle);
    bool pinHovered = false;
    if (canvasHovered) {
        ImVec2 wp = mouseWorldPos(g_schematic, cpos);
        auto hit = SchematicInteraction::pickPin(g_schematic.instances, wp, g_schematic.transform.scale);
        InstanceShape* inst = hit ? g_schematic.findInstanceById(hit->instanceId) : nullptr;
        Port* port = inst ? g_schematic.findPortById(*inst, hit->portId) : nullptr;
        if (port) {
            pinHovered = true;
            g_schematic.hoveredPortId = port->id;
            const PinAction action = pinAction(*port);
            if (action == PinAction::ExpandBus || action == PinAction::LoadNet)
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ImGui::BeginTooltip();
            if (inst->modelName == "port") ImGui::Text("%s  (top-level port)", port->name.c_str());
            else                           ImGui::Text("%s / %s", inst->name.c_str(), port->name.c_str());
            switch (action) {
                case PinAction::ExpandBus: ImGui::TextDisabled("Click: show the individual bits"); break;
                case PinAction::LoadNet:   ImGui::TextDisabled("Click: add this net to the view"); break;
                case PinAction::Loading:   ImGui::TextDisabled("Loading net..."); break;
                case PinAction::None:      ImGui::TextDisabled("Net shown"); break;
            }
            if (g_portEquiByPortId.count(port->id))
                ImGui::TextDisabled("Right-click: Trace to Driver");
            ImGui::EndTooltip();
        }
    }
    if (canvasHovered && !pinHovered) {
        ImVec2 wp = mouseWorldPos(g_schematic, cpos);
        bool overBox = false;
        // Reverse scan: a nested child's rect sits inside its parent's, so
        // the innermost (most specific) box under the cursor is whichever
        // one was appended last -- see the same reasoning on the
        // partialInterface double-click handler above.
        for (auto rit = g_schematic.instances.rbegin(); rit != g_schematic.instances.rend(); ++rit) {
            const auto& inst = *rit;
            if (inst.w <= 0.0f || inst.h <= 0.0f) continue; // skip zero-size term stubs
            if (wp.x < inst.x || wp.x > inst.x + inst.w) continue;
            if (wp.y < inst.y || wp.y > inst.y + inst.h) continue;
            auto it = g_occInfoByShapeId.find(inst.id);
            if (it == g_occInfoByShapeId.end()) break;
            overBox = true;
            if (inst.id != g_hintShapeId) { g_hintShapeId = inst.id; g_hintSince = ImGui::GetTime(); }
            const bool showHint = inst.partialInterface && ImGui::GetTime() - g_hintSince > 0.6;
            auto diagnostics = DiagnosisStore::instanceDiagnostics(it->second.path);
            if (!diagnostics.empty() || showHint) {
                ImGui::BeginTooltip();
                for (const auto* d : diagnostics) {
                    ImGui::TextColored(ImColor(DiagnosisStore::colorForSeverity(d->severity)).Value,
                                       "[%s] %s", toString(d->severity), d->message.c_str());
                    if (!d->source.empty()) ImGui::TextDisabled("source: %s", d->source.c_str());
                }
                if (showHint) ImGui::TextDisabled("Double-click: show all pins");
                ImGui::EndTooltip();
            }
            break;
        }
        if (!overBox) g_hintShapeId = -1;
    } else {
        g_hintShapeId = -1;
    }

    g_schematic.render(dl, cpos, inner);
    if (equipotentials.empty() && g_startInstances.empty()) {
        // Nothing drawn yet: the schematic is only ever filled from a pin's
        // context menu, which a first-time user has no way to guess.
        const char* hint = "Right-click a pin in the Netlist Hierarchy (e.g. top > Terms > a port)\n"
                           "and choose Show Equipotential or Trace to Driver.";
        ImVec2 ts = ImGui::CalcTextSize(hint);
        dl->AddText(ImVec2(cpos.x + (inner.x - ts.x) * 0.5f, cpos.y + (inner.y - ts.y) * 0.5f),
                    IM_COL32(120, 120, 120, 255), hint);
    }
    ImGui::EndChild();

    // Scrollbars
    ImVec2 wMin(-500.f, -500.f), wMax(500.f, 500.f);
    if (g_schematic.computeWorldBounds(wMin, wMax)) {
        wMin.x -= 120.f; wMin.y -= 120.f; wMax.x += 120.f; wMax.y += 120.f;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, 24.f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg,         ImVec4(0.12f, 0.12f, 0.12f, 1));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab,       ImVec4(0.45f, 0.45f, 0.45f, 1));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(0.65f, 0.65f, 0.65f, 1));
    ImGui::SetCursorPos(ImVec2(cur0.x + csz.x, cur0.y));
    ImGui::VSliderFloat("##VS", ImVec2(scrollSz, csz.y),
        &g_schematic.transform.offset.y, wMax.y, wMin.y, "");
    ImGui::SetCursorPos(ImVec2(cur0.x, cur0.y + csz.y));
    ImGui::SetNextItemWidth(csz.x);
    ImGui::SliderFloat("##HS", &g_schematic.transform.offset.x, wMin.x, wMax.x, "");
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();
}

// ---------------------------------------------------------------------------
// renderTable
// ---------------------------------------------------------------------------
void EquipotentialView::renderTable(const std::vector<Equipotential*>& equipotentials) {
    if (!ImGui::BeginTable("eq_table", 3,
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable))
        return;

    ImGui::TableSetupColumn("Element",   ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Hierarchy", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Direction", ImGuiTableColumnFlags_WidthFixed, 70);
    ImGui::TableHeadersRow();

    for (size_t i = 0; i < equipotentials.size(); ++i) {
        const Equipotential* eq = equipotentials[i];
        if (!eq) continue;
        if (i > 0) {
            ImGui::TableNextRow();
            for (int c = 0; c < 3; ++c) { ImGui::TableSetColumnIndex(c); ImGui::Separator(); }
        }
        for (const auto& t : eq->terms) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%s", t.getString().c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextDisabled("(top)");
            ImGui::TableSetColumnIndex(2);
            auto c = topTermColor(t.direction);
            ImGui::PushStyleColor(ImGuiCol_Text,
                ImVec4(c.Value.x, c.Value.y, c.Value.z, c.Value.w));
            ImGui::Text("%s", toString(t.direction));
            ImGui::PopStyleColor();
        }
        for (const auto& occ : eq->occurrences) {
            std::string label = displayPath(occ.path) + "/" + occ.term.getString();
            // The modules enclosing the instance, outermost first, each with
            // its model name when the provider sent one.
            std::string context;
            for (size_t k = 0; k + 1 < occ.path.size(); ++k) {
                if (k) context += " > ";
                context += displayName(occ.path[k]);
                if (k < occ.pathModels.size() && !occ.pathModels[k].empty())
                    context += " (" + occ.pathModels[k] + ")";
            }
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%s", label.c_str());
            ImGui::TableSetColumnIndex(1);
            if (context.empty()) ImGui::TextDisabled("(top)");
            else                 ImGui::Text("%s", context.c_str());
            ImGui::TableSetColumnIndex(2);
            auto c = occTermColor(occ.term.direction);
            ImGui::PushStyleColor(ImGuiCol_Text,
                ImVec4(c.Value.x, c.Value.y, c.Value.z, c.Value.w));
            ImGui::Text("%s", toString(occ.term.direction));
            ImGui::PopStyleColor();
        }
    }
    ImGui::EndTable();
}

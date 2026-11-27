// EquipotentialView.cpp — merged instances, horizontal expansion
#include "EquipotentialView.h"
#include "Console.h"

#include <algorithm>
#include <array>
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
#include "ViewOptions.h"
#include "SchematicInteraction.h"
#include "ReviewStore.h"
#include "ReviewView.h"
#include "SelectionStore.h"
#include "StyleStore.h"
#include "Theme.h"
#include "TraceStore.h"

// Net/instance placement and hierarchy frames live in SchematicLayout
// (pure geometry, unit-tested); this file turns them into drawn shapes.
using namespace SchematicLayout;

// ---------------------------------------------------------------------------
// Layout geometry constants (see SchematicLayout.h for the placement ones)
// ---------------------------------------------------------------------------

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
// The instance whose pin was clicked, the pin, and where the pin was: once
// the net arrives the view is panned so that pin stays put on screen --
// under the cursor that clicked it -- even though adding a net can re-lay
// out everything (hierarchy frames grow and shift). Nothing else moves the
// view: what the net adds off the canvas stays there, to pan to by hand.
static InstancePath       g_keepInPlacePath;
// Or, for a top-level port flag (whose path is the top's, {}, like every
// flag's), its name.
static std::string        g_keepInPlacePort;
static std::string        g_keepInPlacePin;
static ImVec2             g_keepInPlaceWorld{};
static ImVec2             g_keepInPlaceBoxWorld{};  // the box's corner, for a pin gone
// Screen pixels kept clear around what focusObject() brings into view.
static constexpr float    kRevealMargin = 40.0f;
// Layout stability (SchematicLayout's hints): where the last frame's
// placement put each box, and the hints in force. When a pin click's net
// changes what's placed (the boxes, or the nets between them), the layout
// before it becomes the hints, so the boxes already shown keep their columns,
// order and heights; they're kept, unchanged, until the next such change, so
// every frame in between lays out the same. Any other change (expanding a
// box, a tree action, a reload) drops them: a fresh layout, as headless
// exports always get.
struct LayoutHint {
    int   level = 0;
    float y     = 0.f;
};
struct LayoutSignature {
    std::vector<SchematicInteraction::ShapeKey> nodes;
    std::vector<std::pair<int, int>>            edges;  // driver -> receiver, by node
    bool operator==(const LayoutSignature&) const = default;
};
static std::map<SchematicInteraction::ShapeKey, LayoutHint> g_lastLayout;
static std::map<SchematicInteraction::ShapeKey, LayoutHint> g_layoutHints;
static LayoutSignature    g_layoutSignature;
// Pin nets requested by a click and not arrived yet (keyed by
// SchematicInteraction::pinRequestKey), so a second click doesn't re-send.
static SchematicInteraction::PendingRequests g_pendingPinNets;
// Instance box whose "double-click to show all pins" hint is due, and since
// when the cursor has rested on it.
static int                g_hintShapeId      = -1;
static double             g_hintSince        = 0.0;
// Screen position and size of the canvas, last frame.
static ImVec2             g_canvasOrigin{};
static ImVec2             g_canvasSize{};
static INetlistProvider*  g_provider         = nullptr;
// Which instance box (if any) the canvas right-click popup currently
// targets; -1 = the canvas-level menu (Clear all nets / Fit view).
static int                g_ctxInstanceId    = -1;
// Which pin (if any) the popup targets; takes precedence over the instance
// box the pin sits on. -1 = none.
static int                g_ctxPortId        = -1;
// Shape id of the fanout summary right-clicked, or -1.
static int                g_ctxSummaryId     = -1;

// The review badge of an instance box or module frame (ReviewStore).
// --- Opened instances (see openInstance()) ---
// Instances drawn as their contents, by path, and those whose
// load_instance_view is in flight.
static std::map<InstancePath, OpenedInstance> g_opened;
static std::set<InstancePath>                 g_openPending;
// This frame's port flags for opened instances' pins, by the flag's
// made-up path (SchematicLayout::openInstances()).
static std::map<InstancePath, BoundaryPin>    g_boundaryPins;

// The instance a pin belongs to: a box's, the instance an opened
// instance's port flag stands for a pin of, {} for a top-level port.
static InstancePath pinOwnerPath(const InstanceShape& inst) {
    if (inst.modelName != "port") return inst.path;
    auto it = g_boundaryPins.find(inst.path);
    return it != g_boundaryPins.end() ? it->second.owner : InstancePath{};
}

// What a pin is called in a trace's or a comment's label.
static std::string pinLabel(const InstanceShape& inst, const std::string& pin) {
    const InstancePath owner = pinOwnerPath(inst);
    return owner.empty() ? pin : traceLabel(owner, pin);
}

static void setReviewBadge(InstanceShape& shape) {
    const auto threads = ReviewStore::instanceThreads(shape.path);
    shape.reviewOpen  = int(ReviewStore::openCount(threads));
    shape.reviewColor = ReviewStore::openColor(threads);
}

// What a box's context menu needs. `hasSources`: offer "Show Sources" --
// a module frame always does (the provider sent no flag for it).
struct OccurrenceInfo { InstancePath path; DesignRef designRef; bool hasSources = false; };
struct PortEquiRequest {
    std::vector<unsigned> pathIds;
    unsigned              termId = 0;
    std::optional<int>    bit;
};
static std::map<int, OccurrenceInfo>  g_occInfoByShapeId;
static std::map<int, PortEquiRequest> g_portEquiByPortId;
// The term a merged bus pin stands for (no bit: the whole bus), for its
// review threads. Rebuilt every frame.
static std::map<int, PortEquiRequest> g_busTermByPortId;

// The review threads a pin's badge counts (ReviewStore::pinThreads).
static std::vector<const ReviewThread*> pinReviewThreads(int portId) {
    auto bus = g_busTermByPortId.find(portId);
    if (bus != g_busTermByPortId.end())
        return ReviewStore::pinThreads(bus->second.pathIds, bus->second.termId, std::nullopt);
    auto it = g_portEquiByPortId.find(portId);
    if (it == g_portEquiByPortId.end()) return {};
    return ReviewStore::pinThreads(it->second.pathIds, it->second.termId, it->second.bit);
}

// Host styling (StyleStore) on a box or module frame: its fill, a label
// drawn for its name, and its outline -- in the style's color unless a
// diagnosis already outlines it (which keeps its color). Applied before
// the trace convergence outline, which then only goes on unstyled boxes.
static void applyInstanceStyle(InstanceShape& shape) {
    const StyleAttrs style = StyleStore::instanceStyle(shape.path);
    if (style.empty()) return;
    shape.fill = style.fill;
    if (!style.label.empty()) shape.label = style.label;
    if (shape.diagOutline == 0) shape.diagOutline = style.color;
    if (shape.diagOutline != 0) shape.outlineWidth = style.width;
}

// "A[3]" -> 3; nullopt for a scalar name or a merged bus label ("A[7:0]").
static std::optional<int> bitOfLabel(const std::string& label) {
    auto open = label.rfind('[');
    if (open == std::string::npos || label.back() != ']' || open + 2 >= label.size()) return std::nullopt;
    const std::string digits = label.substr(open + 1, label.size() - open - 2);
    if (digits.find_first_not_of("0123456789") != std::string::npos) return std::nullopt;
    return std::stoi(digits);
}

// Host styling (StyleStore::termStyle) on one pin of `inst`, a box or a
// top-level port's flag ({}): the pin's term by id where the view knows it
// (a merged bus pin: the whole bus, no bit), else by its name.
static void applyPinStyle(const InstanceShape& inst, Port& p) {
    std::optional<unsigned> termId;
    std::optional<int> bit;
    if (auto bus = g_busTermByPortId.find(p.id); bus != g_busTermByPortId.end()) {
        termId = bus->second.termId;
    } else if (auto it = g_portEquiByPortId.find(p.id); it != g_portEquiByPortId.end()) {
        termId = it->second.termId;
        bit    = it->second.bit;
    } else {
        bit = bitOfLabel(p.name);
    }
    const InstancePath path = pinOwnerPath(inst);
    const StyleAttrs style = StyleStore::termStyle(path, p.name, termId, bit);
    p.styleColor = style.color;
    p.styleWidth = style.width;
    p.label      = style.label;
}

// Pins the host found unconnected (markUnconnected()).
static std::set<SchematicInteraction::PinRequestKey> g_unconnectedPins;

// focusObject(): the object to bring into view, and whether what adds it
// was requested (since when: given up after a few seconds).
struct PendingFocus {
    EquipotentialView::FocusTarget target;
    bool   requested = false;
    double since = 0.0;
};
static std::optional<PendingFocus> g_focus;
// The pin focusObject() last brought into view, highlighted until then.
static std::optional<EquipotentialView::FocusTarget> g_focusPin;
static double g_focusPinUntil = 0.0;
// Instances whose instance_resolved should add them, not replace the view.
static std::set<InstancePath> g_addOnResolve;
static constexpr double kFocusTimeout = 5.0;
static constexpr double kFocusPinHighlight = 2.5;

// The drawn pin a focus target names: its own bit's pin, else (a bus shown
// merged, or a target without a bit) the term's merged bus pin or any of
// its bit pins.
static std::optional<std::pair<InstanceShape*, Port*>> findFocusPin(
        const EquipotentialView::FocusTarget& t) {
    const auto ids = pathIds(t.path);
    std::optional<std::pair<InstanceShape*, Port*>> loose;
    for (auto& inst : g_schematic.instances) {
        for (auto& p : inst.ports) {
            auto it = g_portEquiByPortId.find(p.id);
            if (it != g_portEquiByPortId.end() && it->second.pathIds == ids &&
                it->second.termId == *t.termId) {
                if (!t.bit || it->second.bit == t.bit) return std::make_pair(&inst, &p);
                continue;
            }
            auto bus = g_busTermByPortId.find(p.id);
            if (bus != g_busTermByPortId.end() && bus->second.pathIds == ids &&
                bus->second.termId == *t.termId && !loose)
                loose = std::make_pair(&inst, &p);
        }
    }
    return loose;
}

// The drawn box (or module frame) of an instance.
static const InstanceShape* findFocusShape(const InstancePath& path) {
    const InstanceShape* frame = nullptr;
    for (const auto& inst : g_schematic.instances) {
        if (inst.path != path || inst.modelName == "port") continue;
        if (!inst.isHierGroup) return &inst;
        frame = &inst;
    }
    return frame;
}

void EquipotentialView::focusObject(const FocusTarget& target) {
    // The top design has no box: only its ports can be shown.
    if (target.path.empty() && !target.termId) { g_focus.reset(); return; }
    g_focus = PendingFocus{target};
}

void EquipotentialView::markUnconnected(const std::vector<unsigned>& pathIds, unsigned termId,
                                        std::optional<int> bit) {
    const auto key = SchematicInteraction::pinRequestKey(pathIds, termId, bit);
    g_unconnectedPins.insert(key);
    g_pendingPinNets.resolve(key);  // answered: no more "loading"
}

bool EquipotentialView::noteUnconnectedReply(const json& reply, const Equipotential& eq) {
    if (!reply.value("found", false) || !reply.contains("start") || !reply["start"].is_object())
        return false;
    const json& s = reply["start"];
    // Ids, signed or not as JSON numbers go (the request's own encoding).
    auto isId = [](const json& v) { return v.is_number_integer() && v.get<long long>() >= 0; };
    std::vector<unsigned> pathIds;
    for (const auto& id : s.value("path", json::array())) {
        if (!isId(id)) return false;
        pathIds.push_back(id.get<unsigned>());
    }
    if (!s.contains("term_id") || !isId(s["term_id"])) return false;
    const unsigned termId = s["term_id"].get<unsigned>();
    std::optional<int> bit;
    if (s.contains("bit") && s["bit"].is_number_integer()) bit = s["bit"].get<int>();
    // Anything on the net but the pin itself: it's connected.
    for (const auto& o : eq.occurrences)
        if (o.pathIds != pathIds || o.term.child_id != termId || o.term.bit != bit) return false;
    for (const auto& t : eq.terms)
        if (!pathIds.empty() || t.child_id != termId || t.bit != bit) return false;
    markUnconnected(pathIds, termId, bit);
    return true;
}

void EquipotentialView::addOnResolve(const InstancePath& path) { g_addOnResolve.insert(path); }

bool EquipotentialView::takeAddOnResolve(const InstancePath& path) {
    return g_addOnResolve.erase(path) > 0;
}

// One step of a pending focusObject(), on this frame's geometry: pan to the
// target if it's drawn, else ask for it once, else give up after a while.
static void advanceFocus(const ImVec2& canvasSize) {
    if (!g_focus) return;
    const auto& t = g_focus->target;
    const double now = ImGui::GetTime();
    ImVec2 lo, hi;
    bool found = false;
    if (t.termId) {
        if (auto pin = findFocusPin(t)) {
            const ImVec2 p = g_schematic.portWorldPos(*pin->first, *pin->second);
            lo = ImVec2(p.x - 30.f, p.y - 30.f);
            hi = ImVec2(p.x + 30.f, p.y + 30.f);
            found = true;
            g_focusPin = t;
            g_focusPinUntil = now + kFocusPinHighlight;
        }
    } else if (const InstanceShape* shape = findFocusShape(t.path)) {
        shapeWorldExtent(*shape, lo, hi);
        found = true;
    }
    if (found) {
        auto v = SchematicInteraction::revealRect(
            { g_schematic.transform.offset, g_schematic.transform.scale },
            lo, hi, canvasSize, kRevealMargin, g_schematic.minScale);
        g_schematic.transform.offset = v.offset;
        g_schematic.transform.scale  = v.scale;
        g_focus.reset();
        return;
    }
    if (g_focus->requested) {
        if (now - g_focus->since > kFocusTimeout) g_focus.reset();
        return;
    }
    // Not drawn: add it to the view -- a pin by its net. An instance's term
    // without a bit may be a whole bus, which has no one net: its instance
    // is added instead, every pin shown (a top-level bus just answers an
    // empty net).
    g_focus->requested = true;
    g_focus->since = now;
    if (!g_provider) { g_focus.reset(); return; }
    json req;
    if (t.termId && (t.bit || t.path.empty())) {
        req["request"] = "load_equipotential";
        req["path"]    = pathIds(t.path);
        req["term_id"] = *t.termId;
        if (t.bit) req["bit"] = *t.bit;
    } else {
        req["request"] = "resolve_instance";
        writePath(req, t.path);
        g_addOnResolve.insert(t.path);
    }
    g_provider->send(req.dump());
}

// The review badge under the cursor (screen position), if any: the threads
// it counts, for its tooltip and click.
static std::optional<SchematicView::ReviewBadge> pickReviewBadge(ImVec2 mouse) {
    std::optional<SchematicView::ReviewBadge> best;
    float bestD = 0.0f;
    for (const auto& b : g_schematic.reviewBadges(g_canvasOrigin, g_canvasSize)) {
        const float dx = mouse.x - b.center.x, dy = mouse.y - b.center.y;
        const float d = dx * dx + dy * dy;
        if (d <= b.radius * b.radius && (!best || d < bestD)) { best = b; bestD = d; }
    }
    return best;
}

static std::vector<const ReviewThread*> badgeThreads(const SchematicView::ReviewBadge& b) {
    if (b.portId >= 0) return pinReviewThreads(b.portId);
    const InstanceShape* shape = g_schematic.findInstanceById(b.instanceId);
    return shape ? ReviewStore::instanceThreads(shape->path) : std::vector<const ReviewThread*>{};
}
// One bus pin group: (instance, bus base name, is input).
using BusGroupKey = std::tuple<InstancePath, std::string, bool>;
// Merged bus-pin port id -> the bus group a double-click (or "Expand Bus")
// splits into its bits. Checked before g_portEquiByPortId. Rebuilt every
// frame.
static std::map<int, BusGroupKey> g_busGroupByPortId;
// Merged bus-pin port id -> the bits whose nets a click on it loads: those
// still open (not shown, not known to be unconnected). Rebuilt every frame.
static std::map<int, std::vector<PortEquiRequest>> g_busOpenBitsByPortId;
// Bit port id of a bus shown expanded -> its bus group, which a
// double-click on the bit (or "Collapse Bus") merges back into one pin.
// Clicking an open bit loads its net like any scalar pin. Rebuilt every
// frame.
static std::map<int, BusGroupKey> g_expandedBusGroupByPortId;

static std::map<InstancePath, std::vector<EquipotentialView::ExpandedPort>> g_expandedInstances;
static std::set<InstancePath> g_pendingExpansions;
// Bus groups currently shown expanded (individual bit pins/wires) instead of
// merged into one pin/wire.
static std::set<BusGroupKey> g_expandedBuses;

// Hierarchy grouping: when on, the leaf instances shown are laid out inside
// nested frames standing for the hierarchical modules that contain them
// (see SchematicLayout::layoutHierarchyGroups) instead of the flat column layout.
static bool g_showHierarchy = true;
// A "Zoom to Module" request from the canvas context menu: the module path
// of the frame to fit the view to once this frame's layout is known.
static std::optional<InstancePath> g_pendingZoomGroup;

// Instances shown on their own (showInstance), by instance path.
static std::map<InstancePath, EquipotentialView::StartInstance> g_startInstances;
// Top-level ports shown as flags with no net yet (showTopPorts), in the
// top's order.
static std::vector<EquipotentialView::ExpandedPort> g_startPorts;

// Fanout summaries (SchematicLayout::planFanout): the groups the user
// expanded, by the net (its first driver pin, or a top-level port by name)
// and the group's key; and which drawn shape stands for which group. A
// summary's made-up path segment ids start at kSummaryIdBase, far above any
// naja instance id; such a path is never sent to the host.
using FanoutNetKey = std::tuple<InstancePath, std::string, unsigned, int>;
using FanoutRef    = std::pair<FanoutNetKey, FanoutGroupKey>;
static std::set<FanoutRef>       g_expandedFanouts;
static std::map<int, FanoutRef>  g_summaryByShapeId;
static constexpr unsigned        kSummaryIdBase = 0xF0000000u;

static FanoutNetKey fanoutNetKey(const FanoutNet& net) {
    const auto& items = net.drivers.empty() ? net.receivers : net.drivers;
    if (items.empty()) return {};
    const Item& it = items.front();
    if (it.isTerm) return { InstancePath{}, it.label, 0u, -1 };
    return { it.path, "", it.termChildId, it.termBit.value_or(-1) };
}

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
    std::vector<ScanPinGroup> leftGroups;
    for (const auto& p : inst.ports) {
        (p.lx < 0.f ? left : right).push_back(p.name);
        if (p.lx < 0.f) leftGroups.push_back(scanPinGroup(p.role));
    }
    const SymbolGeometry g = symbolGeometry(inst.primitiveType, left, right, leftGroups);
    inst.w = g.w;
    inst.scanMux = g.scanMux;
    // Room for the bracket of an expanded bus, inside the box before its
    // bits' names (SchematicView's drawBusBrackets()).
    if (!isGateSymbol(inst.primitiveType) &&
        std::any_of(inst.ports.begin(), inst.ports.end(), [](const Port& p) { return p.busBracket >= 0; }))
        inst.w += kGrid;
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
//   - merged bus pin  -> add the nets of its open bits to the view (one
//                        load_equipotential per bit); the pin stays merged,
//                        so the bus is drawn as one wire where it can be
//   - open pin        -> add its net to the view (load_equipotential)
// A double-click toggles a bus between one pin and its bits: on a merged
// bus pin it shows the bits, on a bit it merges them back.
// A pin whose net is already shown has no click action (its right-click menu
// still offers Trace to Driver). Pins are hit-tested nearest-first with
// SchematicInteraction::pickPin against what was drawn last frame.
// ---------------------------------------------------------------------------
enum class PinAction { None, LoadBus, LoadNet, Loading };

static SchematicInteraction::PinRequestKey pinRequestKey(const PortEquiRequest& r) {
    return SchematicInteraction::pinRequestKey(r.pathIds, r.termId, r.bit);
}

static PinAction pinAction(const Port& p) {
    if (g_busGroupByPortId.count(p.id)) {
        auto bits = g_busOpenBitsByPortId.find(p.id);
        if (bits == g_busOpenBitsByPortId.end() || bits->second.empty()) return PinAction::None;
        for (const auto& b : bits->second)
            if (!g_pendingPinNets.isPending(pinRequestKey(b), ImGui::GetTime())) return PinAction::LoadBus;
        return PinAction::Loading;
    }
    if (p.open) return p.pending ? PinAction::Loading : PinAction::LoadNet;
    return PinAction::None;
}

// What a double-click on the pin does to its bus: split a merged pin into
// its bits (+1), merge a bit back with the rest (-1), or nothing (0).
static int busToggle(int portId) {
    if (g_busGroupByPortId.count(portId)) return +1;
    if (g_expandedBusGroupByPortId.count(portId)) return -1;
    return 0;
}

static void toggleBus(int portId) {
    if (auto it = g_busGroupByPortId.find(portId); it != g_busGroupByPortId.end())
        g_expandedBuses.insert(it->second);
    else if (auto bit = g_expandedBusGroupByPortId.find(portId); bit != g_expandedBusGroupByPortId.end())
        g_expandedBuses.erase(bit->second);
}

// The shape whose pin was clicked last (see requestPinNet()): a box by its
// path, a top-level port flag by its name.
static bool isKeptInPlace(const InstanceShape& inst) {
    if (inst.isHierGroup || inst.path != g_keepInPlacePath) return false;
    if (inst.modelName == "port")
        return !g_keepInPlacePort.empty() && !inst.ports.empty() && inst.ports[0].name == g_keepInPlacePort;
    return g_keepInPlacePort.empty();
}

// Sends load_equipotential for one pin's net unless it's already in flight.
// Returns whether it was sent.
static bool sendPinNet(const PortEquiRequest& r) {
    if (!g_provider || !g_pendingPinNets.begin(pinRequestKey(r), ImGui::GetTime())) return false;
    json j;
    j["request"] = "load_equipotential";
    j["path"]    = r.pathIds;
    j["term_id"] = r.termId;
    if (r.bit.has_value()) j["bit"] = r.bit.value();
    g_provider->send(j.dump());
    return true;
}

// Arranges for `inst` to stay where it is on screen, `port` under the
// cursor that clicked it, when the nets the click asked for arrive.
static void keepInPlace(const InstanceShape& inst, const Port& port) {
    g_skipNextAutoFit  = true;
    g_keepInPlacePath  = inst.path;
    g_keepInPlacePort  = inst.modelName == "port" ? port.name : std::string();
    g_keepInPlacePin   = port.name;
    g_keepInPlaceWorld = portAnchor(inst, port);
    g_keepInPlaceBoxWorld = ImVec2(inst.x, inst.y);
}

// Sends load_equipotential for `port` (sitting on `inst`) unless it's already
// in flight, and keeps `inst` in place when the net arrives.
static void requestPinNet(const InstanceShape& inst, const Port& port) {
    auto it = g_portEquiByPortId.find(port.id);
    if (it == g_portEquiByPortId.end()) return;
    if (sendPinNet(it->second)) keepInPlace(inst, port);
}

// The same for every open bit of a merged bus pin: the pin stays merged,
// and the bits' wires between two merged pins are drawn as one bus wire.
static void requestBusNets(const InstanceShape& inst, const Port& port) {
    auto it = g_busOpenBitsByPortId.find(port.id);
    if (it == g_busOpenBitsByPortId.end()) return;
    bool sent = false;
    for (const auto& r : it->second) sent = sendPinNet(r) || sent;
    if (sent) keepInPlace(inst, port);
}

// Runs the click action of the pin under `wp`, if any. Returns true when a
// pin was hit (even one with no action), so the click isn't also treated as
// a click on the box body underneath.
// The routed wire tree under world point `wp`, as an index in
// g_schematic.routes.
static std::optional<size_t> pickRoute(const ImVec2& wp) {
    std::vector<const std::vector<SchematicLayout::RouteSegment>*> wires;
    wires.reserve(g_schematic.routes.size());
    for (const auto& r : g_schematic.routes) wires.push_back(&r.segments);
    return SchematicInteraction::pickWire(wires, wp, g_schematic.transform.scale);
}

static bool clickPin(const ImVec2& wp) {
    auto hit = SchematicInteraction::pickPin(g_schematic.instances, wp, g_schematic.transform.scale);
    if (!hit) return false;
    InstanceShape* inst = g_schematic.findInstanceById(hit->instanceId);
    Port* port = inst ? g_schematic.findPortById(*inst, hit->portId) : nullptr;
    if (!port) return true;
    switch (pinAction(*port)) {
        case PinAction::LoadBus:   requestBusNets(*inst, *port); break;
        case PinAction::LoadNet:   requestPinNet(*inst, *port); break;
        default: break;
    }
    return true;
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
std::string EquipotentialView::exportSvg() { return g_schematic.exportSvg(); }
std::string EquipotentialView::exportPng(float scale) { return g_schematic.exportPng(scale); }
bool EquipotentialView::hasSchematic() { return !g_schematic.instances.empty(); }
void EquipotentialView::clearNets() { g_pendingClear = true; }

void EquipotentialView::resetLayout() {
    g_startInstances.clear();
    g_startPorts.clear();
    g_keepInPlacePort.clear();
    g_expandedInstances.clear();
    g_pendingExpansions.clear();
    g_expandedBuses.clear();
    g_opened.clear();
    g_openPending.clear();
    g_boundaryPins.clear();
    g_skipNextAutoFit = false;
    g_keepInPlacePath.clear();
    g_keepInPlacePin.clear();
    g_lastLayout.clear();
    g_layoutHints.clear();
    g_layoutSignature = {};
    g_pendingPinNets.clear();
    g_unconnectedPins.clear();
    g_focus.reset();
    g_focusPin.reset();
    g_addOnResolve.clear();
    g_expandedFanouts.clear();
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

static std::optional<std::pair<ImVec2, ImVec2>> g_cameraRect;
void EquipotentialView::setCameraRect(std::optional<std::pair<ImVec2, ImVec2>> rect) { g_cameraRect = rect; }
const SchematicView& EquipotentialView::schematicForTesting() { return g_schematic; }
ImVec2 EquipotentialView::canvasOriginForTesting() { return g_canvasOrigin; }
ImVec2 EquipotentialView::canvasSizeForTesting()   { return g_canvasSize; }

void EquipotentialView::setShowHierarchy(bool on) {
    if (g_showHierarchy == on) return;
    g_showHierarchy = on;
    g_pendingFit    = true;
}

// The pins the host flagged unconnected, marked so they're drawn with
// their no-connect cross without being clicked first.
static void markUnconnectedPorts(const std::vector<unsigned>& ids,
                                 const std::vector<EquipotentialView::ExpandedPort>& ports) {
    for (const auto& ep : ports)
        if (ep.unconnected) EquipotentialView::markUnconnected(ids, ep.childId, ep.bit);
}

void EquipotentialView::showInstance(const StartInstance& start) {
    if (start.path.empty()) return;  // the top design has no box of its own
    g_startInstances[start.path]    = start;
    g_expandedInstances[start.path] = start.ports;
    markUnconnectedPorts(pathIds(start.path), start.ports);
}

void EquipotentialView::showTopPorts(const std::vector<ExpandedPort>& ports) {
    g_startPorts = ports;
    markUnconnectedPorts({}, ports);
}

bool EquipotentialView::hasStartShapes() {
    return !g_startInstances.empty() || !g_startPorts.empty() || !g_opened.empty();
}

void EquipotentialView::requestTopView(bool contents, bool automatic) {
    if (!g_provider) return;
    json req;
    req["request"]       = "load_top_view";
    req["contents"]      = contents;
    req["max_instances"] = kMaxTopViewInstances;
    if (automatic) req["auto"] = true;
    g_provider->send(req.dump());
}

std::vector<EquipotentialView::ExpandedPort> EquipotentialView::portsFromJson(const json& terms) {
    std::vector<ExpandedPort> ports;
    if (!terms.is_array()) return ports;
    for (const auto& t : terms) {
        ExpandedPort ep;
        ep.childId = t.value("child_id", 0u);
        ep.name    = displayPinLabel(t.value("name", std::string("")), ep.childId);
        if (t.contains("bit") && !t["bit"].is_null()) ep.bit = t["bit"].get<int>();
        ep.clock   = t.value("clock", false);
        ep.role    = t.value("role", std::string());
        ep.unconnected = t.value("unconnected", false);
        int dirInt   = t.value("direction", 0);
        ep.direction = dirInt == 1 ? Direction::Output
                     : dirInt == 2 ? Direction::Inout : Direction::Input;
        ports.push_back(std::move(ep));
    }
    return ports;
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
    start.hasSources = inst.value("has_sources", false);
    start.ports = portsFromJson(inst.value("terms", json::array()));
    return start;
}

void EquipotentialView::applyInstanceExpansion(
        const InstancePath& path,
        const std::vector<ExpandedPort>& ports) {
    g_expandedInstances[path] = ports;
    g_pendingExpansions.erase(path);
    markUnconnectedPorts(pathIds(path), ports);
}

void EquipotentialView::openInstance(const InstancePath& path) {
    if (!g_provider || path.empty() || g_opened.count(path) || !g_openPending.insert(path).second) return;
    json req;
    req["request"]       = "load_instance_view";
    writePath(req, path);
    req["max_instances"] = kMaxTopViewInstances;
    g_provider->send(req.dump());
}

void EquipotentialView::closeInstance(const InstancePath& path) {
    // With every instance opened inside it: their contents are drawn in
    // its frame, which is a box again.
    for (auto it = g_opened.begin(); it != g_opened.end();) {
        const InstancePath& p = it->first;
        const bool inside = p.size() >= path.size() && std::equal(path.begin(), path.end(), p.begin());
        it = inside ? g_opened.erase(it) : std::next(it);
    }
    g_pendingFit = true;
}

bool EquipotentialView::isOpened(const InstancePath& path) { return g_opened.count(path) > 0; }

void EquipotentialView::applyInstanceView(const json& reply) {
    auto path = readPath(reply);
    if (!path) {
        Console::Error("instance_view_response without a valid id_path");
        return;
    }
    // Not asked for, or closed meanwhile (a reset): nothing to open.
    if (!g_openPending.erase(*path)) return;
    const std::string name = displayPath(*path);
    if (!reply.value("found", false)) {
        Console::Error("No instance '" + name + "' in the design");
        return;
    }
    const size_t count = reply.value("instance_count", size_t(0));
    if (!reply.value("contents", false)) {
        Console::Log(name + " has " + std::to_string(count) + " instances, more than " +
                     std::to_string(kMaxTopViewInstances) + " can be drawn at once: not opened. "
                     "Click its pins to add their nets, or right-click them to trace their fan-in.");
        return;
    }
    OpenedInstance oi;
    oi.path = *path;
    if (reply.contains("nets") && reply["nets"].is_array())
        for (const auto& n : reply["nets"]) oi.nets.push_back(n.get<Equipotential>());
    // Its model names, from any pin inside it (each path ends with the
    // child's level).
    for (const auto& net : oi.nets)
        for (const auto& occ : net.occurrences)
            if (oi.pathModels.empty() && occ.pathModels.size() == path->size() + 1)
                oi.pathModels.assign(occ.pathModels.begin(), occ.pathModels.end() - 1);
    if (oi.pathModels.empty()) oi.pathModels.assign(path->size(), "");
    Console::Log("Opened " + name + ": " + std::to_string(count) + " instance(s), " +
                 std::to_string(oi.nets.size()) + " net(s)");
    g_opened[*path] = std::move(oi);
    g_pendingFit = true;
}

// ---------------------------------------------------------------------------
// Color helpers
// ---------------------------------------------------------------------------
namespace {
// A pin's direction as text: a driver (top-level input, instance output),
// a receiver, an inout, in the theme's colors.
ImColor topTermColor(Direction d) {
    switch (d) {
        case Direction::Input:  return ImColor(theme().driverText);
        case Direction::Output: return ImColor(theme().receiverText);
        default:                return ImColor(theme().inoutText);
    }
}
ImColor occTermColor(Direction d) {
    switch (d) {
        case Direction::Input:  return ImColor(theme().receiverText);
        case Direction::Output: return ImColor(theme().driverText);
        default:                return ImColor(theme().inoutText);
    }
}

// Faint gray for a net that only hidden traces keep in the view.
constexpr ImU32 kFaintWireColor = IM_COL32(150, 150, 150, 55);

void mergeTraceIds(std::vector<int>& into, const std::vector<int>& ids) {
    for (int id : ids)
        if (std::find(into.begin(), into.end(), id) == into.end()) into.push_back(id);
}

// The drawn color of a wire (or routed tree): a diagnosis color already in
// `color` wins; else one visible trace gives its color, two or more the
// convergence color; else faint if only hidden traces show it.
ImU32 traceWireColor(ImU32 color, const std::vector<int>& traceIds, bool faint) {
    if (color != NetWire{}.color) return color;
    if (traceIds.size() >= 2) return TraceStore::convergenceColor();
    if (traceIds.size() == 1) return TraceStore::color(traceIds.front());
    return faint ? kFaintWireColor : color;
}

// The same with host styling (StyleStore) in between: a diagnosis color in
// `color` wins, then the style's color, then the traces'.
ImU32 styledWireColor(ImU32 color, ImU32 styleColor, const std::vector<int>& traceIds, bool faint) {
    if (color != NetWire{}.color || styleColor == 0) return traceWireColor(color, traceIds, faint);
    return styleColor;
}
} // namespace

// ---------------------------------------------------------------------------
// renderSchematic
// ---------------------------------------------------------------------------
void EquipotentialView::renderSchematic(const std::vector<Equipotential*>& shownNets) {
    // With instances opened, what's drawn is the shown nets joined to their
    // contents (SchematicLayout::openInstances()), rebuilt every frame like
    // the layout.
    static OpenedView s_openedView;
    std::vector<Equipotential*> equipotentials;
    if (g_opened.empty()) {
        equipotentials = shownNets;
        g_boundaryPins.clear();
    } else {
        s_openedView = openInstances(std::vector<const Equipotential*>(shownNets.begin(), shownNets.end()),
                                     g_opened);
        for (auto& net : s_openedView.nets) equipotentials.push_back(&net);
        g_boundaryPins = s_openedView.boundary;
    }
    // No scrollbars while a recording drives the camera.
    const float scrollSz = g_cameraRect ? 0.0f : ImGui::GetFrameHeight();
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
    g_canvasSize     = inner;

    g_schematic.handleInteraction(cpos, inner);

    if (g_pendingZoomSteps) {
        float f = g_pendingZoomSteps > 0 ? 1.1f : 0.9f;
        for (int i = 0; i < std::abs(g_pendingZoomSteps); ++i)
            g_schematic.zoomBy(f, ImVec2(inner.x * 0.5f, inner.y * 0.5f));
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
        g_ctxSummaryId  = -1;
        if (auto hit = SchematicInteraction::pickPin(g_schematic.instances, wp, g_schematic.transform.scale))
            if (g_portEquiByPortId.count(hit->portId) || g_busGroupByPortId.count(hit->portId))
                g_ctxPortId = hit->portId;
        for (auto rit = g_schematic.instances.rbegin(); rit != g_schematic.instances.rend(); ++rit) {
            if (rit->w <= 0.f || rit->h <= 0.f) continue;
            if (wp.x < rit->x || wp.x > rit->x + rit->w) continue;
            if (wp.y < rit->y || wp.y > rit->y + rit->h) continue;
            if (g_occInfoByShapeId.count(rit->id)) g_ctxInstanceId = rit->id;
            if (g_summaryByShapeId.count(rit->id)) g_ctxSummaryId = rit->id;
            break;
        }
        if (g_ctxPortId >= 0) g_ctxInstanceId = -1;
        ImGui::OpenPopup("##ctx");
    }
    auto canvasMenuItems = [] {
        if (ImGui::MenuItem("Clear all nets")) g_pendingClear = true;
        if (ImGui::MenuItem("Fit view"))       g_schematic.requestFit(true);
        if (!g_expandedFanouts.empty() && ImGui::MenuItem("Collapse Fanout Groups")) g_expandedFanouts.clear();
        if (!g_opened.empty() && ImGui::MenuItem("Close Opened Instances")) {
            g_opened.clear();
            g_pendingFit = true;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Show Hierarchy", nullptr, g_showHierarchy)) setShowHierarchy(!g_showHierarchy);
    };
    if (ImGui::BeginPopup("##ctx")) {
        auto occIt = g_ctxInstanceId >= 0 ? g_occInfoByShapeId.find(g_ctxInstanceId)
                                          : g_occInfoByShapeId.end();
        auto portIt = g_ctxPortId >= 0 ? g_portEquiByPortId.find(g_ctxPortId)
                                       : g_portEquiByPortId.end();
        if (portIt != g_portEquiByPortId.end()) {
            if (ImGui::MenuItem("Trace to Driver (add to view)") && g_provider) {
                // Adds to the view (like a pin click) instead of clearing
                // it: the cone is overlaid on what's shown, as its own trace.
                std::string label;
                for (auto& inst : g_schematic.instances) {
                    const Port* p = g_schematic.findPortById(inst, g_ctxPortId);
                    if (!p) continue;
                    label = pinLabel(inst, p->name);
                    break;
                }
                json req;
                req["request"]  = "trace_driver";
                req["path"]     = portIt->second.pathIds;
                req["term_id"]  = portIt->second.termId;
                if (portIt->second.bit.has_value()) req["bit"] = portIt->second.bit.value();
                req["trace_id"] = TraceStore::begin(label);
                g_provider->send(req.dump());
            }
            // A pin's term, by its ids (a top-level port stub's path is {}).
            for (auto& inst : g_schematic.instances) {
                const Port* p = g_schematic.findPortById(inst, g_ctxPortId);
                if (!p) continue;
                ReviewView::commentMenuItem(
                    ReviewStore::termAnchor(pinOwnerPath(inst), portIt->second.termId, p->name,
                                            portIt->second.bit), pinLabel(inst, p->name));
                break;
            }
            auto busIt = g_expandedBusGroupByPortId.find(g_ctxPortId);
            if (busIt != g_expandedBusGroupByPortId.end() && ImGui::MenuItem("Collapse Bus"))
                g_expandedBuses.erase(busIt->second);
        } else if (g_busGroupByPortId.count(g_ctxPortId)) {
            // A merged bus pin: load its bits' nets, or show its bits.
            for (auto& inst : g_schematic.instances) {
                const Port* p = g_schematic.findPortById(inst, g_ctxPortId);
                if (!p) continue;
                if (ImGui::MenuItem("Add Bus Nets", nullptr, false, pinAction(*p) == PinAction::LoadBus))
                    requestBusNets(inst, *p);
                break;
            }
            if (ImGui::MenuItem("Expand Bus")) toggleBus(g_ctxPortId);
        } else if (auto sumIt = g_summaryByShapeId.find(g_ctxSummaryId); sumIt != g_summaryByShapeId.end()) {
            const auto* shape = g_schematic.findInstanceById(g_ctxSummaryId);
            const std::string item = "Expand " + std::to_string(shape ? shape->summaryCount : 0) + " Sinks";
            if (ImGui::MenuItem(item.c_str())) g_expandedFanouts.insert(sumIt->second);
            ImGui::Separator();
            canvasMenuItems();
        } else if (occIt != g_occInfoByShapeId.end()) {
            if (occIt->second.hasSources && ImGui::MenuItem("Show Sources") && g_provider) {
                json req;
                req["request"] = "get_sources";
                req["kind"]    = "instance";
                writePath(req, occIt->second.path);
                g_provider->send(req.dump());
            }
            if (ImGui::MenuItem("Show Properties") && g_provider) {
                json req;
                req["request"] = "get_properties";
                req["kind"]    = "instance";
                writePath(req, occIt->second.path);
                g_provider->send(req.dump());
            }
            ReviewView::commentMenuItem(ReviewStore::instanceAnchor(occIt->second.path),
                                        displayPath(occIt->second.path));
            auto* shape = g_schematic.findInstanceById(g_ctxInstanceId);
            if (shape && !shape->isHierGroup && shape->hasChildren &&
                ImGui::MenuItem("Open Instance"))
                openInstance(occIt->second.path);
            if (shape && shape->isHierGroup && g_opened.count(occIt->second.path) &&
                ImGui::MenuItem("Close Instance"))
                closeInstance(occIt->second.path);
            // A module frame covers a lot of canvas, so right-clicking its
            // empty area also offers the canvas-level entries.
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

    // Single click: a review badge opens its thread in the Review tab; an
    // instance's hierarchy glyph expands/collapses its
    // sub-instances nested inside its box; otherwise a pin runs its click
    // action (see clickPin), a wire selects its net and a box body its
    // instance. The glyph is checked first since it's a small,
    // distinct hotspot (top-center of the box) that never overlaps a pin.
    // Only the first click of a double-click counts, so a double-click
    // doesn't act twice (e.g. load a bus's nets, then load the bit its
    // double-click split off under the cursor).
    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        ImGui::GetMouseClickedCount(ImGuiMouseButton_Left) == 1) {
        ImVec2 wp = mouseWorldPos(g_schematic, cpos);
        bool badgeHit = false;
        if (auto badge = pickReviewBadge(ImGui::GetMousePos())) {
            const auto threads = badgeThreads(*badge);
            badgeHit = !threads.empty();
            ReviewView::showThreads(threads);
        }
        bool glyphHit = false;
        for (const auto& inst : g_schematic.instances) {
            if (badgeHit) break;
            if (!canShowHierToggle(inst)) continue;
            float gx0, gy0, gx1, gy1;
            hierToggleGlyphRect(inst, gx0, gy0, gx1, gy1);
            if (wp.x < gx0 || wp.x > gx1 || wp.y < gy0 || wp.y > gy1) continue;
            glyphHit = true;
            if (inst.isHierGroup) closeInstance(inst.path);
            else                  openInstance(inst.path);
            break;
        }
        if (badgeHit || glyphHit || clickPin(wp)) {
            // handled
        } else if (auto wire = pickRoute(wp)) {
            // On a wire (routes avoid boxes, but cross module frames): select
            // its net.
            SelectionStore::selectNet(g_schematic.routes[*wire].ref);
        } else {
            // On a box body: select that instance (or module frame). Reverse
            // scan so the innermost box under the cursor wins.
            for (auto rit = g_schematic.instances.rbegin(); rit != g_schematic.instances.rend(); ++rit) {
                if (rit->w <= 0.f || rit->h <= 0.f) continue;
                if (wp.x < rit->x || wp.x > rit->x + rit->w) continue;
                if (wp.y < rit->y || wp.y > rit->y + rit->h) continue;
                // A fanout summary: expand it into its sinks.
                if (auto sumIt = g_summaryByShapeId.find(rit->id); sumIt != g_summaryByShapeId.end()) {
                    g_expandedFanouts.insert(sumIt->second);
                    break;
                }
                auto occIt = g_occInfoByShapeId.find(rit->id);
                if (occIt != g_occInfoByShapeId.end())
                    SelectionStore::select(occIt->second.path, SelectionStore::Origin::Schematic);
                break;
            }
        }
    }

    // Double-click on a bus pin: split a merged bus pin into its bits, or
    // merge a bit back with the rest of its bus (the first click of the
    // double-click already ran the pin's click action above).
    // Double-click on a box body: a partial-interface box loads its full
    // interface, revealing the pins whose nets aren't shown yet.
    if (ImGui::IsItemHovered() &&
        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && g_provider) {
        ImVec2 wp = mouseWorldPos(g_schematic, cpos);
        const float wx = wp.x, wy = wp.y;
        const auto pinHit = SchematicInteraction::pickPin(g_schematic.instances, wp,
                                                          g_schematic.transform.scale);
        if (pinHit) toggleBus(pinHit->portId);
        const bool hit = pinHit.has_value() || pickReviewBadge(ImGui::GetMousePos()).has_value();
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
    g_schematic.portBrackets.clear();
    g_schematic.nets.clear();
    g_occInfoByShapeId.clear();
    g_portEquiByPortId.clear();
    g_busGroupByPortId.clear();
    g_expandedBusGroupByPortId.clear();
    g_busOpenBitsByPortId.clear();
    g_busTermByPortId.clear();

    int nextInstId = 1, nextPortId = 1, totalItems = 0;

    // Pass 1: collect merged instance data across all equips
    struct PortSlot {
        int                   portId;
        std::string           name;
        Direction             direction;
        unsigned              termChildId;
        std::optional<int>    termBit;
        bool                  clock = false;
        std::string           role;
        std::vector<unsigned> pathIds;
    };
    struct MInst {
        bool                  initialized = false;
        DesignRef             designRef{};
        PrimitiveType         primitiveType = PrimitiveType::Unknown;
        bool                  hasInstances = false;
        std::optional<size_t> bitTermCount;
        bool                     hasSources = false;
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
    // Net name per equipotential: the driver's net at the driver's own level
    // (its pin's "net"), shown next to a merged bus wire's slash mark and as
    // the SVG's data-net. The driving pin's label when the host sent no net
    // name (older hosts).
    std::vector<std::string> netNameByEi(equipotentials.size());
    // The visible traces each net is on, and whether only hidden traces keep
    // it in the view; per instance, the visible traces reaching it -- two or
    // more make it a convergence point.
    std::vector<std::vector<int>> traceIdsByEi(equipotentials.size());
    std::vector<bool> faintByEi(equipotentials.size(), false);
    std::map<InstancePath, std::vector<int>> traceIdsByInstance;

    // Every net's ends, and the sinks gathered into fanout summaries: never
    // an instance shown on its own, selected, diagnosed or styled.
    const ViewOptions& viewOptions = ViewOptions::current();
    std::vector<FanoutNet> fanNets(equipotentials.size());
    std::set<InstancePath> pinned;
    for (const auto& [path, start] : g_startInstances) pinned.insert(path);
    for (const auto& [path, pin] : g_boundaryPins) pinned.insert(path);
    if (SelectionStore::hasSelection()) pinned.insert(SelectionStore::selected());
    for (size_t ei = 0; ei < equipotentials.size(); ++ei) {
        Equipotential* eq = equipotentials[ei];
        if (!eq || (eq->terms.empty() && eq->occurrences.empty())) continue;
        buildItems(eq, fanNets[ei].drivers, fanNets[ei].receivers);
        // Sinks in natural name order: the first max_sinks drawn, the rest
        // summarized, both in the order a reader expects (lut_111 < lut_1037).
        std::stable_sort(fanNets[ei].receivers.begin(), fanNets[ei].receivers.end(),
                         [](const Item& a, const Item& b) {
                             if (a.isTerm != b.isTerm) return a.isTerm;
                             if (a.isTerm) return naturalLess(a.label, b.label);
                             if (naturalPathLess(a.path, b.path)) return true;
                             if (naturalPathLess(b.path, a.path)) return false;
                             return naturalLess(a.label, b.label);
                         });
        for (const auto& r : fanNets[ei].receivers)
            if (!r.isTerm && (DiagnosisStore::instanceColor(r.path) != 0 || !StyleStore::instanceStyle(r.path).empty()))
                pinned.insert(r.path);
    }
    std::vector<FanoutNetKey> fanNetKeys;
    for (const auto& net : fanNets) fanNetKeys.push_back(fanoutNetKey(net));
    const FanoutPlan fanPlan = planFanout(
        fanNets, viewOptions.maxSinks, viewOptions.groupBy, pinned,
        [&](size_t ni, const FanoutGroupKey& key) { return g_expandedFanouts.count({ fanNetKeys[ni], key }) > 0; });

    for (size_t ei = 0; ei < equipotentials.size(); ++ei) {
        Equipotential* eq = equipotentials[ei];
        if (!eq || (eq->terms.empty() && eq->occurrences.empty())) continue;
        for (int id : eq->traceIds)
            if (TraceStore::isVisible(id)) traceIdsByEi[ei].push_back(id);
        faintByEi[ei] = !eq->direct && !eq->traceIds.empty() && traceIdsByEi[ei].empty();

        const std::vector<Item>& drivers   = fanNets[ei].drivers;
        const std::vector<Item>& receivers = fanNets[ei].receivers;
        totalItems += int(drivers.size() + receivers.size());
        if (!drivers.empty())
            netNameByEi[ei] = drivers[0].netName.empty() ? drivers[0].label : drivers[0].netName;

        for (int pass = 0; pass < 2; ++pass) {
            const auto& items = pass == 0 ? drivers : receivers;
            for (size_t ii = 0; ii < items.size(); ++ii) {
                const Item& item = items[ii];
                if (pass == 1 && fanPlan.groupOf[ei][ii] >= 0) continue;  // in a summary
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
                    mi.hasSources    = item.hasSources;
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
                    ps.role        = item.role;
                    ps.pathIds     = item.pathIds;
                    mi.ports.push_back(std::move(ps));
                    g_portEquiByPortId[pid] = { item.pathIds, item.termChildId, item.termBit };
                    // This pin's net is in the view now: its request is done.
                    g_pendingPinNets.resolve(pinRequestKey(g_portEquiByPortId[pid]));
                }
                equiEnds[ei].push_back({ false, item.path, "", pid, -1, pass == 0 });
                // Only the traces whose reply listed this instance on the
                // net: another reader of a net a trace shares isn't on it.
                for (int id : traceIdsByEi[ei]) {
                    auto reached = eq->traceInstances.find(id);
                    if (reached == eq->traceInstances.end() || reached->second.count(item.path))
                        mergeTraceIds(traceIdsByInstance[item.path], {id});
                }
            }
        }
    }

    // Instances shown on their own (showInstance): a box even with no net
    // through it yet. Their pins come from g_expandedInstances, all open
    // until a net reaches them.
    totalItems += int(g_startPorts.size());
    for (const auto& [path, start] : g_startInstances) {
        if (g_opened.count(path)) continue;  // drawn as its contents
        ++totalItems;
        auto& mi = minsts[path];
        if (mi.initialized) continue;
        mi.initialized  = true;
        mi.designRef    = start.designRef;
        mi.primitiveType = start.primitiveType;
        mi.hasInstances = start.hasInstances;
        mi.bitTermCount = start.ports.size();
        mi.hasSources   = start.hasSources;
        mi.path         = start.path;
        mi.pathModels   = start.pathModels;
        mi.pathIds      = start.pathIds;
    }

    // Each fanout summary: a one-pin box on its net, under a made-up path
    // in the deepest module holding its sinks (so Show Hierarchy frames it
    // with them).
    std::map<InstancePath, size_t> summaryOfPath;
    for (size_t gi = 0; gi < fanPlan.groups.size(); ++gi) {
        const FanoutGroup& g = fanPlan.groups[gi];
        InstancePath path = g.module;
        path.push_back(InstanceRef{ kSummaryIdBase + unsigned(gi), "" });
        summaryOfPath[path] = gi;
        auto& mi = minsts[path];
        mi.initialized  = true;
        mi.path         = path;
        mi.pathModels   = g.moduleModels;
        mi.pathModels.push_back("");
        mi.bitTermCount = 1;
        PortSlot ps;
        ps.portId    = nextPortId++;
        ps.name      = g.pin;
        ps.direction = Direction::Input;
        ps.clock     = g.clock;
        mi.ports.push_back(ps);
        equiEnds[g.net].push_back({ false, path, "", ps.portId, -1, false });
        totalItems += 1;
    }

    // Pass 2: build InstanceShapes from merged data
    std::map<InstancePath, int> pathToInstId;
    g_summaryByShapeId.clear();
    // Per-bit port id (as assigned in pass 1, or freshly allocated below for
    // an expanded-instance port not backed by a loaded equipotential) ->
    // the port id actually drawn, i.e. itself, or the id of the merged bus
    // pin it collapsed into. Used by pass 4 to redirect wire endpoints.
    std::map<int, int> logicalToRenderedPortId;

    for (auto& [key, mi] : minsts) {
        // An opened instance's pin, alone on its net in view: a port flag in
        // its frame, placed like a box (the size of the flag and its name)
        // and turned into a flag once laid out (see below).
        if (auto bp = g_boundaryPins.find(key); bp != g_boundaryPins.end() && !mi.ports.empty()) {
            const BoundaryPin& pin = bp->second;
            InstanceShape inst;
            inst.id        = nextInstId++;
            inst.path      = key;
            inst.name      = mi.ports[0].name;
            inst.modelName = "port";
            inst.partialInterface = false;
            Port p;
            p.id        = mi.ports[0].portId;
            p.name      = mi.ports[0].name;
            p.direction = pin.term.direction;
            p.isInput   = pin.term.direction == Direction::Input;  // drawn as an input port
            p.lx        = p.isInput ? 0.5f : -0.5f;                 // its wire goes in
            p.ly        = 0.f;
            p.color     = DiagnosisStore::netColor(pin.owner, stripBusIndex(p.name));
            inst.w      = boundaryFlagWidth(p.name);
            inst.h      = boundaryFlagHeight();
            inst.ports.push_back(p);
            pathToInstId[key] = inst.id;
            g_schematic.instances.push_back(std::move(inst));
            continue;
        }
        auto expIt = g_expandedInstances.find(key);
        bool isExp = expIt != g_expandedInstances.end() && !expIt->second.empty();

        InstanceShape inst;
        inst.id    = nextInstId++;
        inst.path      = key;
        inst.name      = displayPath(key);
        inst.primitiveType = mi.primitiveType;
        inst.diagOutline = DiagnosisStore::instanceColor(key);
        applyInstanceStyle(inst);
        // A gate two visible traces go through: where they converge -- not
        // on a box the host styles (it marks it its own way), nor when the
        // convergence_outline view option is off.
        if (inst.diagOutline == 0 && traceIdsByInstance[key].size() >= 2 &&
            viewOptions.convergenceOutline && StyleStore::instanceStyle(key).empty())
            inst.diagOutline = TraceStore::convergenceColor();
        inst.selected    = SelectionStore::isSelected(key);
        setReviewBadge(inst);
        pathToInstId[key] = inst.id;
        if (auto sit = summaryOfPath.find(key); sit != summaryOfPath.end()) {
            const FanoutGroup& g = fanPlan.groups[sit->second];
            inst.isSummary    = true;
            inst.summaryCount = int(g.count);
            inst.summaryGroup = g.key.clock ? "clock"
                              : viewOptions.groupBy == FanoutGroupBy::Hierarchy ? displayPath(g.key.module)
                                                                                : g.key.text;
            inst.name         = g.label;
            inst.label        = g.label;
            g_summaryByShapeId[inst.id] = { fanNetKeys[g.net], g.key };
        } else {
            g_occInfoByShapeId[inst.id] = { key, mi.designRef, mi.hasSources };
            inst.model     = mi.pathModels.empty() ? "" : mi.pathModels.back();
            inst.showModel = viewOptions.instanceLabel != InstanceLabel::Name;
            inst.hideName  = viewOptions.instanceLabel == InstanceLabel::Model && !inst.model.empty();
        }

        // A box whose model has sub-instances can be opened (its "+").
        inst.hasChildren  = mi.hasInstances;

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

            std::map<BusGroupKey, int> bracketOf;  // expanded bus -> its index on this box
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
                    // Open while a bit's net can still be loaded; not
                    // connected when no bit is.
                    p.unconnected = true;
                    for (auto* m : row.members) {
                        const PortEquiRequest& r = g_portEquiByPortId[m->pid];
                        const bool nc = g_unconnectedPins.count(pinRequestKey(r)) > 0;
                        p.open        = p.open || (m->open && !nc);
                        p.unconnected = p.unconnected && nc;
                        if (m->open && !nc) g_busOpenBitsByPortId[p.id].push_back(r);
                    }
                } else {
                    p.id   = row.members[0]->pid;
                    p.name = row.members[0]->ep->name;
                    p.clock = row.members[0]->ep->clock;
                    p.role  = row.members[0]->ep->role;
                    p.color = DiagnosisStore::netColor(key, stripBusIndex(row.members[0]->ep->name));
                    if (row.isExpandedBusBit) {
                        g_expandedBusGroupByPortId[p.id] = row.groupKey;
                        p.busBracket = bracketOf.try_emplace(row.groupKey, int(bracketOf.size())).first->second;
                    }
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

            std::map<BusGroupKey, int> bracketOf;  // expanded bus -> its index on this box
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
                    p.role  = row.members[0]->role;
                    p.color = DiagnosisStore::netColor(key, stripBusIndex(row.members[0]->name));
                    if (row.isExpandedBusBit) {
                        g_expandedBusGroupByPortId[p.id] = row.groupKey;
                        p.busBracket = bracketOf.try_emplace(row.groupKey, int(bracketOf.size())).first->second;
                    }
                }
                p.lx = isIn ? -0.5f : 0.5f;
                p.direction = row.members[0]->direction;
                p.isInput   = !isIn;
                inst.ports.push_back(p);
            }
        }
        applySymbolGeometry(inst, 0.f);
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

    // A host style's label on a net goes right past its driver pin (an
    // instance's right-side pin here, a top-level input port in pass 3), in
    // room the layout makes for it (SchematicLayout::netLabelPlaces()).
    std::vector<std::string> styleLabelByEi(equipotentials.size());
    for (size_t ei = 0; ei < equipotentials.size(); ++ei) {
        if (!equipotentials[ei]) continue;
        const std::string label = StyleStore::netStyle(*equipotentials[ei]).label;
        if (!label.empty()) styleLabelByEi[ei] = shortenMiddle(label, netLabelWidth, kNameMaxW);
    }
    for (size_t ei = 0; ei < equipotentials.size(); ++ei) {
        if (styleLabelByEi[ei].empty()) continue;
        for (const auto& we : equiEnds[ei]) {
            if (we.isTerm || !we.drives) continue;
            auto kit = pathToInstId.find(we.path);
            InstanceShape* s = kit != pathToInstId.end() ? g_schematic.findInstanceById(kit->second) : nullptr;
            Port* p = s ? g_schematic.findPortById(*s, renderedPort(we.portId)) : nullptr;
            if (!p || p->lx < 0.f || !p->netLabel.empty()) continue;
            p->netLabel  = styleLabelByEi[ei];
            p->netLabelW = netLabelWidth(p->netLabel);
        }
    }

    // Layered placement of the instance boxes (see SchematicLayout): every
    // net between two instances is an edge from its driver pin to each
    // receiver pin. A box takes its footprint's width: its name and net
    // labels included.
    std::map<int, int> nodeOfShape;
    std::vector<PlaceNode> placeNodes;
    for (const auto& inst : g_schematic.instances) {
        nodeOfShape[inst.id] = int(placeNodes.size());
        placeNodes.push_back({ footprintWidth(inst, instanceNameWidth),
                               footprintHeight(inst, instanceNameWidth) });
    }
    // Loads of one pin go in natural name order, summaries after them.
    {
        std::vector<const InstanceShape*> byName;
        for (const auto& inst : g_schematic.instances) byName.push_back(&inst);
        std::stable_sort(byName.begin(), byName.end(), [](const InstanceShape* a, const InstanceShape* b) {
            if (a->isSummary != b->isSummary) return b->isSummary;
            return a->isSummary ? naturalLess(a->name, b->name) : naturalPathLess(a->path, b->path);
        });
        for (size_t r = 0; r < byName.size(); ++r) placeNodes[size_t(nodeOfShape[byName[r]->id])].rank = int(r);
    }
    std::vector<PlaceNet> placeNets;
    for (const auto& ends : equiEnds) {
        PlaceNet pn;
        // A top-level term receiving the net is an output port.
        const bool toOutput = std::any_of(ends.begin(), ends.end(),
                                          [](const WireEnd& we) { return we.isTerm && !we.drives; });
        for (const auto& we : ends) {
            if (we.isTerm) continue;
            auto kit = pathToInstId.find(we.path);
            if (kit == pathToInstId.end()) continue;
            InstanceShape* s = g_schematic.findInstanceById(kit->second);
            const Port* p = s ? g_schematic.findPortById(*s, renderedPort(we.portId)) : nullptr;
            if (!p) continue;
            PlacePin pin{ nodeOfShape[s->id], (p->ly + 0.5f) * s->h };
            (we.drives ? pn.drivers : pn.receivers).push_back(pin);
            if (we.drives && toOutput) placeNodes[pin.node].drivesOutput = true;
        }
        if (!pn.drivers.empty() && !pn.receivers.empty()) placeNets.push_back(std::move(pn));
    }
    // Stability: roll the hints when what's placed changes (see
    // g_layoutHints), then hint every box placed before.
    {
        LayoutSignature sig;
        for (const auto& inst : g_schematic.instances) sig.nodes.push_back(SchematicInteraction::shapeKey(inst));
        for (const auto& pn : placeNets)
            for (const auto& d : pn.drivers)
                for (const auto& r : pn.receivers) sig.edges.push_back({ d.node, r.node });
        if (!(sig == g_layoutSignature)) {
            if (g_skipNextAutoFit) g_layoutHints = g_lastLayout;
            else                   g_layoutHints.clear();
            g_layoutSignature = std::move(sig);
        }
        for (size_t i = 0; i < placeNodes.size(); ++i) {
            auto it = g_layoutHints.find(g_layoutSignature.nodes[i]);
            if (it == g_layoutHints.end()) continue;
            placeNodes[i].hinted    = true;
            placeNodes[i].hintLevel = it->second.level;
            placeNodes[i].hintY     = it->second.y;
        }
    }
    const Placement placement = layeredPlacement(placeNodes, placeNets, viewOptions.sinkRows);
    g_lastLayout.clear();
    for (size_t i = 0; i < placeNodes.size(); ++i)
        g_lastLayout[g_layoutSignature.nodes[i]] = { placement.logicLevel[i], placement.pos[i].y };
    for (auto& inst : g_schematic.instances) {
        const ImVec2 pos = placement.pos[nodeOfShape[inst.id]];
        inst.x = pos.x;
        inst.y = pos.y;
    }

    // With Show Hierarchy, wrap the leaves in module frames (which re-lays
    // them out, keeping each one's logic level).
    bool grouped = false;
    // A net with a graph is wired segment by segment, through pins on the
    // frames' edges (SchematicLayout::splitNet). The pins are shared by every
    // net on them: one slot each, by (frame, term, bit).
    struct FramePinSlot {
        FramePin pin;
        int      outerId = 0, innerId = 0;  // its two faces' port ids
        float    labelW  = 0.f;
        float    y       = 0.f;
        bool     placed  = false;
    };
    std::vector<FramePinSlot> framePins;
    std::vector<std::optional<NetSegments>> segmentsByEi(equipotentials.size());
    std::vector<std::vector<size_t>>        slotOfPinByEi(equipotentials.size());  // a net's pin -> slot
    std::map<size_t, std::vector<std::pair<size_t, size_t>>> innerSegmentsOf;    // slot -> (net, segment)
    std::map<InstancePath, int> frameIdOf;  // the frames drawn, by module
    if (g_showHierarchy) {
        std::map<InstancePath, LeafHier> leafHier;
        for (const auto& [path, mi] : minsts)
            leafHier[path] = { mi.path, mi.pathModels, placement.level[nodeOfShape[pathToInstId[path]]] };
        std::set<InstancePath> frameSet = framePaths(leafHier);
        // Each drawn end's graph node, by its pin: (instance path, term,
        // bit), {} for a port. An end with none (a fanout summary) leaves
        // the net unsplittable: it's drawn whole.
        using PinKey = std::tuple<InstancePath, unsigned, std::optional<int>>;
        std::vector<std::vector<std::optional<size_t>>> endNodesByEi(equipotentials.size());
        std::vector<bool> mappedByEi(equipotentials.size(), false);
        for (size_t ei = 0; ei < equipotentials.size() && g_opened.empty(); ++ei) {
            const Equipotential* eq = equipotentials[ei];
            if (!eq || !eq->graph) continue;
            std::map<PinKey, size_t> nodeOfPin;
            for (size_t ni = 0; ni < eq->graph->nodes.size(); ++ni) {
                for (size_t o : eq->graph->nodes[ni].occurrences) {
                    const auto& oc = eq->occurrences[o];
                    nodeOfPin.emplace(PinKey{ oc.path, oc.term.child_id, oc.term.bit }, ni);
                }
                for (size_t t : eq->graph->nodes[ni].terms)
                    nodeOfPin.emplace(PinKey{ {}, eq->terms[t].child_id, eq->terms[t].bit }, ni);
            }
            bool mapped = true;
            for (const auto& we : equiEnds[ei]) {
                endNodesByEi[ei].emplace_back();
                auto req = g_portEquiByPortId.find(we.portId);
                if (req != g_portEquiByPortId.end()) {
                    auto node = nodeOfPin.find(PinKey{ we.isTerm ? InstancePath{} : we.path, req->second.termId,
                                                       req->second.bit });
                    if (node != nodeOfPin.end()) endNodesByEi[ei].back() = node->second;
                }
                mapped = mapped && endNodesByEi[ei].back().has_value();
            }
            mappedByEi[ei] = mapped;
        }
        // A module a net goes through with no leaf of it drawn (a
        // feed-through) gets a frame too, empty but for its pins: in a
        // column of its own right after the net's driver, at its height.
        // Only for a net split at frame pins: one drawn whole would leave
        // the frame empty and unwired (its leaves summarized, say).
        std::map<InstancePath, EmptyFrame> emptyFrames;
        for (size_t ei = 0; ei < equipotentials.size() && g_opened.empty(); ++ei) {
            const Equipotential* eq = equipotentials[ei];
            if (!eq || !eq->graph || !mappedByEi[ei]) continue;
            for (const auto& edge : eq->graph->edges) {
                if (!edge.port || frameSet.count(edge.path) || emptyFrames.count(edge.path)) continue;
                EmptyFrame e;
                e.pathModels = edge.pathModels;
                for (const auto& we : equiEnds[ei]) {
                    if (!we.drives) continue;
                    if (!we.isTerm) {
                        auto kit = pathToInstId.find(we.path);
                        if (kit != pathToInstId.end()) e.level = float(placement.level[nodeOfShape[kit->second]]) + 0.5f;
                        if (auto at = pinWorld(we)) e.y = at->y;
                    } else {
                        e.level = -0.5f;
                        for (const auto& r : equiEnds[ei])
                            if (auto at = r.isTerm ? std::nullopt : pinWorld(r)) { e.y = at->y; break; }
                    }
                    break;
                }
                emptyFrames[edge.path] = std::move(e);
            }
        }
        for (const auto& [path, e] : emptyFrames)
            for (size_t k = 1; k <= path.size(); ++k) frameSet.insert(InstancePath(path.begin(), path.begin() + long(k)));
        // (Opened instances' nets are rebuilt by openInstances(), without graphs.)
        std::map<std::tuple<InstancePath, unsigned, std::optional<int>>, size_t> slotOf;
        for (size_t ei = 0; ei < equipotentials.size() && !frameSet.empty() && g_opened.empty(); ++ei) {
            const Equipotential* eq = equipotentials[ei];
            if (!eq || !eq->graph || !mappedByEi[ei]) continue;
            std::vector<bool> drives;
            for (const auto& we : equiEnds[ei]) drives.push_back(we.drives);
            segmentsByEi[ei] = splitNet(*eq->graph, endNodesByEi[ei], drives, frameSet);
            if (!segmentsByEi[ei]) continue;
            for (const auto& pin : segmentsByEi[ei]->pins) {
                auto [it, added] = slotOf.emplace(std::make_tuple(pin.frame, pin.termId, pin.bit), framePins.size());
                if (added) {
                    FramePinSlot slot;
                    slot.pin     = pin;
                    slot.outerId = nextPortId++;
                    slot.innerId = nextPortId++;
                    slot.labelW  = netLabelWidth(pin.name);
                    framePins.push_back(std::move(slot));
                }
                slotOfPinByEi[ei].push_back(it->second);
            }
            // Where each pin's inner face is wired: to place it (below).
            for (size_t si = 0; si < segmentsByEi[ei]->segments.size(); ++si) {
                const NetSegment& seg = segmentsByEi[ei]->segments[si];
                auto note = [&](const SegmentEnd& se) {
                    if (se.pin >= 0 && se.inner) innerSegmentsOf[slotOfPinByEi[ei][size_t(se.pin)]].push_back({ ei, si });
                };
                note(seg.source);
                for (const auto& r : seg.receivers) note(r);
            }
        }
        // Room for the pins inside each frame: their labels, then a track
        // per pin for its inner wire; and their height.
        std::map<InstancePath, FrameRoom> room;
        {
            std::map<InstancePath, std::array<std::pair<float, int>, 2>> sides;  // per side: widest label, count
            for (const auto& slot : framePins) {
                auto& side = sides[slot.pin.frame][slot.pin.entering ? 0 : 1];
                side.first = std::max(side.first, slot.labelW);
                ++side.second;
            }
            for (const auto& [frame, s] : sides) {
                FrameRoom& r = room[frame];
                for (int k = 0; k < 2; ++k) {
                    if (s[size_t(k)].second == 0) continue;
                    const float w = s[size_t(k)].first + 2.f * kNetLabelGap + kTrackPitch * float(s[size_t(k)].second);
                    (k == 0 ? r.left : r.right) = w;
                    r.minH = std::max(r.minH, kGroupHeader + kPinPitch * float(s[size_t(k)].second) + kGroupPad);
                }
            }
        }
        auto frames = layoutHierarchyGroups(leafHier, g_schematic.instances, pathToInstId, nextInstId,
                                            instanceNameWidth, room, emptyFrames);
        grouped = !frames.empty();
        std::vector<InstanceShape> frameShapes;
        for (auto& f : frames) {
            f.shape.diagOutline = DiagnosisStore::instanceColor(f.shape.path);
            applyInstanceStyle(f.shape);
            f.shape.selected    = SelectionStore::isSelected(f.shape.path);
            // An opened instance's frame: its "-" closes it.
            f.shape.hasChildren  = g_opened.count(f.shape.path) > 0;
            f.shape.hierExpanded = f.shape.hasChildren;
            setReviewBadge(f.shape);
            g_occInfoByShapeId[f.shape.id] = { f.shape.path, DesignRef{}, true };
            frameShapes.push_back(std::move(f.shape));
        }
        // Parent-first order at the front: drawn under everything, and reverse
        // hit-test scans still find a leaf before the frame around it.
        g_schematic.instances.insert(g_schematic.instances.begin(),
                                     std::make_move_iterator(frameShapes.begin()),
                                     std::make_move_iterator(frameShapes.end()));

        // The frame pins, on their frames' edges: each level with what it
        // leads to inside (a straight inner wire), spaced kPinPitch apart,
        // inside the frame. Deepest frames first, so a pin can line up with
        // a pin of a frame inside it.
        auto endY = [&](size_t ei, const SegmentEnd& se) -> std::optional<float> {
            if (se.pin >= 0) {
                const FramePinSlot& slot = framePins[slotOfPinByEi[ei][size_t(se.pin)]];
                return slot.placed ? std::optional<float>(slot.y) : std::nullopt;
            }
            const WireEnd& we = equiEnds[ei][size_t(se.end)];
            if (we.isTerm) return std::nullopt;
            if (auto at = pinWorld(we)) return at->y;
            return std::nullopt;
        };
        // What a pin's inner face is wired to: the rest of its inner segment.
        auto innerWant = [&](size_t slotIndex) -> std::optional<float> {
            auto it = innerSegmentsOf.find(slotIndex);
            if (it == innerSegmentsOf.end()) return std::nullopt;
            for (const auto& [ei, si] : it->second) {
                const NetSegment& seg = segmentsByEi[ei]->segments[si];
                auto isThis = [&](const SegmentEnd& se) {
                    return se.pin >= 0 && se.inner && slotOfPinByEi[ei][size_t(se.pin)] == slotIndex;
                };
                // A source pin lines up with what it drives, a receiving one
                // with what drives it.
                std::vector<const SegmentEnd*> others;
                if (!isThis(seg.source)) others.push_back(&seg.source);
                for (const auto& r : seg.receivers) if (!isThis(r)) others.push_back(&r);
                for (const auto* o : others)
                    if (auto y = endY(ei, *o)) return y;
            }
            return std::nullopt;
        };
        std::map<InstancePath, InstanceShape*> frameOf;
        for (auto& s : g_schematic.instances)
            if (s.isHierGroup) {
                frameOf[s.path]   = &s;
                frameIdOf[s.path] = s.id;
            }
        std::vector<size_t> order(framePins.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            const InstancePath& fa = framePins[a].pin.frame;
            const InstancePath& fb = framePins[b].pin.frame;
            return fa.size() != fb.size() ? fa.size() > fb.size() : fa < fb;
        });
        for (size_t at = 0; at < order.size();) {
            const InstancePath frame = framePins[order[at]].pin.frame;
            size_t next = at;
            while (next < order.size() && framePins[order[next]].pin.frame == frame) ++next;
            auto fit = frameOf.find(frame);
            if (fit == frameOf.end()) { at = next; continue; }
            InstanceShape& shape = *fit->second;
            for (int side = 0; side < 2; ++side) {
                std::vector<std::pair<float, size_t>> want;
                for (size_t j = at; j < next; ++j)
                    if (framePins[order[j]].pin.entering == (side == 0))
                        want.push_back({ innerWant(order[j]).value_or(shape.y + kGroupHeader), order[j] });
                std::stable_sort(want.begin(), want.end(),
                                 [](const auto& a, const auto& b) { return a.first < b.first; });
                const float lo = shape.y + kGroupHeader, hi = shape.y + shape.h - kGroupPad;
                std::vector<float> ys(want.size());
                for (size_t k = 0; k < want.size(); ++k) {
                    float y = std::round(want[k].first / kGrid) * kGrid;
                    if (k > 0) y = std::max(y, ys[k - 1] + kPinPitch);
                    ys[k] = std::max(y, lo);
                }
                for (size_t k = want.size(); k-- > 0;) {
                    float y = std::min(ys[k], hi);
                    if (k + 1 < want.size()) y = std::min(y, ys[k + 1] - kPinPitch);
                    ys[k] = y;
                }
                for (size_t k = 0; k < want.size(); ++k) {
                    FramePinSlot& slot = framePins[want[k].second];
                    slot.y      = ys[k];
                    slot.placed = true;
                    for (int face = 0; face < 2; ++face) {
                        Port p;
                        p.id        = face == 0 ? slot.outerId : slot.innerId;
                        p.name      = slot.pin.name;
                        p.direction = slot.pin.direction;
                        p.isInput   = slot.pin.entering;
                        p.lx        = slot.pin.entering ? -0.5f : 0.5f;
                        p.ly        = shape.h > 0.f ? (slot.y - shape.y) / shape.h - 0.5f : 0.f;
                        p.color     = DiagnosisStore::netColor(slot.pin.frame, stripBusIndex(slot.pin.name));
                        p.framePin  = true;
                        p.innerFace = face == 1;
                        shape.ports.push_back(std::move(p));
                    }
                }
            }
            at = next;
        }
    }

    // Opened instances' port flags, now in place: drawn from the pin end
    // (the right end of an input's, the left of an output's), like a
    // top-level port's.
    for (auto& inst : g_schematic.instances) {
        if (inst.modelName != "port" || inst.ports.empty() || !g_boundaryPins.count(inst.path)) continue;
        Port& p = inst.ports[0];
        const ImVec2 at = portAnchor(inst, p);
        inst.x = at.x;
        inst.y = at.y;
        inst.w = inst.h = 0.f;
        p.lx = p.isInput ? 1.0f : -1.0f;
    }

    // Pass 3: top-level ports, on the sheet's edges -- inputs
    // left of everything, outputs right of it -- each level with the pin it
    // connects to (a straight wire) unless that would crowd another port.
    float sheetX0 = 1e30f, sheetX1 = -1e30f;
    for (const auto& inst : g_schematic.instances) {
        if (inst.parentShapeId >= 0 || inst.w <= 0.f) continue;
        sheetX0 = std::min(sheetX0, inst.x);
        sheetX1 = std::max(sheetX1, inst.x + footprintWidth(inst, instanceNameWidth));
    }
    if (sheetX0 > sheetX1) { sheetX0 = kLeftMargin; sheetX1 = kLeftMargin; }
    // ei: the equipotential the port is on, or kNoEqui for a port shown
    // with no net yet (showTopPorts()), drawn open.
    constexpr size_t kNoEqui = size_t(-1);
    // isBus: a bus's bits merged into one flag (see below); members: its
    // wired bits, (ei, bit port id); open: a bit still to load;
    // unconnected: no bit wired and every one found not connected.
    struct PortStub { size_t ei; std::string label; Direction direction; int portId; float wantY;
                      bool isBus = false; bool unconnected = false; bool open = false;
                      std::vector<std::pair<size_t, int>> members; };
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
    // A top-level bus is one flag ("d[7:0]") -- its wired bits and, with
    // showTopPorts(), its open ones -- until double-clicked, like a box's
    // bus pin: then one flag per bit, which a double-click on a bit (or
    // "Collapse Bus" in its menu) merges back. Every wired bit's wire
    // reaches the one flag, so a bus between two merged pins is one wire.
    using TermDir = std::pair<unsigned, Direction>;
    std::set<std::pair<unsigned, int>> shown;  // (term, bit) wired
    std::map<TermDir, std::set<int>> busBits;  // every bit known, wired or open
    std::map<TermDir, std::string>   busBase;
    for (const auto* stubs : { &leftStubs, &rightStubs })
        for (const auto& st : *stubs) {
            auto it = g_portEquiByPortId.find(st.portId);
            if (it == g_portEquiByPortId.end()) continue;
            shown.insert({ it->second.termId, it->second.bit.value_or(-1) });
            if (!it->second.bit) continue;
            busBits[{ it->second.termId, st.direction }].insert(*it->second.bit);
            busBase[{ it->second.termId, st.direction }] = stripBusIndex(st.label);
        }
    for (const auto& ep : g_startPorts) {
        if (!ep.bit) continue;
        busBits[{ ep.childId, ep.direction }].insert(*ep.bit);
        busBase[{ ep.childId, ep.direction }] = stripBusIndex(ep.name);
    }
    auto busKey = [&](const TermDir& td) {
        return BusGroupKey{ InstancePath{}, busBase[td], td.second == Direction::Input };
    };
    // A bus is one flag only when its wired bits all lead to the same pins
    // (a merged bus pin, say): their wires are then one bus wire. Bits going
    // to different pins would share one wire tree from the flag, drawn as
    // if their nets were joined: those get a flag per bit (in a bracketed
    // block), as if the bus were expanded.
    std::set<TermDir> splitBuses;
    {
        using Ends = std::set<std::pair<InstancePath, std::string>>;
        std::map<TermDir, std::optional<Ends>> endsOf;  // the first wired bit's
        for (const auto* stubs : { &leftStubs, &rightStubs })
            for (const auto& st : *stubs) {
                auto it = g_portEquiByPortId.find(st.portId);
                if (it == g_portEquiByPortId.end() || !it->second.bit) continue;
                Ends ends;
                for (const auto& we : equiEnds[st.ei]) {
                    if (we.isTerm && we.portId == st.portId) continue;
                    if (we.isTerm) ends.insert({ InstancePath{}, stripBusIndex(we.term) });
                    else           ends.insert({ we.path, std::to_string(renderedPort(we.portId)) });
                }
                const TermDir td{ it->second.termId, st.direction };
                auto& first = endsOf[td];
                if (!first) first = std::move(ends);
                else if (*first != ends) splitBuses.insert(td);
            }
    }
    auto isBusTerm  = [&](const TermDir& td) { return busBits[td].size() > 1; };
    auto mergedBus  = [&](const TermDir& td) {
        return isBusTerm(td) && !g_expandedBuses.count(busKey(td)) && !splitBuses.count(td);
    };
    auto busStub = [&](const TermDir& td, float wantY) {
        const std::set<int>& bits = busBits[td];
        PortStub bus{ kNoEqui, busLabel(busBase[td], std::vector<int>(bits.begin(), bits.end())),
                      td.second, nextPortId++, wantY, true, true };
        g_busGroupByPortId[bus.portId] = busKey(td);
        g_busTermByPortId[bus.portId]  = { {}, td.first, std::nullopt };
        return bus;
    };
    // Where each merged bus's flag is: its side's stubs, and its index.
    std::map<TermDir, std::pair<std::vector<PortStub>*, size_t>> busFlagOf;
    for (auto* stubs : { &leftStubs, &rightStubs }) {
        std::vector<PortStub> kept;
        for (auto& st : *stubs) {
            auto it = g_portEquiByPortId.find(st.portId);
            const TermDir td{ it != g_portEquiByPortId.end() ? it->second.termId : 0u, st.direction };
            if (it == g_portEquiByPortId.end() || !it->second.bit || !mergedBus(td)) {
                if (it != g_portEquiByPortId.end() && it->second.bit && isBusTerm(td))
                    g_expandedBusGroupByPortId[st.portId] = busKey(td);
                kept.push_back(std::move(st));
                continue;
            }
            // Where its first wired bit would have gone.
            auto [flag, fresh] = busFlagOf.emplace(td, std::make_pair(stubs, kept.size()));
            if (fresh) kept.push_back(busStub(td, st.wantY));
            PortStub& bus = kept[flag->second.second];
            bus.unconnected = false;
            bus.members.push_back({ st.ei, st.portId });
            logicalToRenderedPortId[st.portId] = bus.portId;
        }
        *stubs = std::move(kept);
    }
    // The top's ports no shown net reaches yet (showTopPorts()), open, after
    // the connected ones on their side, in the top's order; a merged bus's
    // open bits join its flag.
    if (!g_startPorts.empty()) {
        float below[2] = { -1e30f, -1e30f };
        for (int side = 0; side < 2; ++side)
            for (const auto& st : side == 0 ? leftStubs : rightStubs)
                below[side] = std::max(below[side], st.wantY);
        for (const auto& ep : g_startPorts) {
            if (shown.count({ ep.childId, ep.bit.value_or(-1) })) continue;
            const bool isInput = ep.direction == Direction::Input;
            auto& stubs = isInput ? leftStubs : rightStubs;
            float& y = below[isInput ? 0 : 1];
            const TermDir td{ ep.childId, ep.direction };
            if (ep.bit && mergedBus(td)) {
                auto flag = busFlagOf.find(td);
                if (flag == busFlagOf.end()) {
                    y = y < -1e29f ? 0.f : y + kPinPitch;
                    flag = busFlagOf.emplace(td, std::make_pair(&stubs, stubs.size())).first;
                    stubs.push_back(busStub(td, y));
                }
                PortStub& bus = (*flag->second.first)[flag->second.second];
                const PortEquiRequest r{ {}, ep.childId, ep.bit };
                if (!g_unconnectedPins.count(pinRequestKey(r))) {
                    bus.open        = true;
                    bus.unconnected = false;
                    g_busOpenBitsByPortId[bus.portId].push_back(r);
                }
                continue;
            }
            const int pid = nextPortId++;
            g_portEquiByPortId[pid] = { {}, ep.childId, ep.bit };
            if (ep.bit && isBusTerm(td)) g_expandedBusGroupByPortId[pid] = busKey(td);
            y = y < -1e29f ? 0.f : y + kPinPitch;
            stubs.push_back({ kNoEqui, ep.name, ep.direction, pid, y });
        }
    }
    // Input ports driving a labeled net: the label goes right past the flag,
    // so every input port moves left to make room for the widest, and its
    // wire reaches the routing channel `leftPortRoom` later (see the routing
    // below): the label is never under a wire's trunk or a box.
    float leftPortRoom = 0.f;
    for (const auto& st : leftStubs)
        if (st.ei != kNoEqui && !styleLabelByEi[st.ei].empty())
            leftPortRoom = std::max(leftPortRoom, 2.f * kNetLabelGap + netLabelWidth(styleLabelByEi[st.ei]));
    for (auto* stubs : { &leftStubs, &rightStubs }) {
        std::vector<float> want;
        for (const auto& st : *stubs) want.push_back(st.wantY);
        // An expanded bus's bit flags stay together, under one bracket, in
        // the order of what they lead to (so their wires don't cross).
        std::map<BusGroupKey, std::vector<size_t>> busFlags;
        for (size_t i = 0; i < stubs->size(); ++i)
            if (auto it = g_expandedBusGroupByPortId.find((*stubs)[i].portId); it != g_expandedBusGroupByPortId.end())
                busFlags[it->second].push_back(i);
        std::vector<std::vector<size_t>> blocks;
        for (auto& [key, members] : busFlags) {
            if (members.size() < 2) continue;
            std::stable_sort(members.begin(), members.end(),
                             [&](size_t a, size_t b) { return want[a] < want[b]; });
            blocks.push_back(members);
        }
        const std::vector<float> ys = stackPorts(want, blocks);
        const bool isInput = stubs == &leftStubs;
        std::map<size_t, int> flagIdOfStub;
        for (size_t i = 0; i < stubs->size(); ++i) {
            const PortStub& st = (*stubs)[i];
            InstanceShape inst;
            inst.id = nextInstId++;
            flagIdOfStub[i] = inst.id;
            inst.x  = isInput ? sheetX0 - kPortGap - leftPortRoom : sheetX1 + kPortGap;
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
            // A merged bus flag's label may be "b (*3)": its base is before the space.
            p.color     = DiagnosisStore::netColor({}, stripBusIndex(st.isBus ? st.label.substr(0, st.label.find(' '))
                                                                               : st.label));
            if (st.isBus) {
                p.open        = st.open;
                p.unconnected = st.unconnected;
                p.isBus       = true;
            } else if (st.ei == kNoEqui) {
                p.open    = true;
                p.pending = g_pendingPinNets.isPending(pinRequestKey(g_portEquiByPortId[p.id]),
                                                       ImGui::GetTime());
            } else if (isInput && !styleLabelByEi[st.ei].empty()) {
                p.netLabel  = styleLabelByEi[st.ei];
                p.netLabelW = netLabelWidth(p.netLabel);
            }
            inst.ports.push_back(p);
            if (st.ei != kNoEqui)
                for (auto& we : equiEnds[st.ei])
                    if (we.isTerm && we.portId == st.portId) we.termInstId = inst.id;
            for (const auto& [ei, pid] : st.members)
                for (auto& we : equiEnds[ei])
                    if (we.isTerm && we.portId == pid) we.termInstId = inst.id;
            g_schematic.instances.push_back(std::move(inst));
        }
        for (const auto& block : blocks) {
            SchematicView::PortBracket b;
            b.isInput = isInput;
            const BusGroupKey& key = g_expandedBusGroupByPortId[(*stubs)[block[0]].portId];
            std::vector<int> bits;
            for (size_t i : block) {
                b.flagIds.push_back(flagIdOfStub[i]);
                if (auto r = g_portEquiByPortId.find((*stubs)[i].portId);
                    r != g_portEquiByPortId.end() && r->second.bit)
                    bits.push_back(*r->second.bit);
            }
            b.label = busLabel(std::get<1>(key), bits);
            g_schematic.portBrackets.push_back(std::move(b));
        }
    }

    // Review badges on pins: the open threads on each pin's term (its bit,
    // or the whole term; a merged bus pin, every bit of the bus).
    for (const auto& [logical, rendered] : logicalToRenderedPortId) {
        auto it = g_portEquiByPortId.find(logical);
        if (logical == rendered || !g_busGroupByPortId.count(rendered) ||
            it == g_portEquiByPortId.end()) continue;
        g_busTermByPortId[rendered] = { it->second.pathIds, it->second.termId, std::nullopt };
    }
    for (auto& inst : g_schematic.instances) {
        for (auto& p : inst.ports) {
            const auto threads = pinReviewThreads(p.id);
            p.reviewOpen  = int(ReviewStore::openCount(threads));
            p.reviewColor = ReviewStore::openColor(threads);
            applyPinStyle(inst, p);
            // A bus pin is loading while every bit it asked for is.
            if (auto bits = g_busOpenBitsByPortId.find(p.id); bits != g_busOpenBitsByPortId.end()) {
                p.pending = p.open && !bits->second.empty();
                for (const auto& b : bits->second)
                    p.pending = p.pending && g_pendingPinNets.isPending(pinRequestKey(b), ImGui::GetTime());
            }
            auto req = g_portEquiByPortId.find(p.id);
            if (req != g_portEquiByPortId.end() && g_unconnectedPins.count(pinRequestKey(req->second))) {
                p.unconnected = true;
                p.open = p.pending = false;  // nothing more to load
            }
        }
    }

    // Pass 4: wires per equip
    // A segment's wire tree, by its source pin: the design it's in, to keep
    // it out of the frames it isn't inside (see the routing below).
    std::map<std::pair<int, int>, InstancePath> segmentScopeOf;
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
        const StyleAttrs netStyle = equipotentials[ei] ? StyleStore::netStyle(*equipotentials[ei])
                                                       : StyleAttrs{};

        // Wired from its driver to every other end -- or, split into
        // segments, from each segment's source to its receivers, through
        // the frame pins' faces.
        struct WirePair { WR src, dst; std::string netName; };
        std::vector<WirePair> pairs;
        if (segmentsByEi[ei]) {
            auto resolve = [&](const SegmentEnd& se) -> std::optional<WR> {
                if (se.pin >= 0) {
                    const FramePinSlot& slot = framePins[slotOfPinByEi[ei][size_t(se.pin)]];
                    auto f = frameIdOf.find(slot.pin.frame);
                    if (f == frameIdOf.end() || !slot.placed) return std::nullopt;
                    return WR{ f->second, se.inner ? slot.innerId : slot.outerId };
                }
                const WireEnd& we = ends[size_t(se.end)];
                int instId = we.termInstId;
                if (!we.isTerm) {
                    auto it = pathToInstId.find(we.path);
                    instId = it != pathToInstId.end() ? it->second : -1;
                }
                if (instId < 0) return std::nullopt;
                return WR{ instId, renderedPort(we.portId) };
            };
            for (const auto& seg : segmentsByEi[ei]->segments) {
                auto src = resolve(seg.source);
                if (!src) continue;
                segmentScopeOf[{ src->instId, src->portId }] = seg.scope;
                for (const auto& r : seg.receivers)
                    if (auto dst = resolve(r))
                        pairs.push_back({ *src, *dst, seg.net.empty() ? netNameByEi[ei] : seg.net });
            }
        } else {
            for (size_t i = 1; i < wrs.size(); ++i) pairs.push_back({ wrs[0], wrs[i], netNameByEi[ei] });
        }

        for (const auto& pair : pairs) {
            NetWire n;
            n.id          = int(g_schematic.nets.size()) + 1;
            n.srcInstance = pair.src.instId;
            n.srcPortId   = pair.src.portId;
            n.dstInstance = pair.dst.instId;
            n.dstPortId   = pair.dst.portId;
            n.netName     = pair.netName;
            n.traceIds    = traceIdsByEi[ei];
            n.faint       = faintByEi[ei];
            n.styleColor  = netStyle.color;
            n.width       = netStyle.width;
            n.label       = netStyle.label;

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
                continue;
            }
            NetWire& m = merged[it->second];
            // The same wire from two nets (e.g. a trace's partial net and the
            // full one): it's on every trace either is on.
            mergeTraceIds(m.traceIds, n.traceIds);
            m.faint = m.faint && n.faint;
            if (m.styleColor == 0) m.styleColor = n.styleColor;
            m.width = std::max(m.width, n.width);
            if (m.label.empty()) m.label = n.label;
            if (portIsBus(n.srcInstance, n.srcPortId) || portIsBus(n.dstInstance, n.dstPortId)) {
                m.isBus = true;
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
        std::vector<RouteRegion>  regions;      // the module frames
        std::vector<InstancePath> regionPaths;  // and their paths
        for (const auto& inst : g_schematic.instances) {
            if (inst.parentShapeId >= 0) continue;
            // Net labels: in the box's span (so the trunk goes after them);
            // an input port's sit in the stretch of wire before its channel.
            for (const auto& l : netLabelPlaces(inst, instanceNameWidth))
                (inst.modelName == "port" ? keepOut : obstacles).push_back(l.rect);
            // An opened instance's port flag is inside its frame, among
            // the contents: routed around.
            if (inst.modelName == "port" && !inst.path.empty() && !inst.ports.empty()) {
                const Port& p = inst.ports[0];
                const float fw = boundaryFlagWidth(p.name), fh = 0.5f * boundaryFlagHeight();
                obstacles.push_back(p.isInput ? RouteRect{ inst.x - fw, inst.y - fh, inst.x, inst.y + fh }
                                              : RouteRect{ inst.x, inst.y - fh, inst.x + fw, inst.y + fh });
                continue;
            }
            if (inst.w <= 0.f || inst.h <= 0.f) continue;
            if (inst.isHierGroup) {  // its label row
                keepOut.push_back({ inst.x, inst.y, inst.x + inst.w, inst.y + kGroupHeader - kPinPitch });
                // Its pins' names, inside it above their wires: the inner
                // wire's trunk goes past them.
                bool walled = false;
                for (const auto& p : inst.ports) {
                    if (!p.framePin || p.innerFace) continue;
                    walled = true;
                    obstacles.push_back(framePinLabelRect(inst, p));
                }
                regions.push_back({ { inst.x, inst.y, inst.x + inst.w, inst.y + inst.h }, walled });
                regionPaths.push_back(inst.path);
                continue;
            }
            obstacles.push_back({ inst.x, inst.y, inst.x + inst.w, inst.y + inst.h });
            if (auto name = nameRect(inst, instanceNameWidth)) obstacles.push_back(*name);
            if (auto model = modelRect(inst, instanceNameWidth)) obstacles.push_back(*model);
        }
        std::map<std::pair<int, int>, size_t> routeOfSource;
        std::vector<RouteNet> routeNetsIn;
        struct RouteTraces { std::vector<int> ids; bool faint = true; ImU32 styleColor = 0; };
        std::vector<RouteTraces> routeTraces;
        g_schematic.routes.clear();
        // An input port's wire is routed from `leftPortRoom` past its flag
        // (where its channel starts), joined to the flag after routing.
        auto isLeftPort = [](const InstanceShape& inst, const Port& p) {
            return inst.modelName == "port" && p.lx >= 0.f && inst.path.empty();
        };
        // (A frame pin's inner face leaves into the frame: the other way.)
        auto pinOf = [&](const InstanceShape& inst, const Port& p) {
            ImVec2 at = portAnchor(inst, p);
            if (isLeftPort(inst, p)) at.x += leftPortRoom;
            return RoutePin{ at, (p.lx >= 0.f) != p.innerFace };
        };
        std::vector<std::vector<RouteSegment>> portLeads;  // per route
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
                // A segment stays out of the frames it isn't inside.
                if (auto scope = segmentScopeOf.find(key); scope != segmentScopeOf.end()) {
                    routeNetsIn.back().confined = true;
                    for (size_t gi = 0; gi < regionPaths.size(); ++gi) {
                        const InstancePath& f = regionPaths[gi];
                        if (f.size() <= scope->second.size() &&
                            std::equal(f.begin(), f.end(), scope->second.begin()))
                            routeNetsIn.back().inside.push_back(int(gi));
                    }
                }
                SchematicView::NetRoute r;
                r.color   = n.color;
                r.netName = n.netName;
                r.ref     = srcInst->modelName == "port"
                    ? SchematicNetRef{ srcInst->path, srcPort->name, true }
                    : SchematicNetRef{ srcInst->path, srcPort->name, false };
                r.selected = SelectionStore::isNetSelected(r.ref);
                for (const auto& l : netLabelPlaces(*srcInst, instanceNameWidth))
                    if (l.portId == srcPort->id) {
                        r.hasLabel  = true;
                        r.labelAt   = l.at;
                        r.labelText = srcPort->netLabel;
                    }
                g_schematic.routes.push_back(std::move(r));
                routeTraces.emplace_back();
                portLeads.emplace_back();
                if (leftPortRoom > 0.f && isLeftPort(*srcInst, *srcPort))
                    portLeads.back().push_back({ portAnchor(*srcInst, *srcPort), pinOf(*srcInst, *srcPort).at });
            }
            if (leftPortRoom > 0.f && isLeftPort(*dstInst, *dstPort))
                portLeads[it->second].push_back({ pinOf(*dstInst, *dstPort).at, portAnchor(*dstInst, *dstPort) });
            mergeTraceIds(routeTraces[it->second].ids, n.traceIds);
            routeTraces[it->second].faint = routeTraces[it->second].faint && n.faint;
            routeNetsIn[it->second].receivers.push_back(pinOf(*dstInst, *dstPort));
            auto& r = g_schematic.routes[it->second];
            // A flagged wire colors the whole tree it belongs to.
            if (n.color != NetWire{}.color) r.color = n.color;
            // So does a styled one (StyleStore), under a diagnosis color.
            if (routeTraces[it->second].styleColor == 0) routeTraces[it->second].styleColor = n.styleColor;
            r.width = std::max(r.width, n.width);
            if (r.label.empty()) r.label = n.label;
            r.isBus = r.isBus || n.isBus;
            n.routed = true;
        }
        for (size_t i = 0; i < g_schematic.routes.size(); ++i) {
            auto& r = g_schematic.routes[i];
            r.color = styledWireColor(r.color, routeTraces[i].styleColor, routeTraces[i].ids,
                                      routeTraces[i].faint);
            // A shared tree shows which traces share it (no diagnosis color).
            if (r.color == TraceStore::convergenceColor()) {
                std::vector<int> ids = routeTraces[i].ids;
                std::sort(ids.begin(), ids.end());  // same stripe order every frame
                for (int id : ids) r.stripeColors.push_back(TraceStore::color(id));
            }
        }
        // Wiring nested in an expanded instance isn't routed: color it here.
        for (auto& n : g_schematic.nets)
            if (!n.routed) n.color = styledWireColor(n.color, n.styleColor, n.traceIds, n.faint);
        // The export legend: the colors actually drawn, one entry per wire
        // tree in drawing order (no duplicates).
        // A styled label names its wire in the color drawn (the style's,
        // under a diagnosis); otherwise the trace drawn on it does: the one
        // whose color the wire has, or each striped trace. A wire with no
        // label (a plain or diagnosed net, a faint one) has no entry.
        {
            auto& legend = g_schematic.legend;
            legend.clear();
            auto add = [&](ImU32 color, const std::string& label) {
                if (label.empty()) return;
                for (const auto& e : legend)
                    if (e.label == label && e.color == color) return;
                legend.push_back({ color, label });
            };
            auto addWire = [&](ImU32 color, const std::string& styleLabel, const std::vector<int>& ids,
                               bool faint, const std::vector<ImU32>& stripes) {
                if (faint) return;
                if (!styleLabel.empty()) { add(color, styleLabel); return; }
                for (int id : ids) {
                    const Trace* t = TraceStore::find(id);
                    if (!t || !TraceStore::isVisible(id)) continue;
                    if (!stripes.empty() || TraceStore::color(id) == color) add(TraceStore::color(id), t->label);
                }
            };
            for (size_t i = 0; i < g_schematic.routes.size(); ++i) {
                const auto& r = g_schematic.routes[i];
                addWire(r.color, r.label, routeTraces[i].ids, routeTraces[i].faint, r.stripeColors);
            }
            for (const auto& n : g_schematic.nets)
                if (!n.routed) addWire(n.color, n.label, n.traceIds, n.faint, {});
        }
        auto routed = routeNets(routeNetsIn, obstacles, keepOut, regions);
        for (size_t i = 0; i < routed.size(); ++i) {
            g_schematic.routes[i].segments  = std::move(routed[i].segments);
            g_schematic.routes[i].segments.insert(g_schematic.routes[i].segments.begin(),
                                                  portLeads[i].begin(), portLeads[i].end());
            g_schematic.routes[i].junctions = std::move(routed[i].junctions);
        }
    }

    static int lastTotal = -1;
    static std::string lastViewOptions;
    if (const std::string vo = viewOptions.layoutJson().dump(); vo != lastViewOptions) {
        if (!lastViewOptions.empty()) g_schematic.requestFit(true);
        lastViewOptions = vo;
    }
    if (totalItems != lastTotal) {
        if (g_skipNextAutoFit) {
            g_skipNextAutoFit = false;
            // Pan so the clicked pin stays under the cursor, wherever the
            // re-layout moved its box (screen = (world - offset) * scale, so
            // shifting offset by the world delta cancels it out). A pin no
            // longer drawn (e.g. merged into a bus) keeps its box in place.
            for (const auto& inst : g_schematic.instances) {
                if (!isKeptInPlace(inst)) continue;
                ImVec2 was = g_keepInPlaceBoxWorld, now(inst.x, inst.y);
                for (const auto& p : inst.ports)
                    if (p.name == g_keepInPlacePin) { was = g_keepInPlaceWorld; now = portAnchor(inst, p); break; }
                g_schematic.transform.offset.x += now.x - was.x;
                g_schematic.transform.offset.y += now.y - was.y;
                break;
            }
            g_keepInPlacePath.clear();
            g_keepInPlacePort.clear();
            g_keepInPlacePin.clear();
        } else if (!(g_focus && g_focus->requested)) {
            // (What a focusObject() request adds is panned to instead.)
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
    if (g_cameraRect) g_schematic.requestFitRect(g_cameraRect->first, g_cameraRect->second);
    g_schematic.updateFitIfNeeded(cpos, inner, 60.f);
    advanceFocus(inner);

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
    // A review badge first: it sits over a pin's or box's corner.
    bool badgeHovered = false;
    if (canvasHovered) {
        if (auto badge = pickReviewBadge(ImGui::GetMousePos())) {
            const auto threads = badgeThreads(*badge);
            if (!threads.empty()) {
                badgeHovered = true;
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                ImGui::BeginTooltip();
                ReviewView::threadsTooltip(threads);
                ImGui::TextDisabled("Click: open in the Review tab");
                ImGui::EndTooltip();
            }
        }
    }
    // A pin focusObject() just brought into view stands out for a moment.
    if (g_focusPin && ImGui::GetTime() < g_focusPinUntil) {
        if (auto pin = findFocusPin(*g_focusPin)) g_schematic.hoveredPortId = pin->second->id;
    } else {
        g_focusPin.reset();
    }
    bool pinHovered = badgeHovered;
    if (canvasHovered && !badgeHovered) {
        ImVec2 wp = mouseWorldPos(g_schematic, cpos);
        auto hit = SchematicInteraction::pickPin(g_schematic.instances, wp, g_schematic.transform.scale);
        InstanceShape* inst = hit ? g_schematic.findInstanceById(hit->instanceId) : nullptr;
        Port* port = inst ? g_schematic.findPortById(*inst, hit->portId) : nullptr;
        if (port) {
            pinHovered = true;
            g_schematic.hoveredPortId = port->id;
            const PinAction action = pinAction(*port);
            if (action == PinAction::LoadBus || action == PinAction::LoadNet)
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ImGui::BeginTooltip();
            if (inst->modelName == "port" && inst->path.empty())
                ImGui::Text("%s  (top-level port)", port->name.c_str());
            else if (inst->modelName == "port")
                ImGui::Text("%s  (pin of %s)", port->name.c_str(), displayPath(pinOwnerPath(*inst)).c_str());
            else                           ImGui::Text("%s / %s", inst->name.c_str(), port->name.c_str());
            switch (action) {
                case PinAction::LoadBus:   ImGui::TextDisabled("Click: add the bus's nets to the view"); break;
                case PinAction::LoadNet:   ImGui::TextDisabled("Click: add this net to the view"); break;
                case PinAction::Loading:   ImGui::TextDisabled("Loading net..."); break;
                case PinAction::None:
                    ImGui::TextDisabled(port->unconnected ? "Not connected"
                                        : port->isBus     ? "Nets shown" : "Net shown");
                    break;
            }
            switch (busToggle(port->id)) {
                case +1: ImGui::TextDisabled("Double-click: show the individual bits"); break;
                case -1: ImGui::TextDisabled("Double-click: merge the bits back into one bus pin"); break;
                default: break;
            }
            if (g_portEquiByPortId.count(port->id))
                ImGui::TextDisabled("Right-click: Trace to Driver (add to view)");
            ImGui::EndTooltip();
        }
    }
    bool wireHovered = false;
    if (canvasHovered && !pinHovered) {
        ImVec2 wp = mouseWorldPos(g_schematic, cpos);
        if (auto wire = pickRoute(wp)) {
            wireHovered = true;
            const auto& r = g_schematic.routes[*wire];
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ImGui::BeginTooltip();
            if (r.ref.port) ImGui::Text("Net of %s  (top-level port)", r.ref.pin.c_str());
            else            ImGui::Text("Net of %s / %s", displayPath(r.ref.path).c_str(), r.ref.pin.c_str());
            if (!r.selected) ImGui::TextDisabled("Click: select this net");
            ImGui::EndTooltip();
        }
    }
    if (canvasHovered && !pinHovered && !wireHovered) {
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
            if (inst.isSummary) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                ImGui::BeginTooltip();
                ImGui::Text("%d sinks of this net, gathered: %s", inst.summaryCount, inst.name.c_str());
                ImGui::TextDisabled("Click: show them");
                ImGui::EndTooltip();
                break;
            }
            auto it = g_occInfoByShapeId.find(inst.id);
            if (it == g_occInfoByShapeId.end()) break;
            overBox = true;
            if (inst.id != g_hintShapeId) { g_hintShapeId = inst.id; g_hintSince = ImGui::GetTime(); }
            const bool showHint = inst.partialInterface && ImGui::GetTime() - g_hintSince > 0.6;
            auto diagnostics = DiagnosisStore::instanceDiagnostics(it->second.path);
            const auto& traceIds = traceIdsByInstance[it->second.path];
            if (!diagnostics.empty() || showHint || traceIds.size() >= 2) {
                ImGui::BeginTooltip();
                for (const auto* d : diagnostics) {
                    ImGui::TextColored(ImColor(DiagnosisStore::colorForSeverity(d->severity)).Value,
                                       "[%s] %s", toString(d->severity), d->message.c_str());
                    if (!d->source.empty()) ImGui::TextDisabled("source: %s", d->source.c_str());
                }
                if (traceIds.size() >= 2) {
                    ImGui::TextUnformatted("On traces:");
                    for (int id : traceIds)
                        if (const Trace* t = TraceStore::find(id))
                            ImGui::TextColored(ImColor(TraceStore::color(id)).Value, "  %s", t->label.c_str());
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

    g_schematic.colors = theme().schematic;
    g_schematic.render(dl, cpos, inner);
    if (equipotentials.empty() && !hasStartShapes()) {
        // Nothing drawn yet: say how to fill it, which a first-time user
        // has no way to guess.
        const char* hint = "Right-click the top design in the Netlist Hierarchy and choose\n"
                           "Show Contents or Show Ports, or right-click a pin there\n"
                           "and choose Show Equipotential or Trace to Driver.\n"
                           "Click open pins on the schematic to add more.";
        ImVec2 ts = ImGui::CalcTextSize(hint);
        dl->AddText(ImVec2(cpos.x + (inner.x - ts.x) * 0.5f, cpos.y + (inner.y - ts.y) * 0.5f),
                    theme().schematic.hint, hint);
    }
    ImGui::EndChild();

    // Scrollbars
    if (g_cameraRect) return;
    ImVec2 wMin(-500.f, -500.f), wMax(500.f, 500.f);
    if (g_schematic.computeWorldBounds(wMin, wMax)) {
        wMin.x -= 120.f; wMin.y -= 120.f; wMax.x += 120.f; wMax.y += 120.f;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, 24.f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg,          theme().scrollFrame);
    ImGui::PushStyleColor(ImGuiCol_SliderGrab,       theme().scrollGrab);
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, theme().scrollGrabActive);
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

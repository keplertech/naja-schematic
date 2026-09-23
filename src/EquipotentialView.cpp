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

// ---------------------------------------------------------------------------
// Layout geometry constants
// ---------------------------------------------------------------------------
static constexpr float kInstW       = 180.0f;
static constexpr float kInstH       = 70.0f;
static constexpr float kColGap      = 120.0f;
static constexpr float kRowSpacing  = 24.0f;
static constexpr float kPortSpacing = 18.0f;
static constexpr float kLeftMargin  = 20.0f;
static constexpr float kNetVGap     = 80.0f;

// Pin click/hit-test radius, in screen pixels (converted to world units by
// dividing by the current zoom scale where it's used). A pin now renders as
// a short tick line (see SchematicView.cpp's drawPorts) reaching a few
// pixels out from the port's anchor point rather than a filled dot sitting
// right on it, so the hit area needs to comfortably cover the whole tick
// (and the start of its label) -- not just the anchor -- or double-click/
// right-click on a pin becomes very hard to land.
static constexpr float kPortHitRadiusPx = 18.0f;

// Hierarchy embedding (nested boxes) geometry.
static constexpr float kHierChildW    = 110.0f;
static constexpr float kHierChildH    = 44.0f;
static constexpr float kHierGap       = 14.0f;
static constexpr float kHierMargin    = 16.0f;
static constexpr float kHierHeaderGap = 26.0f; // room below the box's own label/ports

// Hierarchy grouping (module frames around traced leaves) geometry.
static constexpr float kGroupHeader = 30.0f; // room for the frame's label
static constexpr float kGroupPad    = 18.0f; // inner padding of a frame
static constexpr float kGroupColGap = 90.0f; // gap between columns inside a frame

// ---------------------------------------------------------------------------
// Internal item type
// ---------------------------------------------------------------------------
struct Item {
    std::string            label;       // port/term name (for matching, port rendering)
    std::string            fullName;    // slash-path for instances; term name for terms
    Direction              direction   = Direction::Inout;
    bool                   isTerm      = false;
    DesignRef              designRef{};
    unsigned               termChildId = 0;
    std::optional<int>     termBit;
    std::vector<unsigned>  pathIds;
    // Only meaningful for instance occurrences (isTerm == false): whether
    // this instance's model has sub-instances worth expanding into a nested
    // schematic box.
    bool                   hasInstances = false;
    // RTL source location of the instance itself, if available.
    std::optional<SourceLoc> sourceLoc;
    // Instance occurrences only: per-segment instance names / model names of
    // the full hierarchical path (the last entry is the instance itself).
    std::vector<std::string> path;
    std::vector<std::string> pathModels;

    const std::string& key() const { return fullName.empty() ? label : fullName; }
};

// ---------------------------------------------------------------------------
// Static view state
// ---------------------------------------------------------------------------
static SchematicView      g_schematic;
static int                g_pendingZoomSteps = 0;
static bool               g_pendingFit       = false;
static bool               g_pendingClear     = false;
static INetlistProvider*  g_provider         = nullptr;
// Which instance box (if any) the canvas right-click popup currently
// targets; -1 = the canvas-level menu (Clear all nets / Fit view).
static int                g_ctxInstanceId    = -1;
// Which pin (if any) the popup targets; takes precedence over the instance
// box the pin sits on. -1 = none.
static int                g_ctxPortId        = -1;

struct OccurrenceInfo { std::string pathKey; DesignRef designRef; std::optional<SourceLoc> sourceLoc; };
struct PortEquiRequest {
    std::vector<unsigned> pathIds;
    unsigned              termId = 0;
    std::optional<int>    bit;
};
static std::map<int, OccurrenceInfo>  g_occInfoByShapeId;
static std::map<int, PortEquiRequest> g_portEquiByPortId;
// Merged bus-pin port id (or the topmost bit's port id when a bus is shown
// expanded) -> the bus-group key it toggles. Checked before
// g_portEquiByPortId on double-click. Rebuilt every frame.
static std::map<int, std::string> g_busGroupByPortId;

static std::map<std::string, std::vector<EquipotentialView::ExpandedPort>> g_expandedInstances;
static std::set<std::string> g_pendingExpansions;
// Bus-group keys ("instanceKey<US>busBase<US>I|O") currently shown expanded
// (individual bit pins/wires) instead of merged into one pin/wire.
static std::set<std::string> g_expandedBuses;

// --- Hierarchy embedding (nested boxes) ---
// pathKeys (== InstanceShape::name) currently toggled open to show their
// internal sub-instances nested inside their box.
static std::set<std::string> g_hierExpanded;
// pathKeys with an in-flight load_instance_internals request.
static std::set<std::string> g_hierPending;
// pathKeys whose internals have been loaded (children + internal nets).
static std::map<std::string, EquipotentialView::InstanceInternals> g_instanceInternals;

// Hierarchy grouping: when on, the leaf instances shown are laid out inside
// nested frames standing for the hierarchical modules that contain them
// (see layoutHierarchyGroups) instead of the flat column layout.
static bool g_showHierarchy = true;
// A "Zoom to Module" request from the canvas context menu: the pathKey of the
// frame to fit the view to once this frame's layout is known.
static std::string g_pendingZoomGroup;

// Persistent layout state
static std::map<std::string, ImVec2>  g_placedPositions;  // key → world top-left
static std::set<const Equipotential*> g_laidOut;
static float                          g_layoutNextY = 0.f;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Extract the leaf segment from a slash-separated instance path.
// e.g. "top/sub/<assign:0>" → "<assign:0>"
static std::string leafSegment(const std::string& path) {
    auto pos = path.rfind('/');
    return (pos == std::string::npos) ? path : path.substr(pos + 1);
}

// Strip a trailing bus-bit suffix ("Q[3]" -> "Q") so a pin label can be
// matched against DiagnosisItem::terminal, which is always the base name.
static std::string stripBusIndex(const std::string& label) {
    auto pos = label.rfind('[');
    return pos == std::string::npos ? label : label.substr(0, pos);
}

// Build a bus-group key: unique per (instance, bus base name, direction).
static std::string busGroupKey(const std::string& instKey, const std::string& base, bool isInput) {
    return instKey + "\x1f" + base + "\x1f" + (isInput ? "I" : "O");
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

// Derive the gate/cell model name from the leaf instance name.
// Naja SNL encodes assign statements as "<assign:N>".
// Additional primitives can be detected here as the library grows.
static std::string modelNameFromLeaf(const std::string& leaf) {
    if (leaf.find("assign") != std::string::npos) return "assign";
    // Add more patterns here:
    //   if (leaf.find("DFF") != std::string::npos) return "dff";
    //   if (leaf == "AND2")                        return "and2";
    return "";  // generic box
}

static void buildItems(const Equipotential* eq,
                       std::vector<Item>& drivers,
                       std::vector<Item>& receivers) {
    for (const auto& bt : eq->terms) {
        Item item;
        item.label       = bt.getString();
        item.fullName    = bt.name;
        item.direction   = bt.direction;
        item.isTerm      = true;
        item.termChildId = bt.child_id;
        item.termBit     = bt.bit;
        (bt.direction == Direction::Input ? drivers : receivers).push_back(std::move(item));
    }
    for (const auto& occ : eq->occurrences) {
        Item item;
        item.label       = occ.term.getString();
        item.isTerm      = false;
        item.designRef   = occ.designRef;
        item.termChildId = occ.term.child_id;
        item.termBit     = occ.term.bit;
        item.pathIds     = occ.pathIds;
        item.hasInstances = occ.has_instances;
        item.sourceLoc    = occ.source_loc;
        item.path         = occ.path;
        item.pathModels   = occ.pathModels;
        std::string joined;
        bool first = true;
        for (const auto& seg : occ.path) {
            if (!first) joined += '/';
            joined += seg;
            first = false;
        }
        item.fullName  = std::move(joined);
        item.direction = occ.term.direction;
        (occ.term.direction == Direction::Output ? drivers : receivers).push_back(std::move(item));
    }
}

static float portLy(int i, int n) {
    return n > 1 ? -0.4f + 0.8f * float(i) / float(n - 1) : 0.0f;
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
// Layout: called once per new equip, stores positions in g_placedPositions
// ---------------------------------------------------------------------------
static void layoutEquipotential(const Equipotential* eq) {
    if (g_laidOut.count(eq)) return;
    g_laidOut.insert(eq);

    std::vector<Item> drivers, receivers;
    buildItems(eq, drivers, receivers);
    if (drivers.empty() && receivers.empty()) return;

    // Find anchor: first non-term already placed
    const Item* anchor      = nullptr;
    bool        anchorDrives = false;

    for (const auto& item : drivers) {
        if (!item.isTerm && g_placedPositions.count(item.key()))
            { anchor = &item; anchorDrives = true; break; }
    }
    if (!anchor) {
        for (const auto& item : receivers) {
            if (!item.isTerm && g_placedPositions.count(item.key()))
                { anchor = &item; anchorDrives = false; break; }
        }
    }

    if (!anchor) {
        // First/independent net: two-column layout below existing content
        const float lx = kLeftMargin;
        const float rx = kLeftMargin + kInstW + kColGap;
        float dyl = g_layoutNextY, dyr = g_layoutNextY;
        for (const auto& item : drivers) {
            if (!item.isTerm) {
                g_placedPositions.emplace(item.key(), ImVec2{lx, dyl});
                dyl += kInstH + kRowSpacing;
            }
        }
        for (const auto& item : receivers) {
            if (!item.isTerm) {
                g_placedPositions.emplace(item.key(), ImVec2{rx, dyr});
                dyr += kInstH + kRowSpacing;
            }
        }
        g_layoutNextY = std::max(dyl, dyr) + kNetVGap;
    } else {
        // Expansion: extend horizontally from anchor
        const ImVec2& ap  = g_placedPositions[anchor->key()];
        const auto& items = anchorDrives ? receivers : drivers;
        float newX = anchorDrives
            ? ap.x + kInstW + kColGap    // new receivers go right of driver
            : ap.x - kInstW - kColGap;  // new drivers go left of receiver
        float dy = ap.y;
        // Two nets can anchor to the same instance (e.g. each input of a
        // gate in a driver trace) and would otherwise stack their new boxes
        // at the same spot in the same column: slide down past anything
        // already placed there.
        auto isFree = [&](float x, float y) {
            for (const auto& [key, p] : g_placedPositions)
                if (std::abs(p.x - x) < kInstW && std::abs(p.y - y) < kInstH + kRowSpacing / 2)
                    return false;
            return true;
        };
        for (const auto& item : items) {
            if (item.isTerm || g_placedPositions.count(item.key())) continue;
            while (!isFree(newX, dy)) dy += kInstH + kRowSpacing;
            g_placedPositions.emplace(item.key(), ImVec2{newX, dy});
            dy += kInstH + kRowSpacing;
        }
    }
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
    auto dataIt = g_instanceInternals.find(parent.name);
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
        cs.name          = parent.name.empty() ? c.name : parent.name + "/" + c.name;
        cs.modelName     = modelNameFromLeaf(c.name);
        cs.w             = kHierChildW;
        cs.h             = kHierChildH;
        cs.parentShapeId = parent.id;
        cs.hasChildren   = c.hasInstances;
        cs.hierExpanded  = c.hasInstances && g_hierExpanded.count(cs.name) > 0;
        cs.diagOutline   = DiagnosisStore::instanceColor(cs.name);
        childIdToShapeId[c.childId] = cs.id;
        g_occInfoByShapeId[cs.id] = { cs.name, c.designRef };
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
            p.color = DiagnosisStore::netColor(cs.name, stripBusIndex(p.name));
        }
        cs.ports = std::move(ports);
    }

    // Sub-pass 4: position children top-to-bottom in a single column,
    // recursing into an already-expanded child *before* moving on to the
    // next sibling so a grown height pushes later siblings down correctly.
    std::vector<InstanceShape> descendants;
    float x = parent.x + kHierMargin;
    float y = parent.y + kHierHeaderGap;
    for (auto& cs : children) {
        cs.x = x;
        cs.y = y;
        if (cs.hierExpanded && g_instanceInternals.count(cs.name)) {
            auto sub = emitInstanceInternals(cs, nextInstId, nextPortId);
            descendants.insert(descendants.end(), sub.shapes.begin(), sub.shapes.end());
            result.nets.insert(result.nets.end(), sub.nets.begin(), sub.nets.end());
        }
        y += cs.h + kHierGap;
    }

    float bottom = children.empty() ? (parent.y + kHierHeaderGap) : (y - kHierGap);
    parent.h = std::max(parent.h, (bottom - parent.y) + kHierMargin);
    parent.w = std::max(parent.w, kHierChildW + 2.0f * kHierMargin);

    result.shapes = std::move(children);
    result.shapes.insert(result.shapes.end(), descendants.begin(), descendants.end());
    return result;
}


// ---------------------------------------------------------------------------
// Hierarchy grouping: nested frames for the hierarchical modules containing
// the displayed leaf instances. A driver trace (or any equipotential) spans
// leaf cells anywhere in the design; without this, they're shown as one flat
// sea of boxes and the module structure is lost.
//
// Each leaf keeps the logic column the incremental layout gave it
// (layoutEquipotential's g_placedPositions x, i.e. its distance from the
// traced net), and the modules become a tree of frames laid out bottom-up:
// inside a frame, its own leaves and sub-frames are bucketed into columns by
// the (average) logic column of their contents, left to right, and stacked
// by their original vertical order. That keeps the left-to-right signal
// flow while guaranteeing frames nest cleanly and never overlap.
//
// Frames are separate InstanceShapes (isHierGroup) inserted at the front of
// g_schematic.instances; the leaves stay top-level shapes, so wiring, pins
// and hit-testing work exactly as in the flat layout.
// ---------------------------------------------------------------------------
struct LeafHier {
    std::vector<std::string> path;        // full instance-name path, leaf last
    std::vector<std::string> pathModels;  // matching model names ("" if unknown)
};

namespace {
struct GroupNode {
    std::string pathKey;
    std::string label;
    int         depth = 0;
    std::vector<std::unique_ptr<GroupNode>> groups;
    std::map<std::string, GroupNode*>       groupByName;
    std::vector<InstanceShape*>             leaves;
    // Filled by measureGroup().
    float  levelMin = 0.f, levelMax = 0.f, sortY = 0.f;
    float  w = 0.f, h = 0.f;
    ImVec2 rel{};                                  // top-left, relative to the parent frame
    std::vector<std::pair<InstanceShape*, ImVec2>> leafRel;
};

float leafLevel(const InstanceShape& s) {
    return (s.x - kLeftMargin) / (kInstW + kColGap);
}

void measureGroup(GroupNode& node, bool isRoot) {
    struct Elem { GroupNode* g; InstanceShape* leaf; float center, sortY, w, h; };
    std::vector<Elem> elems;
    node.levelMin = 1e9f; node.levelMax = -1e9f; node.sortY = 1e9f;
    for (auto& g : node.groups) {
        measureGroup(*g, false);
        elems.push_back({ g.get(), nullptr, 0.5f * (g->levelMin + g->levelMax), g->sortY, g->w, g->h });
        node.levelMin = std::min(node.levelMin, g->levelMin);
        node.levelMax = std::max(node.levelMax, g->levelMax);
        node.sortY    = std::min(node.sortY, g->sortY);
    }
    for (auto* leaf : node.leaves) {
        float lv = leafLevel(*leaf);
        elems.push_back({ nullptr, leaf, lv, leaf->y, leaf->w, leaf->h });
        node.levelMin = std::min(node.levelMin, lv);
        node.levelMax = std::max(node.levelMax, lv);
        node.sortY    = std::min(node.sortY, leaf->y);
    }

    // Bucket into columns by half-level so a module spanning an odd number
    // of logic columns doesn't get forced into the same column as a leaf.
    std::map<long, std::vector<Elem*>> columns;
    for (auto& e : elems) columns[std::lround(e.center * 2.0f)].push_back(&e);

    const float pad    = isRoot ? 0.f : kGroupPad;
    const float top    = isRoot ? 0.f : kGroupHeader;
    const float colGap = isRoot ? kColGap : kGroupColGap;
    float x = pad, maxBottom = top;
    for (auto& [col, members] : columns) {
        std::stable_sort(members.begin(), members.end(),
                         [](const Elem* a, const Elem* b) { return a->sortY < b->sortY; });
        float y = top, colW = 0.f;
        for (auto* e : members) {
            if (e->g) e->g->rel = ImVec2(x, y);
            else      node.leafRel.push_back({ e->leaf, ImVec2(x, y) });
            y   += e->h + kRowSpacing;
            colW = std::max(colW, e->w);
        }
        maxBottom = std::max(maxBottom, y - kRowSpacing);
        x += colW + colGap;
    }
    node.w = std::max(x - colGap + pad, 2.f * pad + kInstW);
    node.h = maxBottom + pad;
}

void placeGroup(const GroupNode& node, ImVec2 origin, bool isRoot,
                int& nextInstId, std::vector<InstanceShape>& frames) {
    if (!isRoot) {
        InstanceShape f;
        f.id          = nextInstId++;
        f.name        = node.label;
        f.x           = origin.x;
        f.y           = origin.y;
        f.w           = node.w;
        f.h           = node.h;
        f.isHierGroup = true;
        f.hierDepth   = node.depth;
        f.diagOutline = DiagnosisStore::instanceColor(node.pathKey);
        g_occInfoByShapeId[f.id] = { node.pathKey, DesignRef{}, std::nullopt };
        frames.push_back(std::move(f));
    }
    for (const auto& [leaf, rel] : node.leafRel) {
        leaf->x = origin.x + rel.x;
        leaf->y = origin.y + rel.y;
        if (!isRoot) leaf->label = leafSegment(leaf->name);
    }
    for (const auto& g : node.groups)
        placeGroup(*g, ImVec2(origin.x + g->rel.x, origin.y + g->rel.y), false, nextInstId, frames);
}
} // namespace

// Returns false (leaving every shape untouched) when no displayed leaf sits
// below the top design, i.e. there's no hierarchy to show.
static bool layoutHierarchyGroups(const std::map<std::string, LeafHier>& leafHier,
                                  const std::map<std::string, int>& keyToInstId,
                                  int& nextInstId) {
    bool anyNested = false;
    for (const auto& [key, lh] : leafHier)
        if (lh.path.size() >= 2 && keyToInstId.count(key)) { anyNested = true; break; }
    if (!anyNested) return false;

    GroupNode root;
    for (const auto& [key, lh] : leafHier) {
        auto kit = keyToInstId.find(key);
        if (kit == keyToInstId.end()) continue;
        InstanceShape* leaf = g_schematic.findInstanceById(kit->second);
        if (!leaf) continue;
        GroupNode* node = &root;
        for (size_t i = 0; i + 1 < lh.path.size(); ++i) {
            const std::string& seg = lh.path[i];
            auto git = node->groupByName.find(seg);
            if (git == node->groupByName.end()) {
                auto child = std::make_unique<GroupNode>();
                child->pathKey = node->pathKey.empty() ? seg : node->pathKey + "/" + seg;
                child->depth   = node->depth + 1;
                const std::string model = i < lh.pathModels.size() ? lh.pathModels[i] : "";
                child->label   = model.empty() ? seg : seg + " (" + model + ")";
                git = node->groupByName.emplace(seg, child.get()).first;
                node->groups.push_back(std::move(child));
            }
            node = git->second;
        }
        node->leaves.push_back(leaf);
    }

    measureGroup(root, true);
    std::vector<InstanceShape> frames;
    placeGroup(root, ImVec2(kLeftMargin, 0.f), true, nextInstId, frames);
    // Parent-first order at the front: drawn under everything, and reverse
    // hit-test scans still find a leaf before the frame around it.
    g_schematic.instances.insert(g_schematic.instances.begin(),
                                 std::make_move_iterator(frames.begin()),
                                 std::make_move_iterator(frames.end()));
    return true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void EquipotentialView::zoomIn()    { g_pendingZoomSteps++; }
void EquipotentialView::zoomOut()   { g_pendingZoomSteps--; }
void EquipotentialView::fitView()   { g_pendingFit = true; }
void EquipotentialView::clearNets() { g_pendingClear = true; }

void EquipotentialView::resetLayout() {
    g_placedPositions.clear();
    g_laidOut.clear();
    g_expandedInstances.clear();
    g_pendingExpansions.clear();
    g_expandedBuses.clear();
    g_hierExpanded.clear();
    g_hierPending.clear();
    g_instanceInternals.clear();
    g_layoutNextY = 0.f;
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

void EquipotentialView::setShowHierarchy(bool on) {
    if (g_showHierarchy == on) return;
    g_showHierarchy = on;
    g_pendingFit    = true;
}

void EquipotentialView::applyInstanceExpansion(
        const std::string& pathKey,
        const std::vector<ExpandedPort>& ports) {
    g_expandedInstances[pathKey] = ports;
    g_pendingExpansions.erase(pathKey);
}

void EquipotentialView::applyInstanceInternals(
        const std::string& pathKey,
        const InstanceInternals& data) {
    g_instanceInternals[pathKey] = data;
    g_hierPending.erase(pathKey);
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
        {
            // Same hit radius as the double-click pin test further down.
            const float sc  = std::max(0.01f, g_schematic.transform.scale);
            const float hr2 = (kPortHitRadiusPx / sc) * (kPortHitRadiusPx / sc);
            for (const auto& inst : g_schematic.instances) {
                for (const auto& port : inst.ports) {
                    ImVec2 pw = g_schematic.portWorldPos(inst, port);
                    float dx = wp.x - pw.x, dy = wp.y - pw.y;
                    if (dx*dx + dy*dy <= hr2 && g_portEquiByPortId.count(port.id))
                        g_ctxPortId = port.id;
                }
            }
        }
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
                req["path"]    = splitPathKey(occIt->second.pathKey);
                g_provider->send(req.dump());
            }
            // A module frame covers a lot of canvas, so right-clicking its
            // empty area also offers the canvas-level entries.
            auto* shape = g_schematic.findInstanceById(g_ctxInstanceId);
            if (shape && shape->isHierGroup) {
                if (ImGui::MenuItem("Zoom to Module")) g_pendingZoomGroup = occIt->second.pathKey;
                ImGui::Separator();
                canvasMenuItems();
            }
        } else {
            canvasMenuItems();
        }
        ImGui::EndPopup();
    }

    // Single click on an instance's hierarchy glyph: expand/collapse its
    // sub-instances nested inside its box. Checked before the double-click
    // handling below since it targets a small, distinct hotspot (top-center
    // of the box) that never overlaps a port dot or the body double-click
    // area used for expand_instance_terms.
    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        ImVec2 wp = mouseWorldPos(g_schematic, cpos);
        for (const auto& inst : g_schematic.instances) {
            if (!canShowHierToggle(inst)) continue;
            float gx0, gy0, gx1, gy1;
            hierToggleGlyphRect(inst, gx0, gy0, gx1, gy1);
            if (wp.x < gx0 || wp.x > gx1 || wp.y < gy0 || wp.y > gy1) continue;
            if (!g_hierExpanded.erase(inst.name)) {
                g_hierExpanded.insert(inst.name);
                if (!g_instanceInternals.count(inst.name) && !g_hierPending.count(inst.name) && g_provider) {
                    auto occIt = g_occInfoByShapeId.find(inst.id);
                    if (occIt != g_occInfoByShapeId.end()) {
                        g_hierPending.insert(inst.name);
                        json req;
                        req["request"]                  = "load_instance_internals";
                        req["path_key"]                 = inst.name;
                        req["design_ref"]["db_id"]      = occIt->second.designRef.db_id;
                        req["design_ref"]["library_id"] = occIt->second.designRef.library_id;
                        req["design_ref"]["design_id"]  = occIt->second.designRef.design_id;
                        g_provider->send(req.dump());
                    }
                }
            }
            break;
        }
    }

    // Double-click: port dot → load equip; instance box → expand interface
    if (ImGui::IsItemHovered() &&
        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && g_provider) {

        float s  = g_schematic.transform.scale;
        ImVec2 m = ImGui::GetMousePos();
        float wx = (m.x - cpos.x) / s + g_schematic.transform.offset.x
                   - g_schematic.transform.screenOrigin.x / s;
        float wy = (m.y - cpos.y) / s + g_schematic.transform.offset.y
                   - g_schematic.transform.screenOrigin.y / s;
        const float hr2 = (kPortHitRadiusPx / std::max(0.01f, s)) * (kPortHitRadiusPx / std::max(0.01f, s));
        bool hit = false;

        for (const auto& inst : g_schematic.instances) {
            for (const auto& port : inst.ports) {
                ImVec2 pw = g_schematic.portWorldPos(inst, port);
                float dx = wx - pw.x, dy = wy - pw.y;
                if (dx*dx + dy*dy > hr2) continue;
                auto busIt = g_busGroupByPortId.find(port.id);
                if (busIt != g_busGroupByPortId.end()) {
                    // Merged bus pin, or the topmost bit of an expanded bus:
                    // toggle expand/collapse instead of requesting a net.
                    // All bits are already loaded, so no request is needed.
                    if (!g_expandedBuses.erase(busIt->second))
                        g_expandedBuses.insert(busIt->second);
                    hit = true; break;
                }
                auto it = g_portEquiByPortId.find(port.id);
                if (it == g_portEquiByPortId.end()) break;
                json j;
                j["request"] = "load_equipotential";
                j["path"]    = it->second.pathIds;
                j["term_id"] = it->second.termId;
                if (it->second.bit.has_value()) j["bit"] = it->second.bit.value();
                g_provider->send(j.dump());
                hit = true; break;
            }
            if (hit) break;
        }
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
                if (it != g_occInfoByShapeId.end() && !g_pendingExpansions.count(it->second.pathKey)) {
                    g_pendingExpansions.insert(it->second.pathKey);
                    json req;
                    req["request"]                  = "expand_instance_terms";
                    req["path_key"]                 = it->second.pathKey;
                    req["design_ref"]["db_id"]      = it->second.designRef.db_id;
                    req["design_ref"]["library_id"] = it->second.designRef.library_id;
                    req["design_ref"]["design_id"]  = it->second.designRef.design_id;
                    g_provider->send(req.dump());
                }
            }
        }
    }

    // -----------------------------------------------------------------------
    // Layout new equips (once each, stores positions)
    // -----------------------------------------------------------------------
    for (Equipotential* eq : equipotentials)
        if (eq) layoutEquipotential(eq);

    // -----------------------------------------------------------------------
    // Rebuild geometry: merged instances + per-equip wires
    // -----------------------------------------------------------------------
    g_schematic.instances.clear();
    g_schematic.nets.clear();
    g_occInfoByShapeId.clear();
    g_portEquiByPortId.clear();
    g_busGroupByPortId.clear();

    int nextInstId = 1, nextPortId = 1, totalItems = 0;

    // Pass 1: collect merged instance data across all equips
    struct PortSlot {
        int                   portId;
        std::string           name;
        Direction             direction;
        unsigned              termChildId;
        std::optional<int>    termBit;
        std::vector<unsigned> pathIds;
    };
    struct MInst {
        ImVec2                pos{};
        bool                  initialized = false;
        DesignRef             designRef{};
        bool                  hasInstances = false;
        std::optional<SourceLoc> sourceLoc;
        std::vector<std::string> path;
        std::vector<std::string> pathModels;
        std::vector<PortSlot> ports;
    };
    std::map<std::string, MInst> minsts;

    // Per-equip wire endpoints: {key, portId}
    struct WireEnd { std::string key; int portId; };
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
                    equiEnds[ei].push_back({ "term:" + item.label, pid });
                    g_portEquiByPortId[pid] = { item.pathIds, item.termChildId, item.termBit };
                    continue;
                }
                auto& mi = minsts[item.key()];
                if (!mi.initialized) {
                    mi.initialized   = true;
                    mi.designRef     = item.designRef;
                    mi.hasInstances  = item.hasInstances;
                    mi.sourceLoc     = item.sourceLoc;
                    mi.path          = item.path;
                    mi.pathModels    = item.pathModels;
                    auto pit = g_placedPositions.find(item.key());
                    mi.pos = pit != g_placedPositions.end()
                        ? pit->second : ImVec2{kLeftMargin, 0.f};
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
                    ps.pathIds     = item.pathIds;
                    mi.ports.push_back(std::move(ps));
                    g_portEquiByPortId[pid] = { item.pathIds, item.termChildId, item.termBit };
                }
                equiEnds[ei].push_back({ item.key(), pid });
            }
        }
    }

    // Pass 2: build InstanceShapes from merged data
    std::map<std::string, int> keyToInstId;
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
        inst.x     = mi.pos.x;
        inst.y     = mi.pos.y;
        inst.w     = kInstW;
        inst.name      = key;
        inst.modelName = modelNameFromLeaf(leafSegment(key));
        inst.diagOutline = DiagnosisStore::instanceColor(key);
        g_occInfoByShapeId[inst.id] = { key, mi.designRef, mi.sourceLoc };
        keyToInstId[key] = inst.id;

        // Hierarchy embedding: this box's model has sub-instances, so it can
        // be expanded in place. If it's toggled open, either grow it now to
        // reserve room for its already-loaded internals (added after layout
        // finalizes positions, below) or kick off the request for them.
        inst.hasChildren  = mi.hasInstances;
        inst.hierExpanded = inst.hasChildren && g_hierExpanded.count(key) > 0;
        if (inst.hierExpanded) {
            auto internalsIt = g_instanceInternals.find(key);
            if (internalsIt != g_instanceInternals.end()) {
                size_t n = internalsIt->second.children.size();
                float blockH = n ? float(n) * kHierChildH + float(n - 1) * kHierGap : 0.f;
                inst.h += kHierHeaderGap + blockH + kHierMargin;
                inst.w  = std::max(inst.w, kHierChildW + 2.0f * kHierMargin);
            } else if (!g_hierPending.count(key) && g_provider) {
                g_hierPending.insert(key);
                json req;
                req["request"]                  = "load_instance_internals";
                req["path_key"]                 = key;
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
            struct ResolvedPort { const EquipotentialView::ExpandedPort* ep; int pid; };
            std::vector<ResolvedPort> resolved;
            resolved.reserve(expIt->second.size());
            for (const auto& ep : expIt->second) {
                int pid = nextPortId++;
                for (const auto& ps : mi.ports) {
                    if (ps.name == ep.name && ps.direction == ep.direction)
                        { pid = ps.portId; nextPortId--; break; }
                }
                auto pathIds = mi.ports.empty() ? std::vector<unsigned>{} : mi.ports[0].pathIds;
                g_portEquiByPortId[pid] = { pathIds, ep.childId, ep.bit };
                resolved.push_back({ &ep, pid });
            }

            struct Row { std::vector<const ResolvedPort*> members; bool merged = false;
                         std::string busBase; bool isExpandedBusTop = false; std::string groupKey; };
            std::vector<Row> rows;
            std::set<std::string> seenGroup;
            for (const auto& rp : resolved) {
                std::string base = stripBusIndex(rp.ep->name);
                bool isBusBit = base != rp.ep->name;
                if (!isBusBit) { rows.push_back({ {&rp}, false, "", false, "" }); continue; }
                bool isIn = rp.ep->direction == Direction::Input;
                std::string gk = busGroupKey(key, base, isIn);
                if (seenGroup.count(gk)) continue;
                seenGroup.insert(gk);
                std::vector<const ResolvedPort*> members;
                for (const auto& rp2 : resolved)
                    if (rp2.ep->direction == rp.ep->direction && stripBusIndex(rp2.ep->name) == base)
                        members.push_back(&rp2);
                bool expandedBus = members.size() == 1 || g_expandedBuses.count(gk);
                if (expandedBus) {
                    for (size_t i = 0; i < members.size(); ++i)
                        rows.push_back({ { members[i] }, false, "", i == 0 && members.size() > 1, gk });
                } else {
                    rows.push_back({ members, true, base, false, gk });
                }
            }

            int nL = 0, nR = 0;
            for (const auto& row : rows)
                (row.members[0]->ep->direction == Direction::Input ? nL : nR)++;
            inst.h = std::max(kInstH, float(std::max(nL, nR)) * kPortSpacing + 10.f);

            int li = 0, ri = 0;
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
                } else {
                    p.id   = row.members[0]->pid;
                    p.name = row.members[0]->ep->name;
                    p.color = DiagnosisStore::netColor(key, stripBusIndex(row.members[0]->ep->name));
                    if (row.isExpandedBusTop) g_busGroupByPortId[p.id] = row.groupKey;
                }
                p.lx = isIn ? -0.5f : 0.5f;
                p.ly = isIn ? portLy(li++, nL) : portLy(ri++, nR);
                p.direction = row.members[0]->ep->direction;
                p.isInput   = !isIn;
                inst.ports.push_back(p);
            }
        } else {
            inst.partialInterface = true;

            // Group the per-bit PortSlots accumulated in pass 1 by (direction,
            // bus base name) so a bus with >=2 loaded bits collapses to one
            // pin instead of one row per bit.
            struct Row { std::vector<const PortSlot*> members; bool merged = false;
                         std::string busBase; bool isExpandedBusTop = false; std::string groupKey; };
            std::vector<Row> rows;
            std::set<std::string> seenGroup;
            for (const auto& ps : mi.ports) {
                std::string base = stripBusIndex(ps.name);
                bool isBusBit = base != ps.name;
                if (!isBusBit) { rows.push_back({ {&ps}, false, "", false, "" }); continue; }
                bool isIn = ps.direction == Direction::Input;
                std::string gk = busGroupKey(key, base, isIn);
                if (seenGroup.count(gk)) continue;
                seenGroup.insert(gk);
                std::vector<const PortSlot*> members;
                for (const auto& ps2 : mi.ports)
                    if (ps2.direction == ps.direction && stripBusIndex(ps2.name) == base)
                        members.push_back(&ps2);
                bool expandedBus = members.size() == 1 || g_expandedBuses.count(gk);
                if (expandedBus) {
                    for (size_t i = 0; i < members.size(); ++i)
                        rows.push_back({ { members[i] }, false, "", i == 0 && members.size() > 1, gk });
                } else {
                    rows.push_back({ members, true, base, false, gk });
                }
            }

            int nL = 0, nR = 0;
            for (const auto& row : rows)
                (row.members[0]->direction == Direction::Input ? nL : nR)++;
            inst.h = std::max(kInstH, float(std::max(nL, nR)) * kPortSpacing + 10.f);

            int li = 0, ri = 0;
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
                    p.color = DiagnosisStore::netColor(key, stripBusIndex(row.members[0]->name));
                    if (row.isExpandedBusTop) g_busGroupByPortId[p.id] = row.groupKey;
                }
                p.lx = isIn ? -0.5f : 0.5f;
                p.ly = isIn ? portLy(li++, nL) : portLy(ri++, nR);
                p.direction = row.members[0]->direction;
                p.isInput   = !isIn;
                inst.ports.push_back(p);
            }
        }
        g_schematic.instances.push_back(std::move(inst));
    }

    // Resolve vertical overlaps: layoutEquipotential() reserves a fixed
    // kInstH-tall slot per instance, but an expanded instance's actual
    // height (computed above from its per-bit port count) can exceed that,
    // overlapping whatever was placed below it in the same column. Push
    // later instances down within each column and persist the correction
    // to g_placedPositions so future layout/anchor placement and Pass 3's
    // term-column bandY stay consistent with what's actually drawn.
    bool grouped = false;
    if (g_showHierarchy) {
        std::map<std::string, LeafHier> leafHier;
        for (const auto& [key, mi] : minsts) leafHier[key] = { mi.path, mi.pathModels };
        grouped = layoutHierarchyGroups(leafHier, keyToInstId, nextInstId);
    }
    if (!grouped) {
        std::map<int, std::vector<InstanceShape*>> byColumn;
        for (auto& inst : g_schematic.instances)
            if (inst.w > 0.f) byColumn[std::lround(inst.x)].push_back(&inst);
        for (auto& [x, col] : byColumn) {
            std::sort(col.begin(), col.end(),
                      [](const InstanceShape* a, const InstanceShape* b) { return a->y < b->y; });
            float minY = -1e9f;
            for (auto* inst : col) {
                if (inst->y < minY) inst->y = minY;
                minY = inst->y + inst->h + kRowSpacing;
                g_placedPositions[inst->name] = ImVec2{inst->x, inst->y};
            }
        }
    }

    // Hierarchy embedding: now that top-level positions are finalized (the
    // deoverlap pass above may have shifted a box's y), lay out and append
    // the nested children/internal-nets of every expanded, loaded instance.
    // Collected as ids first (not pointers) since emission appends to the
    // very vectors we're about to iterate, which would invalidate iterators
    // held across a push_back.
    {
        std::vector<int> hierExpandIds;
        for (const auto& inst : g_schematic.instances)
            if (inst.hierExpanded && g_instanceInternals.count(inst.name))
                hierExpandIds.push_back(inst.id);

        for (int id : hierExpandIds) {
            auto* parent = g_schematic.findInstanceById(id);
            if (!parent) continue;
            auto result = emitInstanceInternals(*parent, nextInstId, nextPortId);
            for (auto& s : result.shapes) g_schematic.instances.push_back(std::move(s));
            for (auto& n : result.nets)   g_schematic.nets.push_back(std::move(n));
        }
    }

    // Pass 3: build term InstanceShapes per equip (not merged)
    // Port flags already placed, so two nets whose band lands on the same
    // spot (e.g. two inputs feeding the same cell) stack instead of overlap.
    std::vector<ImVec2> placedTermPos;
    for (size_t ei = 0; ei < equipotentials.size(); ++ei) {
        Equipotential* eq = equipotentials[ei];
        if (!eq) continue;

        std::vector<Item> drivers, receivers;
        buildItems(eq, drivers, receivers);

        // Determine column X for this equip's terms from placed instances
        float lx = kLeftMargin, rx = kLeftMargin + kInstW + kColGap;
        float bandY = 0.f; int nBand = 0;
        auto accInst = [&](const Item& item) {
            if (item.isTerm) return;
            auto kit = keyToInstId.find(item.key());
            const InstanceShape* shape =
                kit != keyToInstId.end() ? g_schematic.findInstanceById(kit->second) : nullptr;
            if (!shape) return;
            lx = std::min(lx, shape->x);
            rx = std::max(rx, shape->x + shape->w);
            bandY += shape->y; ++nBand;
        };
        for (const auto& d : drivers)   accInst(d);
        for (const auto& r : receivers) accInst(r);
        if (nBand) bandY /= float(nBand);
        // With module frames drawn, top-level ports belong outside the
        // outermost frame, not next to a leaf that sits deep inside one.
        if (grouped) {
            for (const auto& inst : g_schematic.instances) {
                if (!inst.isHierGroup) continue;
                lx = std::min(lx, inst.x);
                rx = std::max(rx, inst.x + inst.w);
            }
        }

        float termLx = lx  - 40.f;
        float termRx = rx  + 20.f;

        for (int pass = 0; pass < 2; ++pass) {
            const auto& items = pass == 0 ? drivers : receivers;
            float ty = bandY;
            for (const auto& item : items) {
                if (!item.isTerm) { ty += kInstH + kRowSpacing; continue; }

                int pid = -1;
                for (const auto& we : equiEnds[ei]) {
                    if (we.key == "term:" + item.label) { pid = we.portId; break; }
                }
                if (pid < 0) continue;

                bool isInput = (item.direction == Direction::Input);
                const float tx = isInput ? termLx : termRx;
                for (bool moved = true; moved; ) {
                    moved = false;
                    for (const auto& p : placedTermPos)
                        if (std::abs(p.x - tx) < 1.f && std::abs(p.y - ty) < 24.f)
                            { ty = p.y + 30.f; moved = true; }
                }
                placedTermPos.push_back(ImVec2(tx, ty));
                InstanceShape inst;
                inst.id = nextInstId++;
                inst.x  = tx;
                inst.y  = ty;
                inst.w  = 0.f; inst.h = 0.f;
                inst.partialInterface = false;
                // Draws as a schematic-style boundary-port flag (see
                // drawBoundaryPortInstance in SchematicView.cpp) rather than
                // a generic box -- this pseudo-instance only exists to give
                // the top-level design port a wire anchor point.
                inst.modelName = "port";

                Port p;
                p.id = pid; p.name = item.label;
                p.lx = isInput ? 1.0f : -1.0f;
                p.ly = 0.f;
                p.direction = item.direction;
                p.isInput   = isInput;
                p.color     = DiagnosisStore::netColor("", stripBusIndex(item.label));
                inst.ports.push_back(p);
                keyToInstId["term:" + item.label + ":" + std::to_string(ei)] = inst.id;
                // Patch the equiEnds key so wire lookup works
                for (auto& we : equiEnds[ei]) {
                    if (we.key == "term:" + item.label)
                        we.key = "term:" + item.label + ":" + std::to_string(ei);
                }
                g_schematic.instances.push_back(std::move(inst));
                ty += 30.f;
            }
        }
    }

    // Pass 4: wires per equip
    auto renderedPort = [&](int pid) {
        auto it = logicalToRenderedPortId.find(pid);
        return it != logicalToRenderedPortId.end() ? it->second : pid;
    };
    for (size_t ei = 0; ei < equipotentials.size(); ++ei) {
        const auto& ends = equiEnds[ei];
        if (ends.size() < 2) continue;

        struct WR { int instId; int portId; };
        std::vector<WR> wrs;
        for (const auto& we : ends) {
            auto it = keyToInstId.find(we.key);
            if (it == keyToInstId.end()) continue;
            wrs.push_back({ it->second, renderedPort(we.portId) });
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
        std::vector<NetWire> merged;
        for (const auto& n : g_schematic.nets) {
            auto k = std::make_tuple(n.srcInstance, n.srcPortId, n.dstInstance, n.dstPortId);
            auto it = firstOf.find(k);
            if (it == firstOf.end()) {
                firstOf[k] = merged.size();
                merged.push_back(n);
            } else {
                merged[it->second].isBus = true;
            }
        }
        g_schematic.nets = std::move(merged);
    }

    static int lastTotal = -1;
    if (totalItems != lastTotal) { g_schematic.requestFit(true); lastTotal = totalItems; }
    if (!g_pendingZoomGroup.empty()) {
        for (const auto& inst : g_schematic.instances) {
            if (!inst.isHierGroup) continue;
            auto it = g_occInfoByShapeId.find(inst.id);
            if (it == g_occInfoByShapeId.end() || it->second.pathKey != g_pendingZoomGroup) continue;
            g_schematic.requestFitRect(ImVec2(inst.x, inst.y), ImVec2(inst.x + inst.w, inst.y + inst.h));
            break;
        }
        g_pendingZoomGroup.clear();
    }
    g_schematic.updateFitIfNeeded(cpos, inner, 60.f);

    // Hover tooltip: show diagnosis messages for the instance under the cursor.
    if (ImGui::IsMouseHoveringRect(cpos, ImVec2(cpos.x + inner.x, cpos.y + inner.y))) {
        ImVec2 wp = mouseWorldPos(g_schematic, cpos);
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
            auto diagnostics = DiagnosisStore::instanceDiagnostics(it->second.pathKey);
            if (!diagnostics.empty()) {
                ImGui::BeginTooltip();
                for (const auto* d : diagnostics) {
                    ImGui::TextColored(ImColor(DiagnosisStore::colorForSeverity(d->severity)).Value,
                                       "[%s] %s", toString(d->severity), d->message.c_str());
                    if (!d->source.empty()) ImGui::TextDisabled("source: %s", d->source.c_str());
                }
                ImGui::EndTooltip();
            }
            break;
        }
    }

    g_schematic.render(dl, cpos, inner);
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
            std::string label;
            for (const auto& seg : occ.path) { label += seg; label += '/'; }
            label += occ.term.getString();
            // The modules enclosing the instance, outermost first, each with
            // its model name when the provider sent one.
            std::string context;
            for (size_t k = 0; k + 1 < occ.path.size(); ++k) {
                if (k) context += " > ";
                context += occ.path[k];
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

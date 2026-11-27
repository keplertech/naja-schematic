// SchematicLayout.cpp — pure layout geometry for the equipotential schematic.
#include "SchematicLayout.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <memory>
#include <tuple>

namespace SchematicLayout {

void buildItems(const Equipotential* eq,
                std::vector<Item>& drivers,
                std::vector<Item>& receivers) {
    for (const auto& bt : eq->terms) {
        Item item;
        item.label       = bt.getString();
        item.direction   = bt.direction;
        item.isTerm      = true;
        item.termChildId = bt.child_id;
        item.termBit     = bt.bit;
        if (bt.net) item.netName = displayNetLabel(*bt.net, bt.netId);
        (bt.direction == Direction::Input ? drivers : receivers).push_back(std::move(item));
    }
    for (const auto& occ : eq->occurrences) {
        Item item;
        item.label       = occ.term.getString();
        item.isTerm      = false;
        item.designRef   = occ.designRef;
        item.primitiveType = occ.primitiveType;
        item.termChildId = occ.term.child_id;
        item.termBit     = occ.term.bit;
        item.clock       = occ.term.clock;
        item.role        = occ.term.role;
        if (occ.term.net) item.netName = displayNetLabel(*occ.term.net, occ.term.netId);
        item.pathIds     = occ.pathIds;
        item.hasInstances = occ.has_instances;
        item.bitTermCount = occ.bit_term_count;
        item.hasSources   = occ.has_sources;
        item.path         = occ.path;
        item.pathModels   = occ.pathModels;
        item.direction = occ.term.direction;
        (occ.term.direction == Direction::Output ? drivers : receivers).push_back(std::move(item));
    }
}

// ---------------------------------------------------------------------------
// Opened instances
// ---------------------------------------------------------------------------
namespace {
// A pin: the instance it's on (an occurrence's path, or an opened
// instance's own pin), its term id and bit (-1: not a bus bit).
using PinKey = std::tuple<InstancePath, unsigned, int>;

PinKey pinKey(const InstancePath& path, const BitTerm& t) { return { path, t.child_id, t.bit.value_or(-1) }; }

bool strictlyInside(const InstancePath& outer, const InstancePath& path) {
    return path.size() > outer.size() && std::equal(outer.begin(), outer.end(), path.begin());
}
} // namespace

OpenedView openInstances(const std::vector<const Equipotential*>& shown,
                         const std::map<InstancePath, OpenedInstance>& opened) {
    // Pieces: the shown nets, then every opened instance's nets (`owner`).
    struct Piece { const Equipotential* eq; const OpenedInstance* owner; };
    std::vector<Piece> pieces;
    for (const auto* eq : shown)
        if (eq) pieces.push_back({ eq, nullptr });
    for (const auto& [path, oi] : opened)
        for (const auto& net : oi.nets) pieces.push_back({ &net, &oi });

    // Every pin a piece has; an opened instance's net also has the
    // instance's own pins (its terms).
    std::map<PinKey, std::vector<size_t>> piecesOfPin;
    for (size_t i = 0; i < pieces.size(); ++i) {
        for (const auto& occ : pieces[i].eq->occurrences) piecesOfPin[pinKey(occ.path, occ.term)].push_back(i);
        if (pieces[i].owner)
            for (const auto& t : pieces[i].eq->terms) piecesOfPin[pinKey(pieces[i].owner->path, t)].push_back(i);
    }
    // Joined where they share a pin, when an opened instance's net is one
    // of them (two shown nets stay apart, as they're drawn without it).
    std::vector<size_t> parent(pieces.size());
    for (size_t i = 0; i < parent.size(); ++i) parent[i] = i;
    auto find = [&](size_t i) {
        while (parent[i] != i) i = parent[i] = parent[parent[i]];
        return i;
    };
    for (const auto& [key, ids] : piecesOfPin) {
        if (std::none_of(ids.begin(), ids.end(), [&](size_t i) { return pieces[i].owner != nullptr; })) continue;
        for (size_t k = 1; k < ids.size(); ++k) parent[find(ids[k])] = find(ids[0]);
    }

    // Flag ids, per opened instance: its pins in the order its nets list them.
    std::map<PinKey, unsigned> flagIndex;
    for (const auto& [path, oi] : opened) {
        unsigned next = 0;
        for (const auto& net : oi.nets)
            for (const auto& t : net.terms)
                if (flagIndex.emplace(pinKey(path, t), next).second) ++next;
    }

    OpenedView view;
    std::map<size_t, std::vector<size_t>> members;  // by root, in piece order
    std::vector<size_t> roots;
    for (size_t i = 0; i < pieces.size(); ++i) {
        auto& m = members[find(i)];
        if (m.empty()) roots.push_back(find(i));
        m.push_back(i);
    }
    for (size_t root : roots) {
        Equipotential out;
        out.found = true;
        bool anyShown = false, direct = false;
        std::set<std::pair<unsigned, int>> termSeen;
        std::set<PinKey> occSeen;
        struct Flag { const OpenedInstance* owner; const BitTerm* term; };
        std::vector<Flag> flags;
        std::set<PinKey> flagSeen;
        for (size_t i : members[root]) {
            const Piece& pc = pieces[i];
            if (!pc.owner) {
                anyShown = true;
                direct   = direct || pc.eq->direct;
                for (int id : pc.eq->traceIds)
                    if (std::find(out.traceIds.begin(), out.traceIds.end(), id) == out.traceIds.end())
                        out.traceIds.push_back(id);
                for (const auto& [id, insts] : pc.eq->traceInstances)
                    out.traceInstances[id].insert(insts.begin(), insts.end());
                for (const auto& t : pc.eq->terms)
                    if (termSeen.insert({ t.child_id, t.bit.value_or(-1) }).second) out.terms.push_back(t);
            } else {
                for (const auto& t : pc.eq->terms)
                    if (flagSeen.insert(pinKey(pc.owner->path, t)).second) flags.push_back({ pc.owner, &t });
            }
            for (const auto& occ : pc.eq->occurrences) {
                if (opened.count(occ.path)) continue;  // drawn as its contents
                if (occSeen.insert(pinKey(occ.path, occ.term)).second) out.occurrences.push_back(occ);
            }
        }
        out.direct = anyShown ? direct : true;
        // A pin of an opened instance gets a flag when nothing on its net is
        // outside the instance: everything else is inside it, or another of
        // its own pins (a net passing through).
        std::vector<const InstancePath*> anchors;
        for (const auto& occ : out.occurrences) anchors.push_back(&occ.path);
        static const InstancePath kTop;
        for (size_t k = 0; k < out.terms.size(); ++k) anchors.push_back(&kTop);
        std::vector<InstTermOccurrence> flagOccs;  // appended after: `anchors` points into the list
        for (const auto& f : flags) {
            const InstancePath& owner = f.owner->path;
            bool alone = std::all_of(anchors.begin(), anchors.end(),
                                     [&](const InstancePath* a) { return strictlyInside(owner, *a); });
            for (const auto& g : flags)
                alone = alone && (g.owner->path == owner || strictlyInside(owner, g.owner->path));
            if (!alone) continue;
            InstTermOccurrence occ;
            occ.path = owner;
            occ.path.push_back({ kBoundaryIdBase + flagIndex[pinKey(owner, *f.term)], "" });
            occ.pathIds    = pathIds(owner);
            occ.pathModels = f.owner->pathModels;
            occ.pathModels.push_back("");
            occ.term       = *f.term;
            // Seen from inside: an input pin drives the net, an output
            // receives it.
            occ.term.direction = f.term->direction == Direction::Input  ? Direction::Output
                               : f.term->direction == Direction::Output ? Direction::Input
                                                                        : Direction::Inout;
            occ.bit_term_count = 1;
            view.boundary[occ.path] = { owner, f.owner->pathModels, *f.term };
            flagOccs.push_back(std::move(occ));
        }
        for (auto& occ : flagOccs) out.occurrences.push_back(std::move(occ));
        if (out.terms.size() + out.occurrences.size() < 1) continue;
        view.nets.push_back(std::move(out));
    }
    return view;
}

// ---------------------------------------------------------------------------
// Symbols
// ---------------------------------------------------------------------------
namespace {
// Approximate width of one pin-label character at the base label size, and
// the longest label a box makes room for (drawPorts truncates past that).
constexpr float  kPinCharW       = 6.5f;
constexpr size_t kPinLabelMaxLen = 14;

float snapUp(float v, float step) { return std::ceil(v / step - 1e-4f) * step; }

float labelWidth(const std::vector<std::string>& names) {
    size_t n = 0;
    for (const auto& s : names) n = std::max(n, std::min(s.size(), kPinLabelMaxLen));
    return float(n) * kPinCharW;
}
} // namespace

// ---------------------------------------------------------------------------
// Instance names
// ---------------------------------------------------------------------------
bool naturalLess(const std::string& a, const std::string& b) {
    auto digit = [](char c) { return c >= '0' && c <= '9'; };
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (digit(a[i]) && digit(b[j])) {
            size_t i1 = i, j1 = j;
            while (i1 < a.size() && digit(a[i1])) ++i1;
            while (j1 < b.size() && digit(b[j1])) ++j1;
            // Compare values without overflow: leading zeros off, then by
            // length, then digit by digit.
            size_t i0 = i, j0 = j;
            while (i0 + 1 < i1 && a[i0] == '0') ++i0;
            while (j0 + 1 < j1 && b[j0] == '0') ++j0;
            if (i1 - i0 != j1 - j0) return i1 - i0 < j1 - j0;
            const int c = a.compare(i0, i1 - i0, b, j0, j1 - j0);
            if (c != 0) return c < 0;
            i = i1;
            j = j1;
            continue;
        }
        if (a[i] != b[j]) return (unsigned char)a[i] < (unsigned char)b[j];
        ++i;
        ++j;
    }
    if ((i < a.size()) != (j < b.size())) return j < b.size();
    return a < b;
}

bool naturalPathLess(const InstancePath& a, const InstancePath& b) {
    for (size_t k = 0; k < a.size() && k < b.size(); ++k) {
        if (naturalLess(a[k].name, b[k].name)) return true;
        if (naturalLess(b[k].name, a[k].name)) return false;
    }
    if (a.size() != b.size()) return a.size() < b.size();
    return a < b;
}

std::string shortenMiddle(const std::string& name, const NameWidthFn& width, float maxW) {
    if (width(name) <= maxW) return name;
    static const std::string kEllipsis = "...";
    if (width(kEllipsis) > maxW) return "";
    // Most characters kept, split evenly between head and tail (the tail
    // gets the odd one: it's what tells siblings apart most often).
    auto cut = [&](size_t kept) {
        const size_t head = kept / 2;
        return name.substr(0, head) + kEllipsis + name.substr(name.size() - (kept - head));
    };
    size_t lo = 0, hi = name.size() - 1;
    while (lo < hi) {
        const size_t mid = (lo + hi + 1) / 2;
        if (width(cut(mid)) <= maxW) lo = mid; else hi = mid - 1;
    }
    return cut(lo);
}

std::string shownName(const InstanceShape& s, const NameWidthFn& width) {
    if (s.isHierGroup || s.modelName == "port" || s.hideName) return "";
    return shortenMiddle(s.label.empty() ? s.name : s.label, width, kNameMaxW);
}

std::string shownModel(const InstanceShape& s, const NameWidthFn& width) {
    if (!s.showModel || s.model.empty() || s.isHierGroup || s.modelName == "port") return "";
    return shortenMiddle(s.model, width, kNameMaxW);
}

float footprintWidth(const InstanceShape& s, const NameWidthFn& width) {
    float w = s.w;
    const std::string name = shownName(s, width);
    if (!name.empty()) w = std::max(w, width(name));
    const std::string model = shownModel(s, width);
    if (!model.empty()) w = std::max(w, width(model));
    for (const auto& l : netLabelPlaces(s, width)) w = std::max(w, l.rect.x1 - s.x);
    return w;
}

float footprintHeight(const InstanceShape& s, const NameWidthFn& width) {
    return s.h + (shownModel(s, width).empty() ? 0.f : kNameH);
}

std::optional<RouteRect> modelRect(const InstanceShape& s, const NameWidthFn& width) {
    const std::string model = shownModel(s, width);
    if (model.empty()) return std::nullopt;
    return RouteRect{ s.x, s.y + s.h, s.x + width(model), s.y + s.h + kNameH };
}

std::vector<NetLabelPlace> netLabelPlaces(const InstanceShape& s, const NameWidthFn& width) {
    std::vector<NetLabelPlace> places;
    const auto name = nameRect(s, width);
    for (const auto& p : s.ports) {
        if (p.netLabelW <= 0.f || p.lx < 0.f) continue;
        const ImVec2 pin = portAnchor(s, p);
        const float y1 = pin.y - kNetLabelLift, y0 = y1 - kNetLabelH;
        float x = pin.x + kNetLabelGap;
        if (name && y0 < name->y1 && y1 > name->y0) x = std::max(x, name->x1 + kNetLabelGap);
        places.push_back({ { s.x + s.w, y0, x + p.netLabelW, y1 }, ImVec2(x, y1), p.id });
    }
    return places;
}

std::optional<RouteRect> nameRect(const InstanceShape& s, const NameWidthFn& width) {
    const std::string name = shownName(s, width);
    if (name.empty()) return std::nullopt;
    return RouteRect{ s.x, s.y - kNameH, s.x + width(name), s.y };
}

bool isGateSymbol(PrimitiveType type) {
    switch (type) {
        case PrimitiveType::And:  case PrimitiveType::Nand:
        case PrimitiveType::Or:   case PrimitiveType::Nor:
        case PrimitiveType::Xor:  case PrimitiveType::Xnor:
        case PrimitiveType::Inv:  case PrimitiveType::Buf:
        case PrimitiveType::Assign:
        case PrimitiveType::Tie0: case PrimitiveType::Tie1:
            return true;
        default:
            return false;
    }
}

SymbolGeometry symbolGeometry(PrimitiveType type,
                              const std::vector<std::string>& leftNames,
                              const std::vector<std::string>& rightNames) {
    SymbolGeometry g;
    const size_t nL = leftNames.size(), nR = rightNames.size();
    if (isGateSymbol(type) && nR <= 1) {
        // Inputs centered on the output, kPinPitch apart: with an even
        // pitch and h a multiple of it, every pin lands on the grid.
        g.h = std::max(2.f * kPinPitch, kPinPitch * float(nL));
        for (size_t i = 0; i < nL; ++i)
            g.leftY.push_back(0.5f * g.h + kPinPitch * (float(i) - 0.5f * float(nL - 1)));
        if (nR) g.rightY.push_back(0.5f * g.h);
        const bool single = type == PrimitiveType::Inv || type == PrimitiveType::Buf ||
                            type == PrimitiveType::Assign || type == PrimitiveType::Tie0 ||
                            type == PrimitiveType::Tie1;
        const bool xorLike = type == PrimitiveType::Xor || type == PrimitiveType::Xnor;
        g.w = single ? 2.f * kPinPitch : snapUp(g.h + kGrid + (xorLike ? kGrid : 0.f), kGrid);
        return g;
    }
    // Box: pins run down from the top, names drawn inside next to them.
    const size_t rows = std::max(nL, nR);
    g.h = std::max(2.f * kPinPitch, kPinPitch * float(rows + 1));
    for (size_t i = 0; i < nL; ++i) g.leftY.push_back(kPinPitch * float(i + 1));
    if (type == PrimitiveType::Mux) {
        // Outputs centered: h is a kPinPitch multiple, so h/2 (and every
        // pin a pitch from it) is on the grid.
        for (size_t i = 0; i < nR; ++i)
            g.rightY.push_back(0.5f * g.h + kPinPitch * (float(i) - 0.5f * float(nR - 1)));
    } else {
        for (size_t i = 0; i < nR; ++i) g.rightY.push_back(kPinPitch * float(i + 1));
    }
    const float glyph = isOperatorSymbol(type) ? kOperatorGlyphW : 0.f;
    g.w = snapUp(std::max(6.f * kGrid, labelWidth(leftNames) + labelWidth(rightNames) + glyph + 3.f * kGrid), kGrid);
    return g;
}

float muxSlant(float h, size_t rightPins) {
    const float outerPin = 0.5f * h - 0.5f * kPinPitch * float(rightPins ? rightPins - 1 : 0);
    return std::max(0.f, std::min(0.25f * h, outerPin - 0.5f * kPinPitch));
}

bool isOperatorSymbol(PrimitiveType type) {
    switch (type) {
        case PrimitiveType::Add:  case PrimitiveType::Sub:
        case PrimitiveType::Mul:  case PrimitiveType::DivMod:
        case PrimitiveType::Shl:  case PrimitiveType::Shr:
        case PrimitiveType::Ashr:
            return true;
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------
// Layered placement
// ---------------------------------------------------------------------------
namespace {
struct PlaceEdge {
    int   u, v;      // driver node -> receiver node
    float du, dv;    // pin offsets on each
};

// Weighted isotonic regression with spacing, by pool-adjacent-violators:
// places items (in this order, top to bottom) at y_i >= y_{i-1} + h_{i-1} +
// gap, as close as possible (least squares, weighted) to their targets.
std::vector<float> packColumn(const std::vector<float>& target, const std::vector<float>& weight,
                              const std::vector<float>& h, float gap, bool snap = true) {
    const size_t n = target.size();
    std::vector<float> offset(n, 0.f);
    for (size_t i = 1; i < n; ++i) offset[i] = offset[i - 1] + h[i - 1] + gap;
    struct Block { float sumW, sumWZ; size_t first, count; };
    std::vector<Block> blocks;
    for (size_t i = 0; i < n; ++i) {
        blocks.push_back({ weight[i], weight[i] * (target[i] - offset[i]), i, 1 });
        while (blocks.size() > 1) {
            Block& b = blocks.back();
            Block& a = blocks[blocks.size() - 2];
            if (a.sumWZ / a.sumW <= b.sumWZ / b.sumW) break;
            a.sumW += b.sumW; a.sumWZ += b.sumWZ; a.count += b.count;
            blocks.pop_back();
        }
    }
    std::vector<float> y(n, 0.f);
    for (const auto& b : blocks) {
        float z = b.sumWZ / b.sumW;
        if (snap) z = std::round(z / kGrid) * kGrid;
        for (size_t i = b.first; i < b.first + b.count; ++i) y[i] = z + offset[i];
    }
    return y;
}

float lowerMedian(std::vector<float> v) {
    std::nth_element(v.begin(), v.begin() + (v.size() - 1) / 2, v.end());
    return v[(v.size() - 1) / 2];
}
} // namespace

namespace {
// How much more a hinted node's height counts than one pin's alignment.
constexpr float kHintWeight = 1000.f;

// Levels with hints (see Stability in SchematicLayout.h): the hinted nodes
// of one hinted level are one group, the groups chained in level order at
// their hinted distances; every other node is a group of its own. Edges
// between groups are levelled by longest path, after breaking cycles with
// the chain walked first, so a cycle is always broken on a real edge. An
// edge within a group, or between two hinted groups the wrong way round, is
// a back edge. `topo` gets the nodes in a topological order.
void hintedLevels(const std::vector<PlaceNode>& nodes, const std::vector<PlaceEdge>& edges,
                  const std::vector<std::vector<int>>& outEdges, std::vector<char>& isBack,
                  std::vector<int>& level, std::vector<int>& topo) {
    const int n = int(nodes.size());
    std::vector<int> cols;
    for (const auto& p : nodes) if (p.hinted) cols.push_back(p.hintLevel);
    std::sort(cols.begin(), cols.end());
    cols.erase(std::unique(cols.begin(), cols.end()), cols.end());
    const int chained = int(cols.size());
    std::vector<int> group(n);
    int groups = chained;
    for (int v = 0; v < n; ++v)
        group[v] = nodes[v].hinted
                       ? int(std::lower_bound(cols.begin(), cols.end(), nodes[v].hintLevel) - cols.begin())
                       : groups++;

    struct GEdge { int to, weight, edge; };  // edge -1: the chain
    std::vector<std::vector<GEdge>> gOut(groups);
    for (int k = 0; k + 1 < chained; ++k) gOut[k].push_back({ k + 1, cols[k + 1] - cols[k], -1 });
    for (int v = 0; v < n; ++v)
        for (int e : outEdges[v]) {
            const int a = group[edges[e].u], b = group[edges[e].v];
            if (a == b || (a < chained && b < chained && a > b)) { isBack[e] = 1; continue; }
            gOut[a].push_back({ b, 1, e });
        }

    // Cycle breaking, chain groups first (their chain edge first): the
    // chain is the DFS's first path, so it's never a back edge.
    std::vector<char> state(groups, 0);
    std::vector<std::vector<char>> gBack(groups);
    for (int g = 0; g < groups; ++g) gBack[g].assign(gOut[g].size(), 0);
    for (int root = 0; root < groups; ++root) {
        if (state[root]) continue;
        std::vector<std::pair<int, size_t>> stack{ { root, 0 } };
        state[root] = 1;
        while (!stack.empty()) {
            auto& [g, next] = stack.back();
            if (next == gOut[g].size()) { state[g] = 2; stack.pop_back(); continue; }
            const size_t i = next++;
            const int to = gOut[g][i].to;
            if (state[to] == 1) {
                gBack[g][i] = 1;
                if (gOut[g][i].edge >= 0) isBack[size_t(gOut[g][i].edge)] = 1;
            } else if (state[to] == 0) {
                state[to] = 1;
                stack.push_back({ to, 0 });
            }
        }
    }

    std::vector<int> indeg(groups, 0), gLevel(groups, 0), gTopo;
    for (int g = 0; g < groups; ++g)
        for (size_t i = 0; i < gOut[g].size(); ++i) if (!gBack[g][i]) ++indeg[gOut[g][i].to];
    for (int g = 0; g < groups; ++g) if (!indeg[g]) gTopo.push_back(g);
    for (size_t k = 0; k < gTopo.size(); ++k) {
        const int g = gTopo[k];
        for (size_t i = 0; i < gOut[g].size(); ++i) {
            if (gBack[g][i]) continue;
            const GEdge& ge = gOut[g][i];
            gLevel[ge.to] = std::max(gLevel[ge.to], gLevel[g] + ge.weight);
            if (--indeg[ge.to] == 0) gTopo.push_back(ge.to);
        }
    }
    std::vector<std::vector<int>> members(groups);
    for (int v = 0; v < n; ++v) {
        level[v] = gLevel[group[v]];
        members[group[v]].push_back(v);
    }
    topo.clear();
    for (int g : gTopo) topo.insert(topo.end(), members[g].begin(), members[g].end());
}
} // namespace

Placement layeredPlacement(const std::vector<PlaceNode>& nodes, const std::vector<PlaceNet>& nets,
                           int maxSinkRows) {
    const int n = int(nodes.size());
    Placement out;
    out.pos.assign(n, ImVec2(0.f, 0.f));
    out.level.assign(n, 0);
    if (n == 0) return out;

    std::vector<PlaceEdge> edges;
    for (const auto& net : nets)
        for (const auto& d : net.drivers)
            for (const auto& r : net.receivers)
                if (d.node != r.node && d.node >= 0 && r.node >= 0 && d.node < n && r.node < n)
                    edges.push_back({ d.node, r.node, d.dy, r.dy });

    std::vector<std::vector<int>> outEdges(n), inEdges(n);
    for (int e = 0; e < int(edges.size()); ++e) {
        outEdges[edges[e].u].push_back(e);
        inEdges[edges[e].v].push_back(e);
    }

    const bool hinted = std::any_of(nodes.begin(), nodes.end(), [](const PlaceNode& p) { return p.hinted; });
    std::vector<char> isBack(edges.size(), 0);
    std::vector<int> topo;
    if (!hinted) {
        // 1. Cycle breaking: an edge to a node still on the DFS stack closes a
        //    loop (e.g. a flop's feedback) and is left out of the layering.
        std::vector<char> state(n, 0);
        for (int root = 0; root < n; ++root) {
            if (state[root]) continue;
            std::vector<std::pair<int, size_t>> stack{ { root, 0 } };
            state[root] = 1;
            while (!stack.empty()) {
                auto& [v, next] = stack.back();
                if (next == outEdges[v].size()) { state[v] = 2; stack.pop_back(); continue; }
                int e = outEdges[v][next++];
                int w = edges[e].v;
                if (state[w] == 1) isBack[e] = 1;
                else if (state[w] == 0) { state[w] = 1; stack.push_back({ w, 0 }); }
            }
        }

        // 2. Longest-path levels over the forward edges (Kahn order)...
        std::vector<int> indeg(n, 0);
        for (int e = 0; e < int(edges.size()); ++e) if (!isBack[e]) ++indeg[edges[e].v];
        for (int v = 0; v < n; ++v) if (!indeg[v]) topo.push_back(v);
        for (size_t i = 0; i < topo.size(); ++i) {
            int u = topo[i];
            for (int e : outEdges[u]) {
                if (isBack[e]) continue;
                int v = edges[e].v;
                out.level[v] = std::max(out.level[v], out.level[u] + 1);
                if (--indeg[v] == 0) topo.push_back(v);
            }
        }
    } else {
        hintedLevels(nodes, edges, outEdges, isBack, out.level, topo);
    }
    // ...then send every node that only feeds top-level outputs (e.g. a tie
    // cell on an output) to the last column, beside the output ports, rather
    // than leave it at its own depth with a wire across the whole sheet...
    int lastLevel = 0;
    for (int v = 0; v < n; ++v) lastLevel = std::max(lastLevel, out.level[v]);
    for (int v = 0; v < n; ++v) {
        if (!nodes[v].drivesOutput || nodes[v].hinted) continue;
        bool hasSucc = false;
        for (int e : outEdges[v]) if (!isBack[e]) hasSucc = true;
        if (!hasSucc) out.level[v] = lastLevel;
    }
    // ...and pull every node with no forward in-edge right up against its
    // nearest receiver, so a cone's inputs sit next to what they feed rather
    // than all stacked in column 0.
    for (auto it = topo.rbegin(); it != topo.rend(); ++it) {
        int v = *it;
        if (nodes[v].hinted) continue;
        bool hasPred = false;
        int  minSucc = INT_MAX;
        for (int e : inEdges[v])  if (!isBack[e]) hasPred = true;
        for (int e : outEdges[v]) if (!isBack[e]) minSucc = std::min(minSucc, out.level[edges[e].v]);
        if (!hasPred && minSucc != INT_MAX) out.level[v] = std::max(out.level[v], minSucc - 1);
    }

    int maxLevel = 0;
    for (int v = 0; v < n; ++v) maxLevel = std::max(maxLevel, out.level[v]);
    std::vector<std::vector<int>> layers(maxLevel + 1);
    for (int v = 0; v < n; ++v) layers[out.level[v]].push_back(v);

    // 3. Crossing reduction: barycenter sweeps, down then up. A neighbor
    //    counts at its order index plus the fraction of its height where
    //    the pin sits, so pin order on a tall box matters too.
    // Hinted nodes keep their top-to-bottom order: whatever order a layer
    // gets, they're put back in it in their hinted order, in the slots they
    // took (the others stay where the sweep put them).
    auto keepHintOrder = [&](std::vector<int>& layer) {
        if (!hinted) return;
        std::vector<size_t> slots;
        std::vector<int>    olds;
        for (size_t i = 0; i < layer.size(); ++i)
            if (nodes[layer[i]].hinted) { slots.push_back(i); olds.push_back(layer[i]); }
        std::stable_sort(olds.begin(), olds.end(),
                         [&](int a, int b) { return nodes[a].hintY < nodes[b].hintY; });
        for (size_t k = 0; k < slots.size(); ++k) layer[slots[k]] = olds[k];
    };
    for (auto& layer : layers) keepHintOrder(layer);
    std::vector<float> order(n, 0.f);
    auto renumber = [&] {
        for (auto& layer : layers)
            for (size_t i = 0; i < layer.size(); ++i) order[layer[i]] = float(i);
    };
    renumber();
    auto frac = [&](int node, float dy) { return nodes[node].h > 0.f ? dy / nodes[node].h : 0.5f; };
    std::vector<char> isSink(n, 0);
    for (int v = 0; v < n; ++v) {
        bool drives = nodes[v].drivesOutput;
        for (int e : outEdges[v]) if (!isBack[e]) drives = true;
        isSink[v] = !drives;
    }
    for (int sweep = 0; sweep < 4; ++sweep) {
        const bool down = sweep % 2 == 0;
        for (int li = 0; li <= maxLevel; ++li) {
            int l = down ? li : maxLevel - li;
            auto& layer = layers[l];
            std::vector<std::pair<float, int>> keyed;
            for (int v : layer) {
                float sum = 0.f; int cnt = 0;
                auto visit = [&](int other, float dOther) {
                    int lo = out.level[other];
                    if (down ? lo < l : lo > l) { sum += order[other] + frac(other, dOther); ++cnt; }
                };
                for (int e : inEdges[v])  visit(edges[e].u, edges[e].du);
                for (int e : outEdges[v]) visit(edges[e].v, edges[e].dv);
                keyed.push_back({ cnt ? sum / float(cnt) : order[v] + 0.5f, v });
            }
            // Ties between sinks (the loads of one pin): by rank.
            std::stable_sort(keyed.begin(), keyed.end(), [&](const auto& a, const auto& b) {
                if (a.first != b.first) return a.first < b.first;
                return isSink[a.second] && isSink[b.second] && nodes[a.second].rank < nodes[b.second].rank;
            });
            for (size_t i = 0; i < keyed.size(); ++i) layer[i] = keyed[i].second;
            keepHintOrder(layer);
            renumber();
        }
    }

    out.logicLevel = out.level;

    // 6 (before the columns are measured). Sink grid: a column with too
    //    many sinks is dealt into several, each a level of its own. Its
    //    sinks are banded by driver: each driver's sinks get rows of their
    //    own (a band, placed level with the driver in step 5) and are dealt
    //    into columns from the grid's first one, so every driver's grid
    //    starts in the same column and no net's wire has to cross another
    //    net's sinks to reach its own.
    struct Band { std::vector<std::vector<int>> cols; };
    struct SinkGrid { int first = 0, count = 0; std::vector<Band> bands; };
    std::vector<SinkGrid> grids;
    std::vector<int> gridOfLevel(maxLevel + 1, -1);
    if (maxSinkRows > 0) {
        std::vector<std::vector<int>> wrapped;
        for (const auto& layer : layers) {
            std::vector<int> sinks, others;
            for (int v : layer) {
                bool drives = nodes[v].drivesOutput;
                for (int e : outEdges[v]) if (!isBack[e]) drives = true;
                (drives ? others : sinks).push_back(v);
            }
            if (int(sinks.size()) <= maxSinkRows) { wrapped.push_back(layer); continue; }
            // A sink's band: its nearest driver (the highest level among its
            // forward in-edges, then the first in column order). Bands keep
            // the order their first sink has in the column.
            std::vector<int> bandKeys;
            std::vector<std::vector<int>> bandSinks;
            for (int v : sinks) {
                int key = -1, keyLevel = INT_MIN;
                for (int e : inEdges[v]) {
                    if (isBack[e]) continue;
                    const int u = edges[e].u;
                    if (key < 0 || out.level[u] > keyLevel || (out.level[u] == keyLevel && order[u] < order[key])) {
                        key = u;
                        keyLevel = out.level[u];
                    }
                }
                auto it = std::find(bandKeys.begin(), bandKeys.end(), key);
                if (it == bandKeys.end()) {
                    bandKeys.push_back(key);
                    bandSinks.emplace_back();
                    it = bandKeys.end() - 1;
                }
                bandSinks[size_t(it - bandKeys.begin())].push_back(v);
            }
            SinkGrid grid;
            grid.first = int(wrapped.size());
            for (const auto& bs : bandSinks) {
                const size_t cols = (bs.size() + size_t(maxSinkRows) - 1) / size_t(maxSinkRows);
                const size_t per  = (bs.size() + cols - 1) / cols;
                Band band;
                for (size_t c = 0; c * per < bs.size(); ++c)
                    band.cols.emplace_back(bs.begin() + long(c * per),
                                           bs.begin() + long(std::min(bs.size(), (c + 1) * per)));
                grid.count = std::max(grid.count, int(band.cols.size()));
                grid.bands.push_back(std::move(band));
            }
            for (int c = 0; c < grid.count; ++c) {
                std::vector<int> col;
                for (const auto& band : grid.bands)
                    if (c < int(band.cols.size())) col.insert(col.end(), band.cols[c].begin(), band.cols[c].end());
                wrapped.push_back(std::move(col));
            }
            grids.push_back(std::move(grid));
            if (!others.empty()) wrapped.push_back(others);
        }
        if (!grids.empty()) {
            layers = std::move(wrapped);
            maxLevel = int(layers.size()) - 1;
            for (int l = 0; l <= maxLevel; ++l)
                for (int v : layers[l]) out.level[v] = l;
            renumber();
            gridOfLevel.assign(maxLevel + 1, -1);
            for (int g = 0; g < int(grids.size()); ++g)
                for (int c = 0; c < grids[g].count; ++c) gridOfLevel[grids[g].first + c] = g;
        }
    }

    // 4. Columns: each as wide as its widest node, then a channel sized for
    //    the nets crossing it.
    std::vector<int> crossing(maxLevel + 1, 0);
    for (const auto& net : nets) {
        int lo = INT_MAX, hi = INT_MIN;
        for (const auto* pins : { &net.drivers, &net.receivers })
            for (const auto& p : *pins) {
                if (p.node < 0 || p.node >= n) continue;
                lo = std::min(lo, out.level[p.node]);
                hi = std::max(hi, out.level[p.node]);
            }
        for (int l = std::max(lo, 0); l < hi; ++l) ++crossing[l];
    }
    std::vector<float> colX(maxLevel + 1, kLeftMargin);
    for (int l = 0; l < maxLevel; ++l) {
        float colW = 0.f;
        for (int v : layers[l]) colW = std::max(colW, nodes[v].w);
        colX[l + 1] = colX[l] + colW + kChannelMin + kTrackPitch * float(crossing[l]);
    }

    // 5. Heights: start stacked, then alternate sweeps pulling each column
    //    towards pin-aligned heights with its already-placed neighbors.
    std::vector<float> y(n, 0.f);
    for (auto& layer : layers) {
        float cy = 0.f;
        for (int v : layer) { y[v] = nodes[v].hinted ? nodes[v].hintY : cy; cy += nodes[v].h + kRowSpacing; }
    }
    // A sink grid is placed as a whole: its bands top to bottom, each one's
    // rows level with its driver's pin as far as the bands above allow, the
    // rows of a band aligned across its columns.
    auto placeGrid = [&](const SinkGrid& grid) {
        float prevBottom = 0.f;
        bool  first = true;
        for (const auto& band : grid.bands) {
            size_t rows = 0;
            for (const auto& col : band.cols) rows = std::max(rows, col.size());
            std::vector<float> rowOff(rows, 0.f);
            float rowY = 0.f;
            for (size_t r = 0; r < rows; ++r) {
                float h = 0.f;
                for (const auto& col : band.cols) if (r < col.size()) h = std::max(h, nodes[col[r]].h);
                rowOff[r] = rowY;
                rowY += h + kRowSpacing;
            }
            const float height = rowY - kRowSpacing;
            std::vector<float> want;
            for (const auto& col : band.cols)
                for (size_t r = 0; r < col.size(); ++r)
                    for (int e : inEdges[col[r]])
                        if (!isBack[e] && out.level[edges[e].u] < grid.first)
                            want.push_back(y[edges[e].u] + edges[e].du - edges[e].dv - rowOff[r]);
            float top = want.empty() ? (first ? 0.f : prevBottom + kBandGap) : lowerMedian(want);
            if (!first) top = std::max(top, prevBottom + kBandGap);
            top = std::round(top / kGrid) * kGrid;
            for (const auto& col : band.cols)
                for (size_t r = 0; r < col.size(); ++r) y[col[r]] = top + rowOff[r];
            prevBottom = top + height;
            first = false;
        }
    };
    const int rounds[] = { +1, -1, +1 };
    for (int dir : rounds) {
        for (int li = 0; li <= maxLevel; ++li) {
            int l = dir > 0 ? li : maxLevel - li;
            if (gridOfLevel[l] >= 0) {
                const SinkGrid& grid = grids[size_t(gridOfLevel[l])];
                if (l == (dir > 0 ? grid.first : grid.first + grid.count - 1)) placeGrid(grid);
                continue;
            }
            auto& layer = layers[l];
            std::vector<float> target, weight, h;
            for (int v : layer) {
                std::vector<float> want;
                auto visit = [&](int other, float dOther, float dSelf) {
                    int lo = out.level[other];
                    if (dir > 0 ? lo < l : lo > l) want.push_back(y[other] + dOther - dSelf);
                };
                for (int e : inEdges[v])  visit(edges[e].u, edges[e].du, edges[e].dv);
                for (int e : outEdges[v]) visit(edges[e].v, edges[e].dv, edges[e].du);
                if (nodes[v].hinted) {
                    // Stays put; only a node squeezed in beside it moves it.
                    target.push_back(nodes[v].hintY);
                    weight.push_back(kHintWeight);
                } else {
                    target.push_back(want.empty() ? y[v] : lowerMedian(want));
                    weight.push_back(want.empty() ? 0.05f : float(want.size()));
                }
                h.push_back(nodes[v].h);
            }
            auto placed = packColumn(target, weight, h, kRowSpacing);
            for (size_t i = 0; i < layer.size(); ++i) y[layer[i]] = placed[i];
        }
    }

    float minY = 1e30f;
    for (int v = 0; v < n; ++v) minY = std::min(minY, y[v]);
    for (int v = 0; v < n; ++v)
        out.pos[v] = ImVec2(colX[out.level[v]], y[v] - minY);
    return out;
}

std::vector<float> stackPorts(const std::vector<float>& desiredY,
                              const std::vector<std::vector<size_t>>& blocks) {
    // Items: each block, then every port in none. An item is placed by its
    // top; a block's target is the top that best fits its ports'.
    std::vector<std::vector<size_t>> items;
    std::vector<bool> inBlock(desiredY.size(), false);
    for (const auto& b : blocks) {
        if (b.empty()) continue;
        items.push_back(b);
        for (size_t i : b) inBlock[i] = true;
    }
    for (size_t i = 0; i < desiredY.size(); ++i)
        if (!inBlock[i]) items.push_back({ i });
    std::vector<float> want(items.size());
    for (size_t k = 0; k < items.size(); ++k) {
        float sum = 0.f;
        for (size_t j = 0; j < items[k].size(); ++j) sum += desiredY[items[k][j]] - kPinPitch * float(j);
        want[k] = sum / float(items[k].size());
    }
    std::vector<size_t> idx(items.size());
    for (size_t k = 0; k < idx.size(); ++k) idx[k] = k;
    std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return want[a] < want[b]; });
    std::vector<float> target, weight, h;
    for (size_t k : idx) {
        target.push_back(want[k]);
        weight.push_back(float(items[k].size()));
        h.push_back(kPinPitch * float(items[k].size() - 1));
    }
    // Not snapped: a port lines up with its pin wherever that is.
    auto placed = packColumn(target, weight, h, kPinPitch, /*snap=*/false);
    std::vector<float> out(desiredY.size());
    for (size_t r = 0; r < idx.size(); ++r) {
        const auto& item = items[idx[r]];
        for (size_t j = 0; j < item.size(); ++j) out[item[j]] = placed[r] + kPinPitch * float(j);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Routing
// ---------------------------------------------------------------------------
namespace {
struct Channel { float x0, x1; };

std::vector<Channel> findChannels(const std::vector<RouteRect>& obstacles,
                                  const std::vector<RouteNet>& nets) {
    float pinMin = 1e30f, pinMax = -1e30f;
    for (const auto& net : nets) {
        pinMin = std::min(pinMin, net.driver.at.x); pinMax = std::max(pinMax, net.driver.at.x);
        for (const auto& r : net.receivers) {
            pinMin = std::min(pinMin, r.at.x); pinMax = std::max(pinMax, r.at.x);
        }
    }
    std::vector<std::pair<float, float>> spans;
    for (const auto& r : obstacles) spans.push_back({ r.x0, r.x1 });
    std::sort(spans.begin(), spans.end());
    std::vector<std::pair<float, float>> merged;
    for (const auto& s : spans) {
        if (!merged.empty() && s.first <= merged.back().second + 1.f)
            merged.back().second = std::max(merged.back().second, s.second);
        else
            merged.push_back(s);
    }
    std::vector<Channel> channels;
    if (merged.empty()) {
        channels.push_back({ pinMin - kChannelMin, pinMax + kChannelMin });
        return channels;
    }
    channels.push_back({ std::min(pinMin, merged.front().first - kChannelMin), merged.front().first });
    for (size_t i = 1; i < merged.size(); ++i)
        channels.push_back({ merged[i - 1].second, merged[i].first });
    channels.push_back({ merged.back().second, std::max(pinMax, merged.back().second + kChannelMin) });
    return channels;
}

// The channel a wire enters when it leaves `pin` on its side.
int channelOf(const std::vector<Channel>& channels, const RoutePin& pin) {
    constexpr float eps = 0.5f;
    if (pin.right) {
        for (int c = 0; c < int(channels.size()); ++c)
            if (channels[c].x1 > pin.at.x + eps) return c;
        return int(channels.size()) - 1;
    }
    for (int c = int(channels.size()) - 1; c >= 0; --c)
        if (channels[c].x0 < pin.at.x - eps) return c;
    return 0;
}

// Channel `c` where it runs from y0 to y1: widened over the spans bounding
// it to the nearest one level with that band (the first and last channels
// keep their outer side), never past the next channel.
Channel localChannel(const std::vector<Channel>& channels, const std::vector<RouteRect>& spans,
                     int c, float y0, float y1) {
    constexpr float clearance = 4.f, eps = 0.5f;
    Channel w = channels[size_t(c)];
    const float leftLimit  = c > 0 ? channels[size_t(c) - 1].x1 : w.x0;
    const float rightLimit = c + 1 < int(channels.size()) ? channels[size_t(c) + 1].x0 : w.x1;
    float x0 = leftLimit, x1 = rightLimit;
    for (const auto& r : spans) {
        if (r.y1 <= y0 - clearance || r.y0 >= y1 + clearance) continue;
        if (r.x1 <= w.x0 + eps && r.x1 > leftLimit - eps) x0 = std::max(x0, r.x1);
        if (r.x0 >= w.x1 - eps && r.x0 < rightLimit + eps) x1 = std::min(x1, r.x0);
    }
    return { std::min(x0, w.x0), std::max(x1, w.x1) };
}

// Something a spine must not run along: a box, a label, or another net's
// wire (net >= 0; a net's own wires don't block it).
struct Blocker {
    RouteRect r;
    int       net = -1;
};

bool horizontalIsFree(const std::vector<Blocker>& blockers, int net, float y, float xa, float xb) {
    constexpr float clearance = 4.f;
    for (const auto& b : blockers) {
        const auto& r = b.r;
        if (b.net >= 0 && b.net == net) continue;
        if (y > r.y0 - clearance && y < r.y1 + clearance && xb > r.x0 && xa < r.x1) return false;
    }
    return true;
}

// A wire's vertical run in one channel. Its horizontals attach on the
// channel's left side (from a pin or spine to the left, side -1) or right
// side (+1).
struct Trunk {
    int   net, channel;
    float y0, y1;
    std::vector<std::pair<float, int>> attach;   // (y, side)
    int   track = 0;
    float x = 0.f;
};

// Channel routing's vertical constraint: when A attaches on the left and B on
// the right at the same height, A's horizontal runs from the left edge to
// A's trunk and B's from B's trunk to the right edge -- they only stay apart
// (instead of overlapping into what looks like one wire) if A is left of B.
bool mustBeLeftOf(const Trunk& a, const Trunk& b) {
    for (const auto& [ya, sa] : a.attach)
        if (sa < 0)
            for (const auto& [yb, sb] : b.attach)
                if (sb > 0 && std::abs(ya - yb) < 0.5f * kGrid) return true;
    return false;
}

// Tracks for the trunks of one channel, left to right, respecting the
// vertical constraints (a cycle of them can't be met; its closing edge is
// dropped) and packing trunks that don't overlap vertically onto one track.
// Returns the number of tracks.
int assignTracks(std::vector<Trunk*>& ts) {
    const size_t n = ts.size();
    std::vector<std::vector<size_t>> before(n);   // before[j]: trunks that must be left of j
    std::vector<std::vector<size_t>> after(n);
    for (size_t i = 0; i < n; ++i)
        for (size_t j = 0; j < n; ++j)
            if (i != j && ts[i]->net != ts[j]->net && mustBeLeftOf(*ts[i], *ts[j])) {
                after[i].push_back(j);
                before[j].push_back(i);
            }
    // Rank = longest constraint chain ending here (Kahn; leftover cycle
    // members keep the rank they reached).
    std::vector<int> rank(n, 0), indeg(n, 0);
    for (size_t j = 0; j < n; ++j) indeg[j] = int(before[j].size());
    std::vector<size_t> queue;
    for (size_t i = 0; i < n; ++i) if (!indeg[i]) queue.push_back(i);
    std::vector<char> done(n, 0);
    for (size_t q = 0; q < queue.size(); ++q) {
        size_t i = queue[q];
        done[i] = 1;
        for (size_t j : after[i]) {
            rank[j] = std::max(rank[j], rank[i] + 1);
            if (--indeg[j] == 0) queue.push_back(j);
        }
    }
    std::vector<size_t> orderIdx(n);
    for (size_t i = 0; i < n; ++i) orderIdx[i] = i;
    std::stable_sort(orderIdx.begin(), orderIdx.end(), [&](size_t a, size_t b) {
        return rank[a] != rank[b] ? rank[a] < rank[b] : ts[a]->y0 < ts[b]->y0;
    });
    std::vector<std::vector<size_t>> tracks;
    for (size_t i : orderIdx) {
        size_t minTrack = 0;
        for (size_t p : before[i])
            if (done[p] || rank[p] < rank[i]) minTrack = std::max(minTrack, size_t(ts[p]->track) + 1);
        size_t k = minTrack;
        for (; k < tracks.size(); ++k) {
            bool clear = true;
            for (size_t o : tracks[k])
                if (ts[o]->y0 < ts[i]->y1 + kGrid && ts[i]->y0 < ts[o]->y1 + kGrid) { clear = false; break; }
            if (clear) break;
        }
        if (k >= tracks.size()) tracks.resize(k + 1);
        tracks[k].push_back(i);
        ts[i]->track = int(k);
    }
    return int(tracks.size());
}
} // namespace

std::vector<RoutedNet> routeNets(const std::vector<RouteNet>& nets,
                                 const std::vector<RouteRect>& obstacles,
                                 const std::vector<RouteRect>& keepOut,
                                 const std::vector<RouteRegion>& regions) {
    // A spine avoids boxes, labels, every other net's pin stubs (across the
    // stub's whole channel), and the spines chosen before it.
    std::vector<Blocker> spineBlockers;
    for (const auto* rects : { &obstacles, &keepOut })
        for (const auto& r : *rects) spineBlockers.push_back({ r, -1 });
    std::vector<RoutedNet> out(nets.size());
    if (nets.empty()) return out;
    // A walled frame's edges split the channels: a wire outside it keeps its
    // trunk outside, one inside, inside.
    std::vector<RouteRect> spans = obstacles;
    for (const auto& g : regions)
        if (g.wall)
            for (float x : { g.r.x0, g.r.x1 }) spans.push_back({ x - 0.5f, g.r.y0, x + 0.5f, g.r.y1 });
    const auto channels = findChannels(spans, nets);
    // A confined net's spine stays out of the frames it isn't inside.
    auto spineFree = [&](size_t ni, float y, float xa, float xb) {
        if (!horizontalIsFree(spineBlockers, int(ni), y, xa, xb)) return false;
        if (!nets[ni].confined) return true;
        constexpr float clearance = 4.f;
        for (int gi = 0; gi < int(regions.size()); ++gi) {
            const auto& r = regions[size_t(gi)].r;
            if (std::find(nets[ni].inside.begin(), nets[ni].inside.end(), gi) != nets[ni].inside.end()) continue;
            if (y > r.y0 - clearance && y < r.y1 + clearance && xb > r.x0 && xa < r.x1) return false;
        }
        return true;
    };
    for (size_t ni = 0; ni < nets.size(); ++ni) {
        auto block = [&](const RoutePin& p) {
            const Channel& c = channels[channelOf(channels, p)];
            spineBlockers.push_back({ { c.x0, p.at.y - 1.f, c.x1, p.at.y + 1.f }, int(ni) });
        };
        block(nets[ni].driver);
        for (const auto& r : nets[ni].receivers) block(r);
    }

    // Per net: driver channel, receivers grouped by channel, spine height.
    struct Plan {
        int                          driverChannel = 0;
        std::map<int, std::vector<size_t>> byChannel;   // channel -> receiver indices
        bool                         hasSpine = false;
        float                        spineY   = 0.f;
        std::map<int, int>           trunkOf;           // channel -> index in trunks
    };
    std::vector<Plan>  plans(nets.size());
    std::vector<Trunk> trunks;
    for (size_t ni = 0; ni < nets.size(); ++ni) {
        const auto& net = nets[ni];
        Plan& plan = plans[ni];
        plan.driverChannel = channelOf(channels, net.driver);
        for (size_t ri = 0; ri < net.receivers.size(); ++ri)
            plan.byChannel[channelOf(channels, net.receivers[ri])].push_back(ri);

        float xa = channels[plan.driverChannel].x0, xb = channels[plan.driverChannel].x1;
        for (const auto& [c, _] : plan.byChannel) {
            if (c == plan.driverChannel) continue;
            plan.hasSpine = true;
            xa = std::min(xa, channels[c].x0);
            xb = std::max(xb, channels[c].x1);
        }
        const float dy = net.driver.at.y;
        if (plan.hasSpine) {
            plan.spineY = dy;
            for (int k = 1; k <= 400 && !spineFree(ni, plan.spineY, xa, xb); ++k)
                plan.spineY = dy + (k % 2 ? 1.f : -1.f) * kGrid * float((k + 1) / 2);
            spineBlockers.push_back({ { xa, plan.spineY - 1.f, xb, plan.spineY + 1.f }, int(ni) });
        }

        // Trunk extents: the driver's channel spans the driver, the spine
        // and its own receivers; every other channel the spine and its
        // receivers.
        // A pin left of its channel (wire leaving rightwards) attaches on
        // the channel's left side; the spine attaches on the side facing
        // the channel it comes from or goes to.
        std::map<int, Trunk> byCh;
        auto attach = [&](int c, float y, int side) {
            auto it = byCh.find(c);
            if (it == byCh.end()) it = byCh.emplace(c, Trunk{ int(ni), c, y, y, {} }).first;
            it->second.y0 = std::min(it->second.y0, y);
            it->second.y1 = std::max(it->second.y1, y);
            it->second.attach.push_back({ y, side });
        };
        auto pinSide = [](const RoutePin& p) { return p.right ? -1 : +1; };
        attach(plan.driverChannel, dy, pinSide(net.driver));
        for (const auto& [c, rs] : plan.byChannel) {
            if (c != plan.driverChannel) {
                attach(plan.driverChannel, plan.spineY, c > plan.driverChannel ? +1 : -1);
                attach(c, plan.spineY, c > plan.driverChannel ? -1 : +1);
            }
            for (size_t ri : rs) attach(c, net.receivers[ri].at.y, pinSide(net.receivers[ri]));
        }
        for (auto& [c, t] : byCh) {
            plan.trunkOf[c] = int(trunks.size());
            trunks.push_back(std::move(t));
        }
    }

    // Tracks, spread evenly across each channel. A straight pass-through
    // (a zero-length trunk) takes part too: where its wire meets others'
    // stubs still depends on its position.
    //
    // A channel is a strip free of boxes over the whole sheet, so a box far
    // above or below can make it a sliver -- a leaf next to a module frame,
    // in the same column as the frame's contents. Where its trunks run, it
    // can be wider: over the spans that bound it, up to the first thing
    // level with them (never into the next channel). Trunks in one band
    // (overlapping vertically, chained) share that width, so their order
    // holds. A side against a box leaves room for the pin ticks drawn out
    // of it (a quarter of a narrow channel at most), so no trunk runs
    // under one.
    std::map<int, std::vector<Trunk*>> byChannel;
    for (auto& t : trunks) byChannel[t.channel].push_back(&t);
    for (auto& [c, ts] : byChannel) {
        const float n = float(assignTracks(ts));
        std::vector<Trunk*> sorted = ts;
        std::sort(sorted.begin(), sorted.end(), [](const Trunk* a, const Trunk* b) { return a->y0 < b->y0; });
        for (size_t i = 0; i < sorted.size();) {
            float y0 = sorted[i]->y0, y1 = sorted[i]->y1;
            size_t j = i + 1;
            for (; j < sorted.size() && sorted[j]->y0 < y1 + kGrid; ++j) y1 = std::max(y1, sorted[j]->y1);
            const Channel w = localChannel(channels, spans, c, y0, y1);
            const float d   = std::min(kPinTick, 0.25f * (w.x1 - w.x0));
            const float x0  = w.x0 + (c > 0 ? d : 0.f);
            const float x1  = w.x1 - (c + 1 < int(channels.size()) ? d : 0.f);
            for (size_t k = i; k < j; ++k)
                sorted[k]->x = x0 + (x1 - x0) * float(sorted[k]->track + 1) / (n + 1.f);
            i = j;
        }
    }

    // Segments, then junctions where three or more directions meet.
    for (size_t ni = 0; ni < nets.size(); ++ni) {
        const auto& net = nets[ni];
        const Plan& plan = plans[ni];
        auto& segs = out[ni].segments;
        auto add = [&](ImVec2 a, ImVec2 b) {
            if (std::abs(a.x - b.x) > 1e-3f || std::abs(a.y - b.y) > 1e-3f) segs.push_back({ a, b });
        };
        const Trunk& dt = trunks[plan.trunkOf.at(plan.driverChannel)];
        add(net.driver.at, ImVec2(dt.x, net.driver.at.y));
        float spineX0 = dt.x, spineX1 = dt.x;
        for (const auto& [c, ti] : plan.trunkOf) {
            const Trunk& t = trunks[ti];
            add(ImVec2(t.x, t.y0), ImVec2(t.x, t.y1));
            spineX0 = std::min(spineX0, t.x);
            spineX1 = std::max(spineX1, t.x);
        }
        if (plan.hasSpine) add(ImVec2(spineX0, plan.spineY), ImVec2(spineX1, plan.spineY));
        for (const auto& [c, rs] : plan.byChannel) {
            const Trunk& t = trunks[plan.trunkOf.at(c)];
            for (size_t ri : rs) add(ImVec2(t.x, net.receivers[ri].at.y), net.receivers[ri].at);
        }

        std::vector<ImVec2> points;
        for (const auto& s : segs) { points.push_back(s.a); points.push_back(s.b); }
        for (const auto& p : points) {
            bool left = false, right = false, up = false, down = false;
            for (const auto& s : segs) {
                const float tol = 0.5f;
                if (std::abs(s.a.y - s.b.y) < tol && std::abs(s.a.y - p.y) < tol) {
                    float lo = std::min(s.a.x, s.b.x), hi = std::max(s.a.x, s.b.x);
                    if (p.x > lo - tol && p.x < hi + tol) {
                        if (p.x > lo + tol) left  = true;
                        if (p.x < hi - tol) right = true;
                    }
                } else if (std::abs(s.a.x - s.b.x) < tol && std::abs(s.a.x - p.x) < tol) {
                    float lo = std::min(s.a.y, s.b.y), hi = std::max(s.a.y, s.b.y);
                    if (p.y > lo - tol && p.y < hi + tol) {
                        if (p.y > lo + tol) up   = true;
                        if (p.y < hi - tol) down = true;
                    }
                }
            }
            if (int(left) + int(right) + int(up) + int(down) < 3) continue;
            bool dup = false;
            for (const auto& j : out[ni].junctions)
                if (std::abs(j.x - p.x) < 0.5f && std::abs(j.y - p.y) < 0.5f) { dup = true; break; }
            if (!dup) out[ni].junctions.push_back(p);
        }
    }
    return out;
}
// ---------------------------------------------------------------------------
// Fanout summaries
// ---------------------------------------------------------------------------
namespace {
std::string baseName(const std::string& label) {
    const auto b = label.rfind('[');
    return b != std::string::npos && !label.empty() && label.back() == ']' ? label.substr(0, b) : label;
}
} // namespace

std::string fanoutRoleGroup(const Item& r) {
    const std::string& role = r.role;
    if (role == "clock" || (role.empty() && r.clock))       return "clock";
    if (role == "async_reset" || role == "sync_reset")     return "reset";
    if (role == "async_set" || role == "sync_set")         return "set";
    if (role == "enable" || role == "memory_write_enable") return "enable";
    if (role == "scan_input" || role == "scan_enable")     return "scan";
    if (role == "memory_read_address" || role == "memory_write_address") return "address";
    return "data";
}

FanoutPlan planFanout(const std::vector<FanoutNet>& nets, int maxSinks, FanoutGroupBy groupBy,
                      const std::set<InstancePath>& pinned, const FanoutExpandedFn& expanded) {
    FanoutPlan plan;
    plan.groupOf.resize(nets.size());
    for (size_t ni = 0; ni < nets.size(); ++ni) plan.groupOf[ni].assign(nets[ni].receivers.size(), -1);
    if (maxSinks <= 0) return plan;

    auto isSink = [](const Item& it) { return !it.isTerm; };
    std::set<InstancePath> shown = pinned;
    for (const auto& net : nets)
        for (const auto& d : net.drivers) if (!d.isTerm) shown.insert(d.path);
    // Nets with few enough sinks draw them all.
    std::vector<size_t> big;
    for (size_t ni = 0; ni < nets.size(); ++ni) {
        const auto& rs = nets[ni].receivers;
        const size_t n = size_t(std::count_if(rs.begin(), rs.end(), isSink));
        if (n <= size_t(maxSinks)) {
            for (const auto& r : rs) if (isSink(r)) shown.insert(r.path);
        } else {
            big.push_back(ni);
        }
    }
    // A big net draws the sinks already shown, then the first others up to
    // maxSinks; the rest are candidates for its groups.
    std::vector<std::vector<size_t>> candidates(nets.size());
    for (size_t ni : big) {
        const auto& rs = nets[ni].receivers;
        size_t drawn = 0;
        for (const auto& r : rs) if (isSink(r) && shown.count(r.path)) ++drawn;
        for (size_t ri = 0; ri < rs.size(); ++ri) {
            if (!isSink(rs[ri]) || shown.count(rs[ri].path)) continue;
            if (drawn < size_t(maxSinks)) { shown.insert(rs[ri].path); ++drawn; }
            else                           candidates[ni].push_back(ri);
        }
    }
    auto keyOf = [&](const Item& r) {
        FanoutGroupKey k;
        switch (groupBy) {
            case FanoutGroupBy::Model:
                k.text = r.pathModels.empty() ? "" : r.pathModels.back();
                break;
            case FanoutGroupBy::Hierarchy:
                k.module.assign(r.path.begin(), r.path.end() - (r.path.empty() ? 0 : 1));
                break;
            case FanoutGroupBy::Role:
                k.text = fanoutRoleGroup(r);
                break;
            case FanoutGroupBy::Pin:
                k.clock = r.clock;
                if (!r.clock) k.text = baseName(r.label);
                break;
        }
        return k;
    };
    // Group the candidates; an instance drawn for another reason (or in a
    // group expanded, or alone in its group) is drawn here too, which can
    // make it drawn on another net: repeat until nothing changes.
    std::vector<std::map<FanoutGroupKey, std::vector<size_t>>> byKey(nets.size());
    for (bool changed = true; changed;) {
        changed = false;
        for (size_t ni : big) {
            auto& groups = byKey[ni];
            groups.clear();
            for (size_t ri : candidates[ni]) {
                const Item& r = nets[ni].receivers[ri];
                if (!shown.count(r.path)) groups[keyOf(r)].push_back(ri);
            }
            for (auto& [key, members] : groups) {
                std::set<InstancePath> insts;
                for (size_t ri : members) insts.insert(nets[ni].receivers[ri].path);
                if (insts.size() >= 2 && !(expanded && expanded(ni, key))) continue;
                for (const auto& p : insts) changed |= shown.insert(p).second;
            }
        }
    }
    for (size_t ni : big) {
        for (const auto& [key, members] : byKey[ni]) {
            FanoutGroup g;
            g.net = ni;
            g.key = key;
            std::set<InstancePath> insts;
            for (size_t ri : members) {
                const Item& r = nets[ni].receivers[ri];
                if (shown.count(r.path)) continue;
                g.receivers.push_back(ri);
                insts.insert(r.path);
            }
            if (insts.size() < 2) continue;  // (drawn: see the loop above)
            g.count = insts.size();
            // The pin they share, if they do; the deepest module holding them all.
            const Item& first = nets[ni].receivers[g.receivers.front()];
            g.pin   = baseName(first.label);
            g.clock = true;
            size_t common = first.path.size() - 1;
            for (size_t ri : g.receivers) {
                const Item& r = nets[ni].receivers[ri];
                if (baseName(r.label) != g.pin) g.pin.clear();
                g.clock = g.clock && r.clock;
                size_t k = 0;
                while (k < common && k + 1 < r.path.size() && r.path[k] == first.path[k]) ++k;
                common = k;
            }
            if (g.pin.empty() && groupBy == FanoutGroupBy::Pin && g.clock) g.pin = "clock";
            if (g.pin.empty() && groupBy == FanoutGroupBy::Role)          g.pin = key.text;
            g.module.assign(first.path.begin(), first.path.begin() + long(common));
            g.moduleModels.assign(first.pathModels.begin(),
                                  first.pathModels.begin() + long(std::min(common, first.pathModels.size())));
            std::string what;
            switch (groupBy) {
                case FanoutGroupBy::Model:     what = key.text.empty() ? "cells" : key.text; break;
                case FanoutGroupBy::Hierarchy: what = key.module.empty() ? "(top)" : displayPath(key.module); break;
                case FanoutGroupBy::Role:      what = key.text; break;
                case FanoutGroupBy::Pin:       what = key.clock ? "clock" : key.text; break;
            }
            g.label = what + " \xC3\x97" + std::to_string(g.count);  // U+00D7 multiplication sign
            const int gi = int(plan.groups.size());
            for (size_t ri : g.receivers) plan.groupOf[ni][ri] = gi;
            plan.groups.push_back(std::move(g));
        }
    }
    return plan;
}

// ---------------------------------------------------------------------------
// Net segments
// ---------------------------------------------------------------------------
std::optional<NetSegments> splitNet(const NetGraph& graph, const std::vector<std::optional<size_t>>& endNodes,
                                    const std::vector<bool>& drives, const std::set<InstancePath>& frames) {
    const size_t n = graph.nodes.size();
    if (n == 0) return std::nullopt;
    for (const auto& node : endNodes)
        if (!node || *node >= n) return std::nullopt;

    // Segments: nodes joined by an assign, or by a pin no frame is drawn for.
    std::vector<size_t> parent(n);
    for (size_t i = 0; i < n; ++i) parent[i] = i;
    std::function<size_t(size_t)> find = [&](size_t i) { return parent[i] == i ? i : parent[i] = find(parent[i]); };
    std::vector<size_t> crossings;  // the port edges that are frame pins
    for (size_t e = 0; e < graph.edges.size(); ++e) {
        const NetGraphEdge& edge = graph.edges[e];
        if (edge.port && frames.count(edge.path)) crossings.push_back(e);
        else parent[find(edge.from)] = find(edge.to);
    }
    std::vector<size_t> crossing;  // a pin's two sides may still be one segment (a loop)
    for (size_t e : crossings)
        if (find(graph.edges[e].from) != find(graph.edges[e].to)) crossing.push_back(e);
    if (crossing.empty()) return std::nullopt;

    size_t root = 0;
    bool   hasDriver = false;
    for (size_t i = 0; i < endNodes.size() && !hasDriver; ++i)
        if (drives[i]) { root = find(*endNodes[i]); hasDriver = true; }
    if (!hasDriver) return std::nullopt;

    NetSegments out;
    std::map<size_t, size_t> segmentOf;  // segment root node -> index in out.segments
    auto segment = [&](size_t root) -> NetSegment& {
        auto [it, added] = segmentOf.emplace(root, out.segments.size());
        if (added) {
            NetSegment s;
            s.scope = graph.nodes[root].path;
            for (size_t i = 0; i < n; ++i)
                if (find(i) == root && graph.nodes[i].path.size() < s.scope.size()) s.scope = graph.nodes[i].path;
            out.segments.push_back(std::move(s));
        }
        return out.segments[it->second];
    };
    segment(root);

    // Walk out from the driver's segment, a frame pin at a time.
    std::vector<bool> used(graph.edges.size(), false);
    std::vector<size_t> queue{ root };
    std::set<size_t> reached{ root };
    for (size_t q = 0; q < queue.size(); ++q) {
        const size_t here = queue[q];
        for (size_t e : crossing) {
            if (used[e]) continue;
            const NetGraphEdge& edge = graph.edges[e];
            const size_t outer = find(edge.from), inner = find(edge.to);
            if (outer != here && inner != here) continue;
            used[e] = true;
            const bool entering = outer == here;
            const size_t there = entering ? inner : outer;
            FramePin pin;
            pin.frame     = edge.path;
            pin.termId    = edge.pin.child_id;
            pin.bit       = edge.pin.bit;
            pin.name      = edge.pin.getString();
            pin.direction = edge.pin.direction;
            pin.entering  = entering;
            const int pi = int(out.pins.size());
            out.pins.push_back(std::move(pin));
            // Here's face receives; the face beyond drives its segment --
            // unless that one's reached already (a loop): it receives there too.
            segment(here).receivers.push_back({ -1, pi, !entering });
            if (reached.insert(there).second) {
                NetSegment& beyond = segment(there);
                beyond.source = { -1, pi, entering };
                beyond.net    = graph.nodes[entering ? edge.to : edge.from].net;  // on the pin, that side
                queue.push_back(there);
            } else {
                segment(there).receivers.push_back({ -1, pi, entering });
            }
        }
    }
    // The ends, each on its segment: the first driver is the root's source.
    bool sourced = false;
    for (size_t i = 0; i < endNodes.size(); ++i) {
        const size_t s = find(*endNodes[i]);
        if (!reached.count(s)) return std::nullopt;  // not joined to the driver
        NetSegment& seg = segment(s);
        if (!sourced && drives[i] && s == root) {
            seg.source = { int(i), -1, false };
            seg.net    = graph.nodes[*endNodes[i]].net;  // the driver's own net
            sourced    = true;
            continue;
        }
        seg.receivers.push_back({ int(i), -1, false });
    }
    return out;
}

// ---------------------------------------------------------------------------
// Hierarchy grouping
// ---------------------------------------------------------------------------
namespace {
struct GroupNode {
    InstancePath path;
    std::string label;
    int         depth = 0;
    FrameRoom   room;
    std::optional<EmptyFrame> empty;   // drawn with nothing in it
    std::vector<std::unique_ptr<GroupNode>> groups;
    std::map<InstanceRef, GroupNode*>       groupByRef;   // by instance id
    std::vector<std::pair<InstanceShape*, int>> leaves;   // leaf, logic level
    // Filled by measureGroup().
    float  levelMin = 0.f, levelMax = 0.f, sortY = 0.f;
    float  w = 0.f, h = 0.f;
    ImVec2 rel{};                                  // top-left, relative to the parent frame
    std::vector<std::pair<InstanceShape*, ImVec2>> leafRel;
};

void measureGroup(GroupNode& node, bool isRoot, const NameWidthFn& nameWidth) {
    struct Elem { GroupNode* g; InstanceShape* leaf; float center, sortY, w, h; };
    std::vector<Elem> elems;
    node.levelMin = 1e9f; node.levelMax = -1e9f; node.sortY = 1e9f;
    for (auto& g : node.groups) {
        measureGroup(*g, false, nameWidth);
        elems.push_back({ g.get(), nullptr, 0.5f * (g->levelMin + g->levelMax), g->sortY, g->w, g->h });
        node.levelMin = std::min(node.levelMin, g->levelMin);
        node.levelMax = std::max(node.levelMax, g->levelMax);
        node.sortY    = std::min(node.sortY, g->sortY);
    }
    for (auto [leaf, level] : node.leaves) {
        // The frame already names the module: a leaf in it shows its own name.
        // (A fanout summary keeps its own label: its path is made up; so
        // does a box a host style labels.)
        if (!isRoot && !leaf->path.empty() && !leaf->isSummary && leaf->label.empty())
            leaf->label = displayName(leaf->path.back());
        float lv = float(level);
        elems.push_back({ nullptr, leaf, lv, leaf->y, footprintWidth(*leaf, nameWidth),
                          footprintHeight(*leaf, nameWidth) });
        node.levelMin = std::min(node.levelMin, lv);
        node.levelMax = std::max(node.levelMax, lv);
        node.sortY    = std::min(node.sortY, leaf->y);
    }
    if (elems.empty() && node.empty) {
        node.levelMin = node.levelMax = node.empty->level;
        node.sortY    = node.empty->y;
    }

    // Bucket into columns by half-level so a module spanning an odd number
    // of logic columns doesn't get forced into the same column as a leaf.
    std::map<long, std::vector<Elem*>> columns;
    for (auto& e : elems) columns[std::lround(e.center * 2.0f)].push_back(&e);

    const float pad    = isRoot ? 0.f : kGroupPad;
    const float padL   = pad + node.room.left;
    const float top    = isRoot ? 0.f : kGroupHeader;
    const float colGap = isRoot ? kChannelMin + 2.f * kGroupPad : kGroupColGap;
    // A sub-frame is raised by its header so its own leaves land at their
    // heights; when that would lift it above this frame's top, everything
    // here moves down together instead, keeping the alignment.
    float raise = 0.f;
    for (const auto& e : elems)
        if (e.g) raise = std::max(raise, kGroupHeader - (e.sortY - node.sortY));
    float x = padL, maxBottom = top;
    for (auto& [col, members] : columns) {
        std::stable_sort(members.begin(), members.end(),
                         [](const Elem* a, const Elem* b) { return a->sortY < b->sortY; });
        float y = top, colW = 0.f;
        for (auto* e : members) {
            // Keep the height the layered placement gave it (relative to the
            // frame's topmost content), so pins it aligned stay aligned,
            // unless the column above pushes it down.
            const float want = top + raise + (e->sortY - node.sortY) - (e->g ? kGroupHeader : 0.f);
            y = std::max(y, want);
            if (e->g) e->g->rel = ImVec2(x, y);
            else      node.leafRel.push_back({ e->leaf, ImVec2(x, y) });
            y   += e->h + kRowSpacing;
            colW = std::max(colW, e->w);
        }
        maxBottom = std::max(maxBottom, y - kRowSpacing);
        x += colW + colGap;
    }
    node.w = std::max(x - colGap + pad + node.room.right, padL + pad + node.room.right + 6.f * kGrid);
    node.h = std::max(maxBottom + pad, node.room.minH);
}

void placeGroup(const GroupNode& node, ImVec2 origin, bool isRoot,
                int& nextInstId, std::vector<HierFrame>& frames) {
    if (!isRoot) {
        HierFrame f;
        f.shape.id          = nextInstId++;
        f.shape.name        = node.label;
        f.shape.x           = origin.x;
        f.shape.y           = origin.y;
        f.shape.w           = node.w;
        f.shape.h           = node.h;
        f.shape.isHierGroup = true;
        f.shape.hierDepth   = node.depth;
        f.shape.path        = node.path;
        frames.push_back(std::move(f));
    }
    for (const auto& [leaf, rel] : node.leafRel) {
        leaf->x = origin.x + rel.x;
        leaf->y = origin.y + rel.y;
    }
    for (const auto& g : node.groups)
        placeGroup(*g, ImVec2(origin.x + g->rel.x, origin.y + g->rel.y), false, nextInstId, frames);
}
} // namespace

std::vector<HierFrame> layoutHierarchyGroups(const std::map<InstancePath, LeafHier>& leafHier,
                                             std::vector<InstanceShape>& instances,
                                             const std::map<InstancePath, int>& pathToInstId,
                                             int& nextInstId,
                                             const NameWidthFn& nameWidth,
                                             const std::map<InstancePath, FrameRoom>& room,
                                             const std::map<InstancePath, EmptyFrame>& empty) {
    std::vector<HierFrame> frames;
    bool anyNested = !empty.empty();
    for (const auto& [path, lh] : leafHier)
        if (lh.path.size() >= 2 && pathToInstId.count(path)) { anyNested = true; break; }
    if (!anyNested) return frames;

    auto findById = [&](int id) -> InstanceShape* {
        for (auto& s : instances) if (s.id == id) return &s;
        return nullptr;
    };

    GroupNode root;
    // The frame of each level of `path` down to `depth` levels, made as needed.
    auto descend = [&](const InstancePath& path, const std::vector<std::string>& models, size_t depth) {
        GroupNode* node = &root;
        for (size_t i = 0; i < depth; ++i) {
            const InstanceRef& seg = path[i];
            auto git = node->groupByRef.find(seg);
            if (git == node->groupByRef.end()) {
                auto child = std::make_unique<GroupNode>();
                child->path    = node->path;
                child->path.push_back(seg);
                child->depth   = node->depth + 1;
                const std::string model = i < models.size() ? models[i] : "";
                const std::string name = displayName(seg);
                child->label   = model.empty() ? name : name + " (" + model + ")";
                if (auto r = room.find(child->path); r != room.end()) child->room = r->second;
                git = node->groupByRef.emplace(seg, child.get()).first;
                node->groups.push_back(std::move(child));
            }
            node = git->second;
        }
        return node;
    };
    for (const auto& [path, lh] : leafHier) {
        auto kit = pathToInstId.find(path);
        if (kit == pathToInstId.end()) continue;
        InstanceShape* leaf = findById(kit->second);
        if (!leaf) continue;
        descend(lh.path, lh.pathModels, lh.path.size() - 1)->leaves.push_back({ leaf, lh.level });
    }
    for (const auto& [path, e] : empty) descend(path, e.pathModels, path.size())->empty = e;

    measureGroup(root, true, nameWidth);
    placeGroup(root, ImVec2(kLeftMargin, 0.f), true, nextInstId, frames);
    return frames;
}

std::set<InstancePath> framePaths(const std::map<InstancePath, LeafHier>& leafHier) {
    std::set<InstancePath> out;
    bool anyNested = false;
    for (const auto& [path, lh] : leafHier) {
        if (lh.path.size() >= 2) anyNested = true;
        for (size_t k = 1; k < lh.path.size(); ++k)
            out.insert(InstancePath(lh.path.begin(), lh.path.begin() + long(k)));
    }
    if (!anyNested) out.clear();
    return out;
}

} // namespace SchematicLayout

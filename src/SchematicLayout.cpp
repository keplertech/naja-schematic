// SchematicLayout.cpp — pure layout geometry for the equipotential schematic.
#include "SchematicLayout.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <memory>

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
        item.pathIds     = occ.pathIds;
        item.hasInstances = occ.has_instances;
        item.bitTermCount = occ.bit_term_count;
        item.sourceLoc    = occ.source_loc;
        item.path         = occ.path;
        item.pathModels   = occ.pathModels;
        item.direction = occ.term.direction;
        (occ.term.direction == Direction::Output ? drivers : receivers).push_back(std::move(item));
    }
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

bool isGateSymbol(PrimitiveType type) {
    switch (type) {
        case PrimitiveType::And:  case PrimitiveType::Nand:
        case PrimitiveType::Or:   case PrimitiveType::Nor:
        case PrimitiveType::Xor:  case PrimitiveType::Xnor:
        case PrimitiveType::Inv:  case PrimitiveType::Buf:
        case PrimitiveType::Assign:
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
                            type == PrimitiveType::Assign;
        const bool xorLike = type == PrimitiveType::Xor || type == PrimitiveType::Xnor;
        g.w = single ? 2.f * kPinPitch : snapUp(g.h + kGrid + (xorLike ? kGrid : 0.f), kGrid);
        return g;
    }
    // Box: pins run down from the top, names drawn inside next to them.
    const size_t rows = std::max(nL, nR);
    g.h = std::max(2.f * kPinPitch, kPinPitch * float(rows + 1));
    for (size_t i = 0; i < nL; ++i) g.leftY.push_back(kPinPitch * float(i + 1));
    for (size_t i = 0; i < nR; ++i) g.rightY.push_back(kPinPitch * float(i + 1));
    g.w = snapUp(std::max(6.f * kGrid, labelWidth(leftNames) + labelWidth(rightNames) + 3.f * kGrid), kGrid);
    return g;
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

Placement layeredPlacement(const std::vector<PlaceNode>& nodes, const std::vector<PlaceNet>& nets) {
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

    // 1. Cycle breaking: an edge to a node still on the DFS stack closes a
    //    loop (e.g. a flop's feedback) and is left out of the layering.
    std::vector<char> isBack(edges.size(), 0), state(n, 0);
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
    std::vector<int> indeg(n, 0), topo;
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
    // ...then pull every node with no forward in-edge right up against its
    // nearest receiver, so a cone's inputs sit next to what they feed rather
    // than all stacked in column 0.
    for (auto it = topo.rbegin(); it != topo.rend(); ++it) {
        int v = *it;
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
    std::vector<float> order(n, 0.f);
    auto renumber = [&] {
        for (auto& layer : layers)
            for (size_t i = 0; i < layer.size(); ++i) order[layer[i]] = float(i);
    };
    renumber();
    auto frac = [&](int node, float dy) { return nodes[node].h > 0.f ? dy / nodes[node].h : 0.5f; };
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
            std::stable_sort(keyed.begin(), keyed.end(),
                             [](const auto& a, const auto& b) { return a.first < b.first; });
            for (size_t i = 0; i < keyed.size(); ++i) layer[i] = keyed[i].second;
            renumber();
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
        for (int v : layer) { y[v] = cy; cy += nodes[v].h + kRowSpacing; }
    }
    const int rounds[] = { +1, -1, +1 };
    for (int dir : rounds) {
        for (int li = 0; li <= maxLevel; ++li) {
            int l = dir > 0 ? li : maxLevel - li;
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
                target.push_back(want.empty() ? y[v] : lowerMedian(want));
                weight.push_back(want.empty() ? 0.05f : float(want.size()));
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

std::vector<float> stackPorts(const std::vector<float>& desiredY) {
    std::vector<size_t> idx(desiredY.size());
    for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
    std::stable_sort(idx.begin(), idx.end(),
                     [&](size_t a, size_t b) { return desiredY[a] < desiredY[b]; });
    std::vector<float> target, weight(idx.size(), 1.f), h(idx.size(), 0.f);
    for (size_t i : idx) target.push_back(desiredY[i]);
    // Not snapped: a port lines up with its pin wherever that is.
    auto placed = packColumn(target, weight, h, kPinPitch, /*snap=*/false);
    std::vector<float> out(desiredY.size());
    for (size_t k = 0; k < idx.size(); ++k) out[idx[k]] = placed[k];
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
                                 const std::vector<RouteRect>& keepOut) {
    // A spine avoids boxes, labels, every other net's pin stubs (across the
    // stub's whole channel), and the spines chosen before it.
    std::vector<Blocker> spineBlockers;
    for (const auto* rects : { &obstacles, &keepOut })
        for (const auto& r : *rects) spineBlockers.push_back({ r, -1 });
    std::vector<RoutedNet> out(nets.size());
    if (nets.empty()) return out;
    const auto channels = findChannels(obstacles, nets);
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
            for (int k = 1; k <= 400 && !horizontalIsFree(spineBlockers, int(ni), plan.spineY, xa, xb); ++k)
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
    std::map<int, std::vector<Trunk*>> byChannel;
    for (auto& t : trunks) byChannel[t.channel].push_back(&t);
    for (auto& [c, ts] : byChannel) {
        const float n  = float(assignTracks(ts));
        const float x0 = channels[c].x0, w = channels[c].x1 - channels[c].x0;
        for (auto* t : ts) t->x = x0 + w * float(t->track + 1) / (n + 1.f);
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
// Hierarchy grouping
// ---------------------------------------------------------------------------
namespace {
struct GroupNode {
    InstancePath path;
    std::string label;
    int         depth = 0;
    std::vector<std::unique_ptr<GroupNode>> groups;
    std::map<InstanceRef, GroupNode*>       groupByRef;   // by instance id
    std::vector<std::pair<InstanceShape*, int>> leaves;   // leaf, logic level
    // Filled by measureGroup().
    float  levelMin = 0.f, levelMax = 0.f, sortY = 0.f;
    float  w = 0.f, h = 0.f;
    ImVec2 rel{};                                  // top-left, relative to the parent frame
    std::vector<std::pair<InstanceShape*, ImVec2>> leafRel;
};

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
    for (auto [leaf, level] : node.leaves) {
        float lv = float(level);
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
    const float colGap = isRoot ? kChannelMin + 2.f * kGroupPad : kGroupColGap;
    // A sub-frame is raised by its header so its own leaves land at their
    // heights; when that would lift it above this frame's top, everything
    // here moves down together instead, keeping the alignment.
    float raise = 0.f;
    for (const auto& e : elems)
        if (e.g) raise = std::max(raise, kGroupHeader - (e.sortY - node.sortY));
    float x = pad, maxBottom = top;
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
    node.w = std::max(x - colGap + pad, 2.f * pad + 6.f * kGrid);
    node.h = maxBottom + pad;
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
        if (!isRoot && !leaf->path.empty()) leaf->label = displayName(leaf->path.back());
    }
    for (const auto& g : node.groups)
        placeGroup(*g, ImVec2(origin.x + g->rel.x, origin.y + g->rel.y), false, nextInstId, frames);
}
} // namespace

std::vector<HierFrame> layoutHierarchyGroups(const std::map<InstancePath, LeafHier>& leafHier,
                                             std::vector<InstanceShape>& instances,
                                             const std::map<InstancePath, int>& pathToInstId,
                                             int& nextInstId) {
    std::vector<HierFrame> frames;
    bool anyNested = false;
    for (const auto& [path, lh] : leafHier)
        if (lh.path.size() >= 2 && pathToInstId.count(path)) { anyNested = true; break; }
    if (!anyNested) return frames;

    auto findById = [&](int id) -> InstanceShape* {
        for (auto& s : instances) if (s.id == id) return &s;
        return nullptr;
    };

    GroupNode root;
    for (const auto& [path, lh] : leafHier) {
        auto kit = pathToInstId.find(path);
        if (kit == pathToInstId.end()) continue;
        InstanceShape* leaf = findById(kit->second);
        if (!leaf) continue;
        GroupNode* node = &root;
        for (size_t i = 0; i + 1 < lh.path.size(); ++i) {
            const InstanceRef& seg = lh.path[i];
            auto git = node->groupByRef.find(seg);
            if (git == node->groupByRef.end()) {
                auto child = std::make_unique<GroupNode>();
                child->path    = node->path;
                child->path.push_back(seg);
                child->depth   = node->depth + 1;
                const std::string model = i < lh.pathModels.size() ? lh.pathModels[i] : "";
                const std::string name = displayName(seg);
                child->label   = model.empty() ? name : name + " (" + model + ")";
                git = node->groupByRef.emplace(seg, child.get()).first;
                node->groups.push_back(std::move(child));
            }
            node = git->second;
        }
        node->leaves.push_back({ leaf, lh.level });
    }

    measureGroup(root, true);
    placeGroup(root, ImVec2(kLeftMargin, 0.f), true, nextInstId, frames);
    return frames;
}

} // namespace SchematicLayout

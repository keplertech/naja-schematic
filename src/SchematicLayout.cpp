// SchematicLayout.cpp — pure layout geometry for the equipotential schematic.
#include "SchematicLayout.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace SchematicLayout {

// Extract the leaf segment from a slash-separated instance path.
// e.g. "top/sub/<assign:0>" → "<assign:0>"
static std::string leafSegment(const std::string& path) {
    auto pos = path.rfind('/');
    return (pos == std::string::npos) ? path : path.substr(pos + 1);
}

void buildItems(const Equipotential* eq,
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
        item.bitTermCount = occ.bit_term_count;
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

// ---------------------------------------------------------------------------
// IncrementalLayout
// ---------------------------------------------------------------------------
void IncrementalLayout::place(const Equipotential* eq) {
    if (laidOut_.count(eq)) return;
    laidOut_.insert(eq);

    std::vector<Item> drivers, receivers;
    buildItems(eq, drivers, receivers);
    if (drivers.empty() && receivers.empty()) return;

    // Find anchor: first non-term already placed
    const Item* anchor      = nullptr;
    bool        anchorDrives = false;

    for (const auto& item : drivers) {
        if (!item.isTerm && placed_.count(item.key()))
            { anchor = &item; anchorDrives = true; break; }
    }
    if (!anchor) {
        for (const auto& item : receivers) {
            if (!item.isTerm && placed_.count(item.key()))
                { anchor = &item; anchorDrives = false; break; }
        }
    }

    if (!anchor) {
        // First/independent net: two-column layout below existing content
        const float lx = kLeftMargin;
        const float rx = kLeftMargin + kInstW + kColGap;
        float dyl = nextY_, dyr = nextY_;
        for (const auto& item : drivers) {
            if (!item.isTerm) {
                placed_.emplace(item.key(), ImVec2{lx, dyl});
                dyl += kInstH + kRowSpacing;
            }
        }
        for (const auto& item : receivers) {
            if (!item.isTerm) {
                placed_.emplace(item.key(), ImVec2{rx, dyr});
                dyr += kInstH + kRowSpacing;
            }
        }
        nextY_ = std::max(dyl, dyr) + kNetVGap;
    } else {
        // Expansion: extend horizontally from anchor
        const ImVec2 ap   = placed_[anchor->key()];
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
            for (const auto& [key, p] : placed_)
                if (std::abs(p.x - x) < kInstW && std::abs(p.y - y) < kInstH + kRowSpacing / 2)
                    return false;
            return true;
        };
        for (const auto& item : items) {
            if (item.isTerm || placed_.count(item.key())) continue;
            while (!isFree(newX, dy)) dy += kInstH + kRowSpacing;
            placed_.emplace(item.key(), ImVec2{newX, dy});
            dy += kInstH + kRowSpacing;
        }
    }
}

void IncrementalLayout::resolveColumnOverlaps(std::vector<InstanceShape>& instances) {
    std::map<int, std::vector<InstanceShape*>> byColumn;
    for (auto& inst : instances)
        if (inst.w > 0.f) byColumn[std::lround(inst.x)].push_back(&inst);
    for (auto& [x, col] : byColumn) {
        std::sort(col.begin(), col.end(),
                  [](const InstanceShape* a, const InstanceShape* b) { return a->y < b->y; });
        float minY = -1e9f;
        for (auto* inst : col) {
            if (inst->y < minY) inst->y = minY;
            minY = inst->y + inst->h + kRowSpacing;
            placed_[inst->name] = ImVec2{inst->x, inst->y};
        }
    }
}

void IncrementalLayout::clear() {
    placed_.clear();
    laidOut_.clear();
    nextY_ = 0.f;
}

// ---------------------------------------------------------------------------
// Hierarchy grouping
// ---------------------------------------------------------------------------
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
        f.pathKey           = node.pathKey;
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

std::vector<HierFrame> layoutHierarchyGroups(const std::map<std::string, LeafHier>& leafHier,
                                             std::vector<InstanceShape>& instances,
                                             const std::map<std::string, int>& keyToInstId,
                                             int& nextInstId) {
    std::vector<HierFrame> frames;
    bool anyNested = false;
    for (const auto& [key, lh] : leafHier)
        if (lh.path.size() >= 2 && keyToInstId.count(key)) { anyNested = true; break; }
    if (!anyNested) return frames;

    auto findById = [&](int id) -> InstanceShape* {
        for (auto& s : instances) if (s.id == id) return &s;
        return nullptr;
    };

    GroupNode root;
    for (const auto& [key, lh] : leafHier) {
        auto kit = keyToInstId.find(key);
        if (kit == keyToInstId.end()) continue;
        InstanceShape* leaf = findById(kit->second);
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
    placeGroup(root, ImVec2(kLeftMargin, 0.f), true, nextInstId, frames);
    return frames;
}

} // namespace SchematicLayout

// SchematicLayout.h — pure layout geometry for the equipotential schematic.
//
// Everything here only computes world-space positions/sizes: no ImGui frame,
// no provider requests, no DiagnosisStore lookups. EquipotentialView drives
// it from renderSchematic() and adds the interactive/decorative parts, which
// keeps the geometry testable without a render backend (see
// tests/SchematicLayoutTest.cpp).
#pragma once

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>
#include <imgui.h>

#include "Types.h"

namespace SchematicLayout {

// ---------------------------------------------------------------------------
// Geometry constants (world units)
// ---------------------------------------------------------------------------
inline constexpr float kInstW      = 180.0f;
inline constexpr float kInstH      = 70.0f;
inline constexpr float kColGap     = 120.0f;
inline constexpr float kRowSpacing = 24.0f;
inline constexpr float kLeftMargin = 20.0f;
inline constexpr float kNetVGap    = 80.0f;

// Hierarchy grouping (module frames around traced leaves).
inline constexpr float kGroupHeader = 30.0f; // room for the frame's label
inline constexpr float kGroupPad    = 18.0f; // inner padding of a frame
inline constexpr float kGroupColGap = 90.0f; // gap between columns inside a frame

// ---------------------------------------------------------------------------
// One endpoint of an equipotential: a top-level term, or a pin on an instance
// occurrence. Drivers are top-level inputs and instance outputs; everything
// else is a receiver.
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
    // Total bit-term count of the instance's model, if known.
    std::optional<size_t>  bitTermCount;
    // RTL source location of the instance itself, if available.
    std::optional<SourceLoc> sourceLoc;
    // Instance occurrences only: per-segment instance names / model names of
    // the full hierarchical path (the last entry is the instance itself).
    std::vector<std::string> path;
    std::vector<std::string> pathModels;

    const std::string& key() const { return fullName.empty() ? label : fullName; }
};

void buildItems(const Equipotential* eq, std::vector<Item>& drivers, std::vector<Item>& receivers);

// ---------------------------------------------------------------------------
// Incremental layout: each equipotential is placed once, the first time it's
// seen, and its instances keep their position from then on (so the view
// doesn't reshuffle as nets are added). A net sharing an already-placed
// instance extends horizontally from it (new receivers to the right of a
// driver, new drivers to the left of a receiver); an unrelated net gets a
// fresh two-column block below everything placed so far.
// ---------------------------------------------------------------------------
class IncrementalLayout {
  public:
    // No-op if `eq` was already placed.
    void place(const Equipotential* eq);

    // Push boxes down within each column (same x) so none overlaps the one
    // above it -- place() reserves a fixed kInstH slot per instance, but a box
    // with many ports is drawn taller. Zero-width shapes (term stubs) are
    // ignored. The corrected positions are persisted so later place() calls
    // anchor on what's actually drawn.
    void resolveColumnOverlaps(std::vector<InstanceShape>& instances);

    void clear();

    // Instance key (slash-joined path) -> world top-left.
    const std::map<std::string, ImVec2>& positions() const { return placed_; }

  private:
    std::map<std::string, ImVec2>  placed_;
    std::set<const Equipotential*> laidOut_;
    float                          nextY_ = 0.f;
};

// ---------------------------------------------------------------------------
// Hierarchy grouping: nested frames for the hierarchical modules containing
// the displayed leaf instances. A driver trace (or any equipotential) spans
// leaf cells anywhere in the design; without this, they're shown as one flat
// sea of boxes and the module structure is lost.
//
// Each leaf keeps the logic column the incremental layout gave it (its x,
// i.e. its distance from the traced net), and the modules become a tree of
// frames laid out bottom-up: inside a frame, its own leaves and sub-frames
// are bucketed into columns by the (average) logic column of their contents,
// left to right, and stacked by their original vertical order. That keeps the
// left-to-right signal flow while guaranteeing frames nest cleanly and never
// overlap.
// ---------------------------------------------------------------------------
struct LeafHier {
    std::vector<std::string> path;        // full instance-name path, leaf last
    std::vector<std::string> pathModels;  // matching model names ("" if unknown)
};

struct HierFrame {
    InstanceShape shape;    // isHierGroup set, label "inst (Model)" in `name`
    std::string   pathKey;  // slash-joined instance path of the module
};

// Moves the leaves of `instances` (looked up through keyToInstId) into their
// module frames and returns the frames, parent first, with ids allocated from
// nextInstId. Leaves inside a frame get their `label` shortened to the leaf
// name. Returns an empty vector, leaving every shape untouched, when no
// displayed leaf sits below the top design (no hierarchy to show).
std::vector<HierFrame> layoutHierarchyGroups(const std::map<std::string, LeafHier>& leafHier,
                                             std::vector<InstanceShape>& instances,
                                             const std::map<std::string, int>& keyToInstId,
                                             int& nextInstId);

} // namespace SchematicLayout

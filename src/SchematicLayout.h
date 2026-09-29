// SchematicLayout.h — pure layout geometry for the equipotential schematic.
//
// Everything here only computes world-space positions/sizes: no ImGui frame,
// no provider requests, no DiagnosisStore lookups. EquipotentialView drives
// it from renderSchematic() and adds the interactive/decorative parts, which
// keeps the geometry testable without a render backend (see
// tests/SchematicLayoutTest.cpp).
#pragma once

#include <functional>
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
//
// Schematic sheet: every pin sits on a kGrid multiple, so a driver pin and
// the receiver pin placed level with it are joined by a straight wire.
// ---------------------------------------------------------------------------
inline constexpr float kGrid       = 10.0f;
inline constexpr float kPinPitch   = 20.0f;  // distance between neighboring pins
inline constexpr float kRowSpacing = 30.0f;  // vertical gap between boxes in a column
inline constexpr float kLeftMargin = 20.0f;
inline constexpr float kChannelMin = 50.0f;  // narrowest routing channel between columns
inline constexpr float kTrackPitch = 10.0f;  // extra channel width per vertical wire track
inline constexpr float kPortGap    = 50.0f;  // top-level ports -> nearest box

// Hierarchy grouping (module frames around traced leaves).
// Grid multiples, so leaves inside frames keep their pins on the grid.
inline constexpr float kGroupHeader = 40.0f; // room for the frame's label and a leaf's name
inline constexpr float kGroupPad    = 20.0f; // inner padding of a frame
inline constexpr float kGroupColGap = 70.0f; // gap between columns inside a frame

// ---------------------------------------------------------------------------
// Instance names. A symbol's name is drawn above it, left-aligned, and is
// part of the symbol's footprint: a column (flat, or inside a module frame)
// is as wide as its widest symbol or name, the sheet's edges clear the names,
// and names are routing obstacles. So a name never runs into another symbol,
// another name or a wire, at any zoom (the drawn name never gets wider in
// world units than at kNameFontSize). A name wider than kNameMaxW is
// shortened in the middle, keeping its start and its end.
// ---------------------------------------------------------------------------
inline constexpr float kNameFontSize = 13.0f;     // world-space size of a name
inline constexpr float kNameH        = kPinPitch; // the row a name takes above its symbol
inline constexpr float kNameMaxW     = 240.0f;

// World width of a string drawn at kNameFontSize. The view measures with its
// font; tests can pass any function.
using NameWidthFn = std::function<float(const std::string&)>;

// `name` fitted into maxW: whole if it fits, else its head and tail around
// "...". Empty when not even the ellipsis fits.
std::string shortenMiddle(const std::string& name, const NameWidthFn& width, float maxW);

// The name drawn above `s` (its label, else its name, shortened to
// kNameMaxW), or "" for shapes drawn without one (frames, top-level ports).
std::string shownName(const InstanceShape& s, const NameWidthFn& width);

// Width of `s`'s footprint: its symbol, or its shown name when wider.
float footprintWidth(const InstanceShape& s, const NameWidthFn& width);

// ---------------------------------------------------------------------------
// Symbols. A gate (AND/OR/XOR families, INV/BUF, assign) is drawn as its
// standard symbol, sized by its input count; anything else is a box sized to
// fit its pin names, which are drawn inside it. Pins sit on the kPinPitch
// grid: a gate's inputs are centered on its single output, a box's pins run
// down from the top on each side. Pin offsets are from the symbol's top edge.
// ---------------------------------------------------------------------------
struct SymbolGeometry {
    float              w = 0.f, h = 0.f;
    std::vector<float> leftY;   // one per left (input) pin, in order
    std::vector<float> rightY;  // one per right (output/inout) pin, in order
};

// True for the types drawn as a gate symbol rather than a box.
bool isGateSymbol(PrimitiveType type);

// A gate with more than one right-side pin can't be drawn as its symbol and
// falls back to a box.
SymbolGeometry symbolGeometry(PrimitiveType type,
                              const std::vector<std::string>& leftNames,
                              const std::vector<std::string>& rightNames);

// ---------------------------------------------------------------------------
// Layered placement (Sugiyama-style):
//   1. cycles are broken (DFS back edges are ignored for layering);
//   2. each instance gets a logic level: longest path from the sources, then
//      each source is pulled right next to its nearest receiver;
//   3. instances are ordered within each level by barycenter sweeps to cut
//      wire crossings (first-seen order breaks ties, so adding a net mostly
//      appends to the drawing);
//   4. levels become columns, with a routing channel between neighbors that
//      grows with the number of nets crossing it;
//   5. each column is placed vertically by isotonic regression towards the
//      heights that put an instance's pins level with the pins they connect
//      to (straight wires), keeping the column order and spacing.
// Pure geometry: the result depends only on the inputs, in their order.
// ---------------------------------------------------------------------------
struct PlaceNode {
    float w = 0.f, h = 0.f;
};
struct PlacePin {
    int   node = 0;
    float dy   = 0.f;   // pin offset from the node's top edge
};
struct PlaceNet {
    std::vector<PlacePin> drivers;
    std::vector<PlacePin> receivers;
};
struct Placement {
    std::vector<ImVec2> pos;    // top-left per node
    std::vector<int>    level;  // logic level per node (0 = leftmost column)
};
Placement layeredPlacement(const std::vector<PlaceNode>& nodes, const std::vector<PlaceNet>& nets);

// Stacks the top-level port flags of one side of the drawing: each wants the
// height of the pin it connects to (`desiredY`), and no two may be closer than
// kPinPitch. Returns the heights, in input order, as close to their desired
// ones as that spacing allows.
std::vector<float> stackPorts(const std::vector<float>& desiredY);

// ---------------------------------------------------------------------------
// Orthogonal net routing. Box rectangles are obstacles, and the vertical
// strips free of any box between the placed columns are routing channels.
// Each net is a tree:
//   - a vertical trunk in the channel next to the driver pin, and one in the
//     channel next to each group of receivers further away;
//   - a horizontal spine joining those trunks, at the driver's height when
//     that line is free, else at the nearest free height;
//   - a horizontal branch from each pin to its trunk.
// Trunks sharing a channel are spread over distinct tracks, so two nets never
// overlap on one vertical line, ordered so that two nets' horizontals at the
// same height never overlap either (channel routing's vertical constraints);
// trunks that don't overlap vertically share a track. A
// junction dot marks every point where three or more wire directions meet.
// ---------------------------------------------------------------------------
struct RouteRect {
    float x0 = 0.f, y0 = 0.f, x1 = 0.f, y1 = 0.f;
};
// The row `s`'s shown name takes above it, if it has one -- an obstacle
// like the symbol itself.
std::optional<RouteRect> nameRect(const InstanceShape& s, const NameWidthFn& width);
struct RoutePin {
    ImVec2 at;
    bool   right = true;   // the wire leaves the pin to the right (else left)
};
struct RouteNet {
    RoutePin              driver;
    std::vector<RoutePin> receivers;
};
struct RouteSegment {
    ImVec2 a, b;
};
struct RoutedNet {
    std::vector<RouteSegment> segments;
    std::vector<ImVec2>       junctions;
};
// `keepOut` areas (labels drawn outside boxes) aren't obstacles for channels
// or pin stubs, but a spine avoids running through them when it can.
std::vector<RoutedNet> routeNets(const std::vector<RouteNet>& nets,
                                 const std::vector<RouteRect>& obstacles,
                                 const std::vector<RouteRect>& keepOut = {});

// ---------------------------------------------------------------------------
// One endpoint of an equipotential: a top-level term, or a pin on an instance
// occurrence. Drivers are top-level inputs and instance outputs; everything
// else is a receiver.
// ---------------------------------------------------------------------------
struct Item {
    std::string            label;       // port/term name (for matching, port rendering)
    Direction              direction   = Direction::Inout;
    bool                   isTerm      = false;
    DesignRef              designRef{};
    PrimitiveType          primitiveType = PrimitiveType::Unknown;
    unsigned               termChildId = 0;
    std::optional<int>     termBit;
    bool                   clock = false;  // a sequential cell's clock pin
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
    // `path` is the instance's identity in the layout.
    InstancePath             path;
    std::vector<std::string> pathModels;
};

void buildItems(const Equipotential* eq, std::vector<Item>& drivers, std::vector<Item>& receivers);

// ---------------------------------------------------------------------------
// Hierarchy grouping: nested frames for the hierarchical modules containing
// the displayed leaf instances. A driver trace (or any equipotential) spans
// leaf cells anywhere in the design; without this, they're shown as one flat
// sea of boxes and the module structure is lost.
//
// Each leaf keeps the logic level the layered placement gave it, and the
// modules become a tree of
// frames laid out bottom-up: inside a frame, its own leaves and sub-frames
// are bucketed into columns by the (average) logic column of their contents,
// left to right, and stacked by their original vertical order. That keeps the
// left-to-right signal flow while guaranteeing frames nest cleanly and never
// overlap.
// ---------------------------------------------------------------------------
struct LeafHier {
    InstancePath             path;        // full instance-name path, leaf last
    std::vector<std::string> pathModels;  // matching model names ("" if unknown)
    int                      level = 0;   // logic level (layeredPlacement)
};

struct HierFrame {
    InstanceShape shape;    // isHierGroup set, label "inst (Model)" in `name`,
                            // the module's instance path in `path`
};

// Moves the leaves of `instances` (looked up through pathToInstId) into their
// module frames and returns the frames, parent first, with ids allocated from
// nextInstId. Leaves inside a frame get their `label` shortened to the leaf
// name; a column is as wide as its widest footprint (see footprintWidth).
// Returns an empty vector, leaving every shape untouched, when no displayed
// leaf sits below the top design (no hierarchy to show).
std::vector<HierFrame> layoutHierarchyGroups(const std::map<InstancePath, LeafHier>& leafHier,
                                             std::vector<InstanceShape>& instances,
                                             const std::map<InstancePath, int>& pathToInstId,
                                             int& nextInstId,
                                             const NameWidthFn& nameWidth);

} // namespace SchematicLayout

#pragma once

#include <string>
#include <optional>
#include <vector>

#include <nlohmann/json.hpp>
using json = nlohmann::json;

//
// --- API / JSON types (kept first) ---
//

struct DesignRef {
  int db_id;
  int library_id;
  int design_id;
};

// RTL source location for an elaborated object (naja's SNLRTLInfos), only
// populated for SystemVerilog-loaded designs today (see CLAUDE.md). Absent
// means "no source location available" -- not an error.
struct SourceLoc {
  std::string file;
  int line = 0;
  int endLine = 0;
  int column = 0;
  int endColumn = 0;
};

struct InstanceResponseJson {
  std::string name;
  unsigned child_id;
  std::string model_name;
  DesignRef design_ref;
  bool has_primitives;
  bool has_instances;
  bool has_terms;
  bool has_nets;
  std::optional<SourceLoc> source_loc;
};

struct InstancesResponseJson {
  bool found;
  int gui_id;
  std::vector<InstanceResponseJson> children;
};

enum class Direction {
  Input,
  Output,
  Inout
};

inline const char* toString(Direction dir) {
  switch (dir) {
    case Direction::Input:  return "Input";
    case Direction::Output: return "Output";
    case Direction::Inout:  return "Inout";
    default:                return "Unknown";
  }
}

struct BitTerm {
  std::string name;
  unsigned child_id;
  Direction direction;
  std::optional<int> bit;

  std::string getString() const {
    return name + (bit.has_value() ? ("[" + std::to_string(bit.value()) + "]") : "");
  }

  std::string getDebugString() const {
    return name + " (child_id: " + std::to_string(child_id) +
           ", direction: " + toString(direction) +
           (bit.has_value() ? ", bit: " + std::to_string(bit.value()) : "") + ")";
  }
};

using Path = std::vector<std::string>;

struct InstTermOccurrence {
  Path path;                       // instance names (display)
  std::vector<unsigned> pathIds;   // instance child_ids (used to send load_equipotential)
  std::vector<std::string> pathModels; // model name per path entry ("" when the provider omits it)
  BitTerm term;
  DesignRef designRef;             // model of the tail instance — used to fetch its full interface
  // True when the tail instance's own model has sub-instances worth showing
  // in a nested schematic — drives the hierarchy expand/collapse glyph.
  bool has_instances = false;
  // Total bit-term count of the tail instance's model (bus terms counted per
  // bit), if the provider sent it -- used to tell a fully-shown interface
  // from a partial one.
  std::optional<size_t> bit_term_count;
  // RTL source location of the tail instance itself, if available.
  std::optional<SourceLoc> source_loc;
};

struct TermResponseJson {
  std::string name;
  unsigned child_id;
  Direction direction;
  std::optional<int> msb;
  std::optional<int> lsb;
};

struct TermsResponseJson {
  bool found;
  int gui_id;
  std::vector<TermResponseJson> children;
};

// A net has no direction (unlike a term) -- it's just a signal, identified
// by name/bit rather than by the provider-specific numeric child_id a term
// carries for building load_equipotential requests (see NetlistTree's
// NetlistTreeNetNode -- nets don't offer a "Show Equipotential" action).
struct NetResponseJson {
  std::string name;
  std::optional<int> msb;
  std::optional<int> lsb;
};

struct NetsResponseJson {
  bool found;
  int gui_id;
  std::vector<NetResponseJson> children;
};

struct Equipotential {
  bool found;
  std::vector<BitTerm> terms;
  std::vector<InstTermOccurrence> occurrences;
};

// True when every endpoint (top-level term or instance pin) of `candidate` is
// already one of `shown`'s -- adding it to the view would only redraw wires
// already there. Covers exact duplicates (clicking a pin whose net is shown)
// and the partial nets of a driver trace (which list only the receivers the
// trace entered through) whose full net is already displayed.
bool equipotentialCovers(const Equipotential& shown, const Equipotential& candidate);

//
// --- Diagnosis overlay types ---
// A diagnosis_response annotates an already-loaded netlist with findings from
// an external AI/formal-verification loop (e.g. kepler-formal, naja-scope).
// See DiagnosisStore for how these are indexed and queried during rendering.
//

enum class DiagnosisKind {
  Instance,
  Net
};

// Ordered so "worse" severities compare greater (used to pick the worst
// severity among multiple diagnostics on the same instance/net).
enum class DiagnosisSeverity {
  Info    = 0,
  Warning = 1,
  Error   = 2
};

inline const char* toString(DiagnosisSeverity s) {
  switch (s) {
    case DiagnosisSeverity::Info:    return "Info";
    case DiagnosisSeverity::Warning: return "Warning";
    case DiagnosisSeverity::Error:   return "Error";
    default:                         return "Unknown";
  }
}

struct DiagnosisItem {
  DiagnosisKind             kind     = DiagnosisKind::Instance;
  std::vector<std::string>  path;              // instance-name path, root excluded; empty = top level
  std::string               terminal;          // pin/port base name (no bus-bit suffix); Kind::Net only
  DiagnosisSeverity          severity = DiagnosisSeverity::Info;
  std::string               message;
  std::string               source;            // e.g. "kepler-formal", "naja-scope"

  // Slash-joined instance path, matching NetlistTree::getPathKey() and
  // EquipotentialView's instance-item keys.
  std::string pathKey() const {
    std::string out;
    for (size_t i = 0; i < path.size(); ++i) {
      if (i) out += '/';
      out += path[i];
    }
    return out;
  }
};

//
// --- Properties overlay types ---
// A general name/value inspector for whatever object is currently selected
// (an instance or a term/pin). Unlike diagnosis_response, this is a
// request/response pair (get_properties -> properties_response), answered by
// both LocalSNLProvider (native) and najaeda_server.py (WASM/browser) the
// same way load_terms etc. are. The object is identified the same way
// DiagnosisItem identifies things: a slash-joined instance-name path (root
// excluded), not provider-specific numeric ids, so both backends resolve it
// by walking instance names from the top design.
//

struct PropertyItem {
  std::string name;
  std::string value;
};

struct PropertiesResponseJson {
  std::vector<PropertyItem> properties;
};

// Inverse of DiagnosisItem::pathKey(): splits a slash-joined instance-name
// path back into per-segment names. "" (root/top-level) yields an empty path.
inline std::vector<std::string> splitPathKey(const std::string& key) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= key.size()) {
    size_t slash = key.find('/', start);
    std::string seg = key.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
    if (!seg.empty()) out.push_back(seg);
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  return out;
}

//
// --- Renderer / UI types (kept separate from the JSON / API types above) ---
//

#include <imgui.h>

struct Port {
    int id = 0;
    std::string name;
    float lx = 0.0f;    // normalized local x (-0.5..0.5)
    float ly = 0.0f;    // normalized local y (-0.5..0.5)
    Direction direction = Direction::Inout; // logical direction for geometry
    bool isInput = false; // used by renderer to pick red/green
    ImU32 color = 0;      // optional explicit color override (0 == no override)
    // True when this pin represents multiple merged bus bits rather than a
    // single bit/scalar terminal — drives a distinct draw style and expands
    // the bus (instead of load_equipotential) on click.
    bool isBus = false;
    // True when this pin's net isn't in the view yet (only revealed by
    // expanding the instance's full interface): clicking it adds that net.
    // Drawn with an open-circle stub so it reads as "more to see here".
    bool open = false;
    // An open pin whose net has been requested but hasn't arrived yet.
    bool pending = false;
};

struct InstanceShape {
    int id = 0;
    std::string name;       // instance path (display label)
    // Optional display label overriding `name` on the box (e.g. just the
    // leaf name when a hierarchy frame around the box already shows the
    // rest of the path); `name` stays the identity key either way.
    std::string label;
    std::string modelName;  // gate/cell type — drives the icon dispatcher in drawInstance()
    float x = 0.0f;
    float y = 0.0f;  // world coords (top-left)
    float w = 100.0f;
    float h = 50.0f;  // size in world units
    // When true, only a subset of ports is shown (e.g. only those on the
    // current net).  The renderer draws a dashed border so the user knows
    // the instance can be expanded to reveal its full interface.
    bool partialInterface = false;
    // Severity color from DiagnosisStore::instanceColor(), 0 if unflagged.
    // Drawn as an extra outline so it doesn't fight partialInterface's dash.
    ImU32 diagOutline = 0;
    std::vector<Port> ports;

    // --- Hierarchy embedding (nested boxes) ---
    // True when this instance's model has sub-instances — draws the small
    // expand/collapse glyph (see hierToggleGlyphRect() below).
    bool hasChildren = false;
    // True when currently showing its internals nested inside this box
    // (drives "+" vs "-" on the glyph); its children are other entries in
    // the same flat SchematicView::instances vector with parentShapeId
    // pointing back at this shape's id.
    bool hierExpanded = false;
    // -1 = top-level box; otherwise the id of the InstanceShape this box is
    // nested inside of.
    int parentShapeId = -1;

    // --- Hierarchy grouping (driver traces) ---
    // True for a frame standing for a hierarchical module that encloses some
    // of the traced leaf instances (see EquipotentialView's hierarchy
    // grouping). Drawn as a translucent labeled frame *under* the nets rather
    // than an opaque box, has no ports, and is never a parentShapeId target:
    // the leaves it surrounds stay top-level shapes so the existing wiring/
    // hit-test code is unaffected.
    bool isHierGroup = false;
    // Nesting depth of a hierarchy group frame (1 = directly under the top
    // design), used to shade nested frames progressively.
    int  hierDepth = 0;
    // The instance (or module frame) selected in the viewer -- see
    // SelectionStore. Drawn with a selection outline.
    bool selected = false;
};

struct NetWire {
    int id = 0;
    int srcInstance = 0;
    int srcPortId = 0;
    int dstInstance = 0;
    int dstPortId = 0;
    // Nlview-style default: near-monochrome. Color is reserved for
    // highlighting (diagnosis severity, selection) via an explicit override
    // further down the pipeline -- see EquipotentialView.cpp's srcPort/
    // dstPort->color checks -- rather than being a per-net decoration.
    ImU32 color = IM_COL32(150,150,150,255);
    // True when this wire represents multiple merged bus-bit nets between
    // the same two (merged) pins — drawn thicker, with a diagonal bus slash.
    bool isBus = false;
    // -1 = a top-level net (drawn under all instances, as before). Otherwise
    // the id of the InstanceShape whose internals this net belongs to — drawn
    // right after that instance's own box so its opaque fill doesn't hide
    // wiring nested inside it, but before that instance's children so the
    // children still render on top.
    int containerShapeId = -1;

    // Best-effort display name for the underlying net (driver pin/port name,
    // or the InternalNet name for hierarchy-embedded nets), shown next to
    // the bus slash mark on a merged bus wire -- see SchematicView::drawNet.
    std::string netName;
};

// World-space point where a pin meets its box (and where its wire attaches):
// lx/ly are normalized -0.5..0.5 across the box, 0 at its center.
inline ImVec2 portAnchor(const InstanceShape& inst, const Port& port) {
    return ImVec2(inst.x + inst.w * (0.5f + port.lx),
                  inst.y + inst.h * (0.5f + port.ly));
}

// World-space rect of an instance's hierarchy expand/collapse glyph
// (a small square straddling the top-center of the box). Shared by
// SchematicView's draw code and EquipotentialView's click hit-test so the
// two never drift apart.
inline void hierToggleGlyphRect(const InstanceShape& inst,
                                float& x0, float& y0, float& x1, float& y1) {
    x0 = inst.x + inst.w * 0.5f - 8.0f;
    x1 = x0 + 16.0f;
    y0 = inst.y - 2.0f;
    y1 = y0 + 16.0f;
}

// True when a box is large enough to host the hierarchy toggle glyph
// (excludes zero-size term stubs and other tiny/degenerate shapes).
inline bool canShowHierToggle(const InstanceShape& inst) {
    return inst.hasChildren && inst.w >= 40.0f && inst.h >= 30.0f;
}

// Renderer-side term/occurrence types (used only by the UI renderer)
struct Term {
    Direction direction = Direction::Inout;
    std::string name;
    std::string getString() const { return name; }
};

struct Occurrence {
    std::vector<std::string> path; // instance path, last element is instance name
    Term term;
};

// Renderer-side equipotential (keeps renderer expectations separate from API types)
struct RenderEquipotential {
    std::vector<Term> terms;
    std::vector<Occurrence> occurrences;
};

/* JSON deserializers (declarations kept for project) */
void from_json(const json& j, DesignRef& d);
void from_json(const json& j, InstanceResponseJson& r);
void from_json(const json& j, InstancesResponseJson& r);
void from_json(const json& j, TermsResponseJson& r);
void from_json(const json& j, NetsResponseJson& r);
void from_json(const json& j, Equipotential& e);
void from_json(const json& j, DiagnosisItem& d);
void from_json(const json& j, PropertyItem& p);
void from_json(const json& j, PropertiesResponseJson& r);

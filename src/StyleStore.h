#pragma once

#include <optional>
#include <string>
#include <vector>

#include <imgui.h>
#include <nlohmann/json.hpp>

#include "Types.h"

// Host-pushed styling (style_response, native --style): colors, widths and
// labels the host puts on nets, instances and pins -- e.g. a report showing
// which nets a fix duplicated. Same static/global state pattern as
// DiagnosisStore: replaced by each style_response, cleared on every fresh
// root load (the host pushes it again after each root_response), queried by
// EquipotentialView while it builds the shapes, so the styling reaches
// whatever draws them -- the canvas, File > Export Schematic as SVG..., the
// headless exports.
//
// Matching, by the same addresses as diagnosis items: an instance by its
// `id_path` (preferred), else its name `path`; a pin by the instance
// containing it ([] = a top-level port) plus the term, by `term_id` when
// given, else by its `terminal` base name, and an optional `bit` (absent:
// every bit). A "net" item styles the whole net through that pin: every
// wire of an equipotential with the pin among its ends. Several items on
// the same object merge in order, each set field overriding the earlier
// ones'.
//
// Precedence, the same everywhere (canvas, SVG, headless): diagnosis colors
// > style colors > trace colors (a trace's own, the convergence color and
// the stripes of shared nets) > the default ink. A style's width and fill
// apply whatever the color. The selection outline and halo are drawn on
// top of all of them.
enum class StyleKind { Instance, Net, Term };

struct StyleAttrs {
  ImU32       color = 0;   // stroke: a net's wires, an instance's outline, a pin's tick
  float       width = 0;   // stroke width, world units (SVG px at 1x); 0 = default
  ImU32       fill  = 0;   // instances: body fill; alpha < 255 tints the default fill
  std::string label;       // instance: replaces its name; net: on its wire; pin: replaces its name

  bool empty() const { return color == 0 && width <= 0 && fill == 0 && label.empty(); }
  // Fields set in `over` replace these.
  void merge(const StyleAttrs& over);
};

struct StyleItem {
  StyleKind                            kind = StyleKind::Instance;
  // The instance (Instance) or the instance containing the pin (Net, Term),
  // top excluded: by IDs and/or names, as DiagnosisItem.
  std::vector<std::string>             path;
  std::optional<std::vector<unsigned>> idPath;
  std::string                          terminal;  // base name, no "[bit]"
  std::optional<unsigned>              termId;    // wins over `terminal`
  std::optional<int>                   bit;       // absent: every bit
  StyleAttrs                           attrs;
};

class StyleStore {
  public:
    // Replaces the current styling.
    static void setStyles(std::vector<StyleItem> items);
    static void clear();
    static const std::vector<StyleItem>& all();

    // A style_response's "items" (or a --style file: {"items": [...]} or a
    // bare array). An entry that names nothing or has a malformed color is
    // skipped, and reported in `errors` when given.
    static std::vector<StyleItem> parse(const nlohmann::json& j,
                                        std::vector<std::string>* errors = nullptr);

    static StyleAttrs instanceStyle(const InstancePath& path);
    // A pin: `bit` nullopt for a scalar term or a whole (merged) bus pin --
    // which only items without a bit match.
    static StyleAttrs termStyle(const InstancePath& path, const std::string& terminal,
                                std::optional<unsigned> termId, std::optional<int> bit);
    // The net with these ends: every Net item naming one of them, merged.
    static StyleAttrs netStyle(const Equipotential& eq);

    // "#rrggbb" (opaque) or "#rrggbbaa" -> a color; nullopt for anything else.
    static std::optional<ImU32> parseColor(const std::string& text);
};

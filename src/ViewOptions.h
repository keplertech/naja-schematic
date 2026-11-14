#pragma once

#include <string>

#include <nlohmann/json.hpp>

// How the schematic is drawn, as opposed to what it shows: the text on an
// instance, and how a net with many sinks is laid out. One global set
// (same static pattern as the stores), changed from the View menu, by the
// host's view_options push (Python: view_options=, the render CLIs' flags)
// or natively by the command line (--instance-label, --sink-rows,
// --max-sinks, --group-by). Not cleared by a root load: it's a viewer
// setting, not something about the design. EquipotentialView reads it
// while building the shapes, so it reaches every export.
enum class InstanceLabel {
  Name,   // the instance name, above the symbol (the default)
  Model,  // the cell model, under the symbol, and no name
  Both,   // the name above, the model under
};

// How the sinks a net has past max_sinks are gathered into summary boxes.
enum class FanoutGroupBy {
  Model,      // by cell model ("DFF x812")
  Hierarchy,  // by the module containing the sink ("u_core/u_regs x812")
  Role,       // by the receiving pin's naja role: "clock", "reset", "enable", ..., "data"
  Pin,        // by the receiving pin: "clock" for a clock pin, else its name
};

struct ViewOptions {
  InstanceLabel instanceLabel = InstanceLabel::Name;
  // A column holding more sink-only boxes (nothing drawn downstream of
  // them) than this is wrapped into a grid of several columns. 0: never.
  int sinkRows = 8;
  // A net with more instance sinks than this draws the first ones and
  // gathers the rest into summary boxes, grouped by `groupBy`, that expand
  // when clicked. Its driver, and a sink also shown for another reason
  // (it drives something drawn, it's selected or diagnosed), is always
  // drawn. 0: never.
  int maxSinks = 32;
  FanoutGroupBy groupBy = FanoutGroupBy::Model;
  // A box two or more visible traces go through is outlined in the
  // near-black convergence color (never one the host styles). false: no
  // such outline (shared wires still show the traces' stripes).
  bool convergenceOutline = true;

  // Export decorations: they change only exported drawings (SVG and PNG,
  // SchematicView::exportSvg()/exportPng()), never the canvas.
  // No "more pins" dots on a partial box, no hollow open-pin stubs.
  bool        hideUiGlyphs = false;
  // Text in a band above / below the drawing ("" = none; '\n' breaks lines).
  std::string title;
  std::string caption;
  // A swatch and label below the drawing for each visible trace, and each
  // net a host style labels.
  bool        legend = false;

  static ViewOptions& current();

  // Applies the keys a view_options message (or a native flag) sets --
  // "instance_label", "sink_rows", "max_sinks", "group_by",
  // "convergence_outline", "hide_ui_glyphs", "title", "caption", "legend"
  // -- leaving the others
  // as they are. False, changing nothing, if one is malformed.
  bool apply(const nlohmann::json& message, std::string& error);
  nlohmann::json toJson() const;
  // toJson() without the keys that don't move anything (export
  // decorations, convergence_outline): what the layout depends on.
  nlohmann::json layoutJson() const;

  static const char* name(InstanceLabel v);
  static const char* name(FanoutGroupBy v);
};

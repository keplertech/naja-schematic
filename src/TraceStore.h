#pragma once

#include <string>
#include <vector>

#include <imgui.h>

#include "Types.h"

// The driver traces currently in the schematic -- same static/global state
// pattern as SelectionStore/DiagnosisStore. A trace is registered when its
// trace_driver request is sent (the id travels as the request's trace_id and
// comes back in the trace_driver_response), and the nets of the reply are
// tagged with it (Equipotential::traceIds) so the view can tell the traces
// apart: each draws in its own color, and wires/instances reached by two or
// more visible traces are drawn as convergence points.
//
// Colors are a highlight, not a finding: a diagnosis severity color always
// wins over a trace color (see EquipotentialView), and the palette stays
// clear of the severity red/amber/blue.
struct Trace {
  int         id = 0;
  std::string label;       // the pin the trace started from
  int         style = 0;   // index into the palette
  bool        visible = true;
};

// A trace's name in the UI: the pin it starts from, e.g. "u1/u2/A[3]", or
// just the port name for a top-level port. Display only, never parsed.
std::string traceLabel(const InstancePath& instance, const std::string& pin);

class TraceStore {
  public:
    static constexpr int kStyleCount = 4;

    // Registers a new trace and returns its id (> 0). It takes the first
    // palette style no current trace uses.
    static int begin(const std::string& label);
    // Forgets one trace, or all of them (the view was cleared).
    static void remove(int id);
    static void clear();

    static const std::vector<Trace>& traces();
    static const Trace* find(int id);
    static bool isVisible(int id);
    static void setVisible(int id, bool visible);
    // Moves the trace to the next palette color.
    static void cycleStyle(int id);

    static ImU32 color(int id);
    static ImU32 styleColor(int style);
    // Wires and instances two or more visible traces go through.
    static ImU32 convergenceColor();
};

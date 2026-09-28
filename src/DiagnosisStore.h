#pragma once

#include <string>
#include <vector>

#include <imgui.h>

#include "Types.h"

// Global store for AI-diagnosis annotations (kepler-formal, naja-scope, ...)
// overlaid on the currently loaded netlist.
//
// Static/global state, following the same pattern as EquipotentialView: set
// once per diagnosis_response (or "Load Diagnosis JSON..."), queried by
// NetlistTree and EquipotentialView during rendering so they don't need a
// GUIData/provider pointer threaded through just for this.
class DiagnosisStore {
  public:
    // Replaces the current diagnosis set (a fresh diagnosis run).
    static void setDiagnostics(std::vector<DiagnosisItem> items);
    static void clear();
    static const std::vector<DiagnosisItem>& all();

    // path: instance names, top excluded ({} == top level), as in
    // DiagnosisItem::path, NetlistTree::getInstancePath() and
    // InstanceShape::path.
    static std::vector<const DiagnosisItem*> instanceDiagnostics(const InstancePath& path);

    // terminal: pin/port base name, no bus-bit suffix (e.g. "Q", not "Q[3]").
    static std::vector<const DiagnosisItem*> netDiagnostics(const InstancePath& path,
                                                             const std::string& terminal);

    // Convenience: color for the worst severity at this path, 0 if unflagged.
    static ImU32 instanceColor(const InstancePath& path);
    static ImU32 netColor(const InstancePath& path, const std::string& terminal);

    static ImU32 colorForSeverity(DiagnosisSeverity sev);
};

#include "GUIData.h"

#include <algorithm>

#include "TraceStore.h"
#include "Types.h"

std::string GUIData::getString() const {
  std::string result = "GUIData:\n";
  result += netlist_ ? "  NetlistTree: loaded\n" : "  NetlistTree: null\n";
  result += "  Equipotentials: " + std::to_string(equipotentials_.size()) + "\n";
  return result;
}

static void addOwner(Equipotential& eq, int traceId) {
  if (traceId == 0) {
    eq.direct = true;
  } else if (std::find(eq.traceIds.begin(), eq.traceIds.end(), traceId) == eq.traceIds.end()) {
    eq.traceIds.push_back(traceId);
  }
}

bool GUIData::addEquipotential(Equipotential* eq, int traceId) {
  for (auto* shown : equipotentials_) {
    if (equipotentialCovers(*shown, *eq)) {
      addOwner(*shown, traceId);
      delete eq;
      return false;
    }
  }
  eq->traceIds.clear();
  eq->direct = false;
  addOwner(*eq, traceId);
  equipotentials_.push_back(eq);
  return true;
}

void GUIData::removeTrace(int traceId) {
  std::vector<Equipotential*> kept;
  for (auto* eq : equipotentials_) {
    auto& ids = eq->traceIds;
    ids.erase(std::remove(ids.begin(), ids.end(), traceId), ids.end());
    if (ids.empty() && !eq->direct) delete eq;
    else                            kept.push_back(eq);
  }
  equipotentials_ = std::move(kept);
  TraceStore::remove(traceId);
}

void GUIData::clearEquipotentials() {
  for (auto* eq : equipotentials_) delete eq;
  equipotentials_.clear();
  TraceStore::clear();
}

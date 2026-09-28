#include "GUIData.h"
#include "Types.h"

std::string GUIData::getString() const {
  std::string result = "GUIData:\n";
  result += netlist_ ? "  NetlistTree: loaded\n" : "  NetlistTree: null\n";
  result += "  Equipotentials: " + std::to_string(equipotentials_.size()) + "\n";
  return result;
}

bool GUIData::addEquipotential(Equipotential* eq) {
  for (const auto* shown : equipotentials_) {
    if (equipotentialCovers(*shown, *eq)) {
      delete eq;
      return false;
    }
  }
  equipotentials_.push_back(eq);
  return true;
}

void GUIData::clearEquipotentials() {
  for (auto* eq : equipotentials_) delete eq;
  equipotentials_.clear();
}

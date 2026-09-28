#pragma once

#include <string>
#include <vector>

class NetlistTree;
struct Equipotential;

struct GUIData {
  public:
    std::string getString() const;

    // Takes ownership. Returns false (and deletes `eq`) when an equipotential
    // already shown covers all of its endpoints -- see equipotentialCovers().
    bool addEquipotential(Equipotential* eq);
    void clearEquipotentials();

    NetlistTree*                  netlist_       {nullptr};
    std::vector<Equipotential*>   equipotentials_;
};

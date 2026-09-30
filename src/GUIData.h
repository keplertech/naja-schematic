#pragma once

#include <string>
#include <vector>

class NetlistTree;
struct Equipotential;

struct GUIData {
  public:
    std::string getString() const;

    // Takes ownership. `traceId` is the driver trace (TraceStore id) the net
    // belongs to, 0 when it's shown on its own. Returns false (and deletes
    // `eq`) when an equipotential already shown covers all of its endpoints
    // -- see equipotentialCovers() -- after recording the new owner on that
    // one, so a net two traces share knows it's on both.
    bool addEquipotential(Equipotential* eq, int traceId = 0);
    // Takes one trace out of the view: its nets that nothing else still
    // shows are removed, and it's dropped from TraceStore.
    void removeTrace(int traceId);
    // Clears the view, traces included.
    void clearEquipotentials();

    NetlistTree*                  netlist_       {nullptr};
    std::vector<Equipotential*>   equipotentials_;
};

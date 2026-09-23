#pragma once

#include <string>
#include <vector>
#include "Types.h"

class Equipotential;
class INetlistProvider;

class EquipotentialView {
  public:
    // A single port from a fully-loaded instance interface.
    struct ExpandedPort {
      std::string          name;
      Direction            direction = Direction::Inout;
      unsigned             childId   = 0;      // term child_id on the model
      std::optional<int>   bit;                // set for bus bits
    };

    // --- Hierarchy embedding (nested boxes) ---
    // One sub-instance one level inside an expanded instance's model.
    struct InstanceChild {
      std::string name;
      unsigned    childId = 0;
      DesignRef   designRef{};
      bool        hasInstances = false;  // can itself be expanded further
    };
    // One endpoint of an internal net: a bit term on a specific sub-instance
    // (instChildId set), or the parent instance's own boundary port
    // (instChildId absent).
    struct InternalPin {
      std::optional<unsigned> instChildId;
      std::string             name;
      Direction                direction = Direction::Inout;
      std::optional<int>       bit;
    };
    struct InternalNet {
      std::string               name;
      std::optional<int>        bit;
      std::vector<InternalPin>  pins;
    };
    struct InstanceInternals {
      std::vector<InstanceChild> children;
      std::vector<InternalNet>   nets;
    };

    static void renderSchematic(const std::vector<Equipotential*>& equipotentials);
    static void renderTable(const std::vector<Equipotential*>& equipotentials);
    static void zoomIn();
    static void zoomOut();
    static void fitView();
    static void clearNets();
    // Returns true (and resets the flag) if the canvas right-click requested a clear.
    static bool takePendingClear();
    // Clears persistent layout state immediately (no deferred flag).
    // Use this when the equipotentials vector is also being cleared synchronously.
    static void resetLayout();

    // Hierarchy grouping: draw the hierarchical modules containing the
    // displayed leaf instances as nested frames around them (default on).
    static bool showHierarchy();
    static void setShowHierarchy(bool on);

    // Called once during app setup so the view can send expansion requests.
    static void setProvider(INetlistProvider* provider);

    // Called by AppLogic when an expanded_instance_terms response arrives.
    static void applyInstanceExpansion(const std::string& pathKey,
                                       const std::vector<ExpandedPort>& ports);

    // Called by AppLogic when an instance_internals_response arrives.
    static void applyInstanceInternals(const std::string& pathKey,
                                       const InstanceInternals& data);
};

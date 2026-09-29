#pragma once

#include <string>
#include <vector>
#include "Types.h"

class Equipotential;
class INetlistProvider;
class SchematicView;

class EquipotentialView {
  public:
    // A single port from a fully-loaded instance interface.
    struct ExpandedPort {
      std::string          name;
      Direction            direction = Direction::Inout;
      unsigned             childId   = 0;      // term child_id on the model
      std::optional<int>   bit;                // set for bus bits
      bool                 clock = false;      // a sequential cell's clock pin
    };

    // --- Hierarchy embedding (nested boxes) ---
    // One sub-instance one level inside an expanded instance's model.
    struct InstanceChild {
      std::string name;
      unsigned    childId = 0;
      DesignRef   designRef{};
      PrimitiveType primitiveType = PrimitiveType::Unknown;
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

    // One instance to draw on its own, with its full interface and no net
    // yet -- a starting point to extend pin by pin (see showInstance()).
    struct StartInstance {
      std::vector<std::string> path;        // instance names, top excluded
      std::vector<unsigned>    pathIds;     // matching child_ids
      std::vector<std::string> pathModels;  // matching model names
      DesignRef                designRef{}; // the instance's model
      PrimitiveType            primitiveType = PrimitiveType::Unknown;
      bool                     hasInstances = false;
      std::optional<SourceLoc> sourceLoc;
      std::vector<ExpandedPort> ports;      // every pin, bus bits expanded
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
    static void applyInstanceExpansion(const InstancePath& path,
                                       const std::vector<ExpandedPort>& ports);

    // Adds `start` to the view as a lone box showing all its pins, each
    // "open" until its net is loaded. Call resetLayout() first to start a
    // fresh view from it (as a host's focus_instance does).
    static void showInstance(const StartInstance& start);
    // The StartInstance described by an instance_resolved reply, or nullopt
    // when it has none (not found, or the top design).
    static std::optional<StartInstance> startInstanceFromResolved(const json& reply);

    // Called by AppLogic when an instance_internals_response arrives.
    static void applyInstanceInternals(const InstancePath& path,
                                       const InstanceInternals& data);

    // Test hooks (read-only): the geometry the last renderSchematic() drew,
    // and the screen position of its canvas's top-left corner -- enough for a
    // headless ImGui test to turn a pin's world position into a mouse
    // position (see tests/EquipotentialViewTest.cpp).
    static const SchematicView& schematicForTesting();
    static ImVec2 canvasOriginForTesting();
};

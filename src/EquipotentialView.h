#pragma once

#include <optional>
#include <string>
#include <utility>
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
      // The host found nothing else on its net ("unconnected": true): drawn
      // with the no-connect cross at once (markUnconnected()).
      bool                 unconnected = false;
    };

    // One instance to draw on its own, with its full interface and no net
    // yet -- a starting point to extend pin by pin (see showInstance()).
    struct StartInstance {
      InstancePath             path;        // top excluded
      std::vector<unsigned>    pathIds;     // pathIds(path)
      std::vector<std::string> pathModels;  // matching model names
      DesignRef                designRef{}; // the instance's model
      PrimitiveType            primitiveType = PrimitiveType::Unknown;
      bool                     hasInstances = false;
      bool                     hasSources = false;
      std::vector<ExpandedPort> ports;      // every pin, bus bits expanded
    };

    static void renderSchematic(const std::vector<Equipotential*>& equipotentials);
    static void renderTable(const std::vector<Equipotential*>& equipotentials);
    static void zoomIn();
    static void zoomOut();
    static void fitView();
    // The schematic as drawn, as a standalone SVG document ("" if empty).
    static std::string exportSvg();
    static std::string exportPng(float scale);
    static bool hasSchematic();
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

    // The top design as a sheet (load_top_view): its ports on the sheet's
    // edges, each open until its net is loaded, and -- when the top has no
    // more than kMaxTopViewInstances instances -- its contents, every
    // top-level net, which the reply's handler adds as equipotentials.
    // `contents` false asks for the ports only; `automatic` marks the
    // request made on load, whose reply only draws into an empty view.
    static constexpr unsigned kMaxTopViewInstances = 250;
    static void requestTopView(bool contents, bool automatic);
    // Adds the top's ports as flags, each open until a net reaches it.
    // Call resetLayout() first to start a fresh view from them.
    static void showTopPorts(const std::vector<ExpandedPort>& ports);
    // True when the view shows nothing on its own: no instance shown alone
    // (showInstance()), no top port flag (showTopPorts()).
    static bool hasStartShapes();
    // A terms list (instance_resolved's, top_view_response's "ports") as
    // pins: bus bits expanded, labels displayable.
    static std::vector<ExpandedPort> portsFromJson(const json& terms);

    // An object to bring into view (a review thread's): an instance, or one
    // of its pins (a term, and a bus bit if `bit` is set; `path` {} = a
    // top-level port).
    struct FocusTarget {
      InstancePath            path;
      std::optional<unsigned> termId;
      std::optional<int>      bit;
    };
    // Pans to the target if it's drawn, as little as possible, and
    // highlights a pin for a moment. Otherwise adds it to the view, keeping
    // what's shown: an instance as a lone box (resolve_instance, see
    // takeAddOnResolve()), a pin by loading its net (an instance's term
    // without a bit, maybe a whole bus: its instance); then pans to it once
    // it arrives.
    static void focusObject(const FocusTarget& target);
    // True once for an instance_resolved that focusObject() asked for: the
    // instance is added to the view rather than replacing it.
    static bool takeAddOnResolve(const InstancePath& path);
    // The next instance_resolved for `path` adds the instance to the view
    // rather than replacing it (a host's focus_instance with "add": true).
    static void addOnResolve(const InstancePath& path);

    // The host found this pin and nothing else on its net (no net, or a net
    // with only it): it's drawn as not connected (a no-connect cross) and
    // clicking it asks nothing more. `pathIds` {} = a top-level port.
    static void markUnconnected(const std::vector<unsigned>& pathIds, unsigned termId,
                                std::optional<int> bit);
    // For an equipotential_response: when the host found the pin it echoes
    // ("found", "start") and nothing on its net but that pin, marks it
    // unconnected and returns true.
    static bool noteUnconnectedReply(const json& reply, const Equipotential& eq);

    // Opening a hierarchical instance (its box's "+"): it's drawn as its
    // contents -- its model's nets, through load_instance_view -- in its
    // frame instead of as a box, the nets already reaching its pins going on
    // inside; a pin with nothing outside it in view is a port flag in the
    // frame. Closing it (its frame's "-") draws the box again, and closes
    // the instances opened inside it.
    static void openInstance(const InstancePath& path);
    static void closeInstance(const InstancePath& path);
    static bool isOpened(const InstancePath& path);
    // Called by AppLogic when an instance_view_response arrives: opens the
    // instance, or logs why not (not found, too many instances to draw).
    static void applyInstanceView(const json& reply);

    // Test hooks (read-only): the geometry the last renderSchematic() drew,
    // and the screen position of its canvas's top-left corner -- enough for a
    // headless ImGui test to turn a pin's world position into a mouse
    // position (see tests/EquipotentialViewTest.cpp).
    // Scripted recording: while set, every frame shows this world rect
    // (fitted like View > Fit), whatever else asks for a fit.
    static void setCameraRect(std::optional<std::pair<ImVec2, ImVec2>> rect);
    static const SchematicView& schematicForTesting();
    static ImVec2 canvasOriginForTesting();
    static ImVec2 canvasSizeForTesting();
};

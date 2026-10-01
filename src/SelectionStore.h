#pragma once

#include <functional>

#include "Types.h"

// What is selected in the viewer -- same static/global state pattern as
// PropertiesStore/DiagnosisStore. Either one instance, by instance path ({} =
// the top design), or one net of the schematic (a SchematicNetRef), never
// both: selecting one drops the other.
//
// An instance is selected by a click on it in the tree or the schematic, or
// by a host's focus_instance push; it's drawn highlighted in both, and
// reported to the host (instance_selected) through the change listener
// AppLogic installs. A net is selected by a click on its wires in the
// schematic and drawn highlighted there; it isn't reported to the host (the
// schematic's wire trees aren't netlist objects the protocol can name).
class SelectionStore {
  public:
    enum class Origin { Tree, Schematic, Host };

    // Selects `path` and notifies the listener. Selecting what is already
    // selected does nothing (no notification).
    static void select(const InstancePath& path, Origin origin);
    // Selects a net in the schematic, dropping any instance selection
    // (without notifying the listener).
    static void selectNet(const SchematicNetRef& net);
    // Drops the selection without notifying (a new design was loaded).
    static void clear();

    // An instance is selected.
    static bool hasSelection();
    static const InstancePath& selected();
    static bool isSelected(const InstancePath& path);
    // A net is selected.
    static bool hasNetSelection();
    static const SchematicNetRef& selectedNet();
    static bool isNetSelected(const SchematicNetRef& net);
    static Origin origin();
    // Bumped on every change, so a view can react once to a new selection
    // (e.g. the tree revealing one made in the schematic).
    static unsigned revision();

    static void setListener(std::function<void(const InstancePath& path)> listener);
};

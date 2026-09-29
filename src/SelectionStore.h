#pragma once

#include <functional>

#include "Types.h"

// The one selected instance, by instance path ({} = the top design) -- same
// static/global state pattern as PropertiesStore/DiagnosisStore. Set by a
// click on an instance in the tree or the schematic, or by a host's
// focus_instance push; drawn highlighted in both, and reported to the host
// (instance_selected) through the change listener AppLogic installs.
class SelectionStore {
  public:
    enum class Origin { Tree, Schematic, Host };

    // Selects `path` and notifies the listener. Selecting what is already
    // selected does nothing (no notification).
    static void select(const InstancePath& path, Origin origin);
    // Drops the selection without notifying (a new design was loaded).
    static void clear();

    static bool hasSelection();
    static const InstancePath& selected();
    static bool isSelected(const InstancePath& path);
    static Origin origin();
    // Bumped on every change, so a view can react once to a new selection
    // (e.g. the tree revealing one made in the schematic).
    static unsigned revision();

    static void setListener(std::function<void(const InstancePath& path)> listener);
};

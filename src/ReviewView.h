#pragma once

#include <optional>
#include <string>
#include <vector>

#include <imgui.h>

#include "ReviewStore.h"

class INetlistProvider;

// The "Review" bottom-panel tab (the threads in ReviewStore: filter, read,
// reply, resolve) and the "Add Comment" dialog the tree and schematic menus
// open. Writes go to the host as review_* requests; what shows is always
// the host's next review_response.
class ReviewView {
  public:
    static void render(INetlistProvider* provider);

    // Opens the Add Comment dialog for an object: `anchor` is a
    // ReviewStore::*Anchor(), `label` names the object in the dialog.
    static void openComposer(const json& anchor, const std::string& label);
    // The dialog, drawn once per frame at the top level (a modal popup).
    static void renderComposer(INetlistProvider* provider);

    // "Add Comment..." menu item for a context menu: shown only when the
    // host can write; opens the dialog when clicked.
    static void commentMenuItem(const json& anchor, const std::string& label);
    // Hover tooltip listing these threads (their first comment each).
    static void threadsTooltip(const std::vector<const ReviewThread*>& threads);

    // Screen center of the dialog's Comment button in the last frame drawn
    // (tests click it), or nullopt while the dialog is closed.
    static std::optional<ImVec2> commentButtonForTesting();
};

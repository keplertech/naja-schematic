#pragma once

#include <functional>
#include <optional>
#include <string>

#include <imgui.h>
#include <nlohmann/json.hpp>

// The nets a host pushed with show_net (notebook/ViewerServer show_net()):
// same static/global state pattern as TraceStore. Each push becomes a
// one-net trace -- registered with its label (and color) when the push
// arrives, its net asked for with a load_equipotential tagged with the
// trace's id, exactly as the tree's "Show Equipotential" does.
//
// Two things set a host net apart from a tree one, both because the host
// re-sends its pushes after every root load, after its focus_instance:
// - a push naming a net the view already shows as a host net is skipped,
//   so a re-send doesn't stack a second trace (and color) on it;
// - a focus drawn after the push clears the view, and its traces, before
//   the net's reply comes back: the reply then registers the trace again,
//   same label and color, instead of drawing the net uncolored.
// And when the view is about to be cleared -- a focus_instance or an
// "add": false push arrives, the user clears it, the design is reloaded --
// forget() drops every host net: a reply still in flight for one is then
// not drawn at all.
class ShowNetStore {
  public:
    // Handles a show_net push. Returns the load_equipotential request to
    // send, or nullopt for a malformed push or a net already shown.
    // `clearView` is called first when the push has "add": false.
    static std::optional<nlohmann::json> request(const nlohmann::json& push,
                                                 const std::function<void()>& clearView);
    // An equipotential_response's trace_id -> the trace to draw its net
    // under: the same id, the id of the trace registered again for a host
    // net whose trace a view clear dropped while it was in flight, or -1:
    // a forgotten host net, not to be drawn.
    static int takeReply(int traceId);
    // The view is cleared, or about to be: forget every host net, and drop
    // the replies still in flight for them.
    static void forget();

    // "#rrggbb" -> an opaque color; nullopt for anything else.
    static std::optional<ImU32> parseColor(const std::string& text);
};

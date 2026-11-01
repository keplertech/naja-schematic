#pragma once

#include <optional>
#include <string>
#include <vector>

#include <imgui.h>

#include "Types.h"

// The review threads the host last pushed (review_response, see
// docs/review_format.md) -- same static/global state pattern as
// DiagnosisStore/TraceStore. The host owns the review files: the viewer only
// shows what it is sent and asks for writes (review_add_thread,
// review_add_event), answered by a fresh review_response.
//
// Threads are matched to netlist objects by ids only: a thread is drawn on
// an object only when the host resolved it "exact" on the loaded design, and
// then by its anchor's id_path and term/net id, never by name.

enum class ReviewAnchorKind { Instance, Term, Net, View };

// docs/review_format.md "Resolving an anchor".
enum class ReviewState { Exact, OtherSnapshot, Unresolved };

struct ReviewAnchor {
  ReviewAnchorKind          kind = ReviewAnchorKind::Instance;
  std::vector<unsigned>     idPath;   // the instance, or the one containing the term/net
  std::optional<unsigned>   termId;   // Term only
  std::optional<unsigned>   netId;    // Net only
  std::optional<int>        bit;      // Term/Net, optional: one bus bit
  std::string               view;     // View only
  // Labels, for display only.
  std::vector<std::string>  path;
  std::string               terminal;
  std::string               net;
};

struct ReviewComment {
  std::string id;
  std::string author;
  std::string date;
  std::string text;
};

struct ReviewThread {
  std::string               id;
  std::string               title;
  std::string               author;
  std::string               source;     // "human" or a tool name
  std::string               created;
  std::string               snapshot;
  bool                      snapshotStored = false;
  ReviewAnchor              anchor;
  ReviewState               state = ReviewState::Unresolved;
  // Exact only: the anchor's instance as it is now (ids and names).
  std::optional<InstancePath> resolvedPath;
  std::string               status = "open";   // "open" | "resolved" | "wontfix"
  DiagnosisSeverity         severity = DiagnosisSeverity::Info;
  std::vector<std::string>  labels;
  std::vector<ReviewComment> comments;

  bool isOpen() const { return status == "open"; }
};

class ReviewStore {
  public:
    // A review_response push replaces everything.
    static void setResponse(const json& response);
    // Forget the review (a fresh design: the host pushes the review again
    // after its root_response).
    static void clear();

    // A review directory is configured on the host (a review_response came).
    static bool hasReview();
    // The host has an author to write as.
    static bool writable();
    static const std::string& author();
    static const std::vector<ReviewThread>& threads();
    // Bumped on every change, for views caching anything derived.
    static unsigned revision();

    // Exact threads on an object, every status. `path`: the instance
    // (Instance), or the instance containing the term/net, as in
    // NetlistTree::getInstancePath() and InstanceShape::path. `bit`: one bus
    // bit, or nullopt for the whole object (all its bits included).
    static std::vector<const ReviewThread*> instanceThreads(const InstancePath& path);
    static std::vector<const ReviewThread*> termThreads(const InstancePath& path, unsigned termId,
                                                        std::optional<int> bit = std::nullopt);
    static std::vector<const ReviewThread*> netThreads(const InstancePath& path, unsigned netId,
                                                       std::optional<int> bit = std::nullopt);

    // Severity color of the worst open thread, 0 when none is open.
    static ImU32 openColor(const std::vector<const ReviewThread*>& threads);
    static size_t openCount(const std::vector<const ReviewThread*>& threads);

    // The last refused write (an "error" reply to a review_* request), shown
    // in the Review tab until the next review_response.
    static void setError(const std::string& message);
    static const std::string& error();

    // Requests. `anchor` is the review_add_thread anchor JSON (ids, plus the
    // name labels the viewer knows).
    static json addThreadRequest(const json& anchor, const std::string& text,
                                 DiagnosisSeverity severity, const std::string& title);
    static json addEventRequest(const std::string& thread, const std::string& text,
                                const std::string& status);

    // Anchor JSON for an instance, or a term/net of an instance.
    static json instanceAnchor(const InstancePath& path);
    static json termAnchor(const InstancePath& path, unsigned termId, const std::string& terminal,
                           std::optional<int> bit);
    static json netAnchor(const InstancePath& path, unsigned netId, const std::string& net,
                          std::optional<int> bit);
};

const char* toString(ReviewState state);
// A thread's object for people, e.g. "u1/u2/Q[3]" (labels only).
std::string anchorLabel(const ReviewAnchor& anchor);

#pragma once

#include <string>

// Drops the replies that belong to a design the host has replaced.
//
// On a design_changed push the viewer resets itself and sends load_root.
// Replies to requests it sent before the reset can still be on their way:
// they name tree nodes and instance ids of the old design, so they must not
// reach the new tree. Each transport answers requests in order, so every
// such reply arrives before the root_response to that load_root, and it is
// enough to drop everything until then. Pushes that arrive meanwhile
// (diagnoses, focus) are dropped too: the host re-sends them right after
// root_response.
//
// When several design_changed pushes overlap, only the root_response to the
// last load_root is kept; earlier ones (and what follows them) describe a
// design that is already gone.
class ReloadGuard {
public:
  // A reload started: a load_root is about to be sent.
  void reloadStarted() { ++pendingRoots_; }

  // Whether to handle an incoming message whose "response" field is
  // `response`.
  bool accept(const std::string& response) {
    if (pendingRoots_ == 0) return true;
    if (response == "root_response" || response == "root_loaded") {
      --pendingRoots_;
      return pendingRoots_ == 0;
    }
    return false;
  }

  bool reloading() const { return pendingRoots_ > 0; }

private:
  unsigned pendingRoots_ {0};
};

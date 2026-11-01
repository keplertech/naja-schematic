# Netlist reviews in naja-schematic

How people (and agents) leave findings on a netlist and pass them to each
other, asynchronously, through git. Status: threads are implemented in the
Python host (`python/naja_schematic/review.py`, najaeda >= 0.7.34 for net ids)
and the viewer (`src/ReviewStore.cpp`, `src/ReviewView.cpp`), so the browser
and notebook viewers read and write reviews; the native provider (desktop
app) and saved views are not yet. This document is the contract the two backends
(`src/LocalSNLProvider.cpp`, `python/naja_schematic/protocol.py`) and any
external writer (scripts, naja-agent) follow.

## Goals

- A finding outlives the window it was made in: anchored on a netlist object,
  stored in a file, read back by anyone who opens the same design.
- Sharing is git: the review sits next to the design, is committed, diffed and
  reviewed in pull requests like the RTL. No server, no database, nothing
  leaves the network.
- **No merge conflicts by construction**: every write creates a new file,
  no file is ever modified. Two people commenting on the same thread on two
  branches merge cleanly.
- Humans and tools are equal authors: a kepler-formal diagnosis is a thread a
  human can reply to, confirm or dismiss.
- **Objects are identified by naja ids only, never by names.** A thread is
  pinned to the exact netlist it was written on (its snapshot); on another
  netlist it isn't guessed at by name -- a name match can land a comment on
  the wrong gate. Names are stored only as labels for people.

## Concepts

- **Thread**: a discussion anchored on one netlist object (or on a saved
  view). Its history is an ordered list of **events**: comments, and changes
  of status/severity/labels.
- **Saved view**: a named, replayable schematic (focus instance, traces,
  equipotentials, hierarchy toggle, camera), stored as what builds it, never
  as geometry, so it re-lays out on a newer netlist.
- **Anchor**: the object, by its naja ids (instance id path, term or net id,
  bus bit), with its names alongside as labels.
- **Snapshot**: the netlist a thread was written on, stored in the review as
  a naja-if dump and named by its content hash. A thread's ids mean
  something only on its snapshot; anyone can open that exact netlist, with
  no design flow (RTL, defines, Liberty, synthesis) at hand.

## Directory layout

A review is a directory, `.naja-review/` by convention, committed with the
design:

```
.naja-review/
  review.json                              {"format": 1}
  .gitattributes                           snapshots/** binary
  snapshots/
    <snapshot-id>/                         a naja-if dump, written once per netlist
  threads/
    <thread-id>/
      thread.json                          created once, never modified
      <timestamp>-<event-id>.json          one per comment / status change
  views/
    <view-id>.json                         created once, never modified
```

- Thread, event and view ids are lowercase hex UUID4s (32 characters, no
  dashes); snapshot ids are lowercase hex sha256s (64 characters).
- `<timestamp>` is UTC with milliseconds, `YYYYMMDDTHHMMSSmmmZ`, so a
  directory listing is in chronological order. A writer keeps its own
  timestamps strictly increasing; the event id breaks ties between writers.
- Dates inside files are ISO 8601 UTC with milliseconds,
  `2026-10-31T14:25:30.123Z`.
- Files are UTF-8 JSON, 2-space indented, keys sorted, trailing newline, so a
  writer's output is stable and diffs are minimal.
- Readers ignore unknown keys and unknown files (forward compatibility), and
  skip a malformed file with a warning rather than failing the whole review.
- "Editing" a view means saving a new one; deleting a view or a thread is
  `git rm`, outside the tool's file model (the viewer offers it, as a
  file deletion, never as a rewrite).

## Anchor

```json
{
  "kind": "term",         // "instance" | "term" | "net" | "view"
  // identity, in the thread's snapshot:
  "id_path": [3, 7],      // instance ids (SNLInstance::getID()), root excluded, [] = top
  "term_id": 2,           // "term" only: SNLTerm id in that instance's model
  "net_id": 5,            // "net" only: SNLNet id in that instance's model
  "bit": 3,               // "term"/"net", optional: one bus bit, by its bit number
  "view": "<view-id>",    // "view" only
  // labels, for people only, never used to locate anything:
  "path": ["u1", "u2"],   // instance names, same levels ("" = anonymous)
  "terminal": "Q",        // "term": the term's name
  "net": "data"           // "net": the net's name ("" = anonymous)
}
```

For `instance` the id path is the instance itself; for `term`/`net` it is
the *containing* instance (`[]` = a top-level port or a top design net),
and the term/net id is looked up in its model (the top design for `[]`). Ids
reach what names can't: anonymous instances and anonymous nets (e.g.
naja's unnamed constant tie-offs).

Writers store ids and labels together, read from the design. A writer may
*name* the object instead (`path`, `terminal`, `net`), e.g. an agent or a
log adapter that only knows hierarchical names: the host locates it once,
at write time, in the loaded design (same rule as `resolve_request_path()`:
never through an anonymous `""` level), and stores its ids. Readers never
use the labels to locate anything.

## Snapshots

The host identifies the loaded design by dumping the whole universe as
naja-if (`netlist.dump_naja_if()`, `SNLCapnP::dump()`) and hashing the dump:

```
sha256( join(sorted(relative_posix_path + " " + sha256_hex(file) for file in dump), "\n") )
```

naja-if dumps are deterministic and keep instance ids, so the same netlist
gets the same id wherever and however it was loaded (Verilog, SystemVerilog,
VHDL, naja-if, or built in a notebook), and a reloaded snapshot answers the
same `id_path`s. The id covers everything naja built -- `--top`, defines,
Liberty cells, and the naja version's id assignment -- not the input files.

- The dump is made once per loaded design (again after each root load or
  `design_changed`), and stored as `snapshots/<id>/` the first time a thread
  is opened on it, before the thread file naming it. It's staged under a
  dot-name and renamed into place, so a snapshot directory is always
  complete; two writers storing the same id write the same bytes.
- `naja-schematic --review DIR` with no design opens the latest snapshot a
  thread was written on (`--snapshot ID` picks one).
- A snapshot is required: a thread's ids mean nothing without it. A thread
  whose snapshot isn't in the review (deleted, or not pulled through Git
  LFS) can still be read, but not shown on a schematic.
- Snapshots are binary and don't diff; a large netlist can be big. The
  review writes `snapshots/** binary` in its `.gitattributes`; teams with
  large designs should track them with Git LFS
  (`git lfs track ".naja-review/snapshots/**"`).
- Source files are not in a snapshot (only naja's references to them): Show
  Sources still needs the RTL checkout.

## Resolving an anchor

When a review is loaded against a design, each anchor resolves to one
`state`:

1. **`exact`** — the thread's snapshot is the loaded design's, and its ids
   resolve there (`resolve_request_path()` on `id_path`, then the term/net
   id and bit). The thread is drawn on that object.
2. **`other_snapshot`** — the thread was written on another netlist. Ids
   aren't comparable across netlists (a re-synthesized one can reuse an id
   for an unrelated instance), and names aren't trusted: the thread is
   listed, with its labels, and the viewer offers to open its snapshot
   (`snapshot_stored` says whether the review has it).
3. **`unresolved`** — its own snapshot is loaded but the ids don't resolve
   (a hand-edited or corrupt file). Listed, not drawn.

A `view` anchor is `exact` while the view file exists, else `unresolved`.

Carrying threads from one netlist to the next is deliberately left out: it
needs a real correspondence between the two (e.g. kepler-formal's
equivalence matching, or a naja diff mapping one snapshot's ids to the
other's), not a name heuristic.

## `thread.json`

```json
{
  "format": 1,
  "id": "<thread-id>",
  "created": "2026-10-31T14:25:30Z",
  "author": "Jane Doe",          // a person or a tool ("kepler-formal")
  "source": "human",             // "human" | free-form tool name
  "snapshot": "<snapshot-id>",   // the netlist the anchor's ids refer to
  "anchor": { ... },
  "view": "<view-id>",           // optional: a view illustrating it
  "title": "Reset not synchronized"
}
```

## Events: `<timestamp>-<event-id>.json`

```json
{
  "format": 1,
  "id": "<event-id>",
  "date": "2026-10-31T14:25:30Z",
  "author": "John Roe",
  "text": "Markdown body",       // optional: a comment
  "status": "resolved",          // optional: "open" | "resolved" | "wontfix"
  "severity": "error",           // optional: "info" | "warning" | "error"
  "labels": ["cdc"]              // optional: replaces the label set
}
```

The first event of a thread is written together with `thread.json` and
normally carries the opening comment, `status: "open"` and the severity. A
thread's current status/severity/labels is the value from the latest event
(by `date`, then `id`) that sets it; a thread with no status event is
`open`. Comments are shown in that same order.

## Saved view: `views/<view-id>.json`

```json
{
  "format": 1,
  "id": "<view-id>",
  "name": "reset cone",
  "author": "Jane Doe",
  "created": "2026-10-31T14:25:30Z",
  "snapshot": "<snapshot-id>",
  "focus": { "kind": "instance", ... },          // optional
  "traces": [
    { "start": { "kind": "term", ..., "bit": 0 },
      "color": "#d62728",                        // optional, else palette
      "visible": true }
  ],
  "equipotentials": [ { "start": { "kind": "term", ... } } ],
  "show_hierarchy": true,
  "camera": { "center": [120.0, 40.0], "zoom": 1.5 }   // optional hint
}
```

Opening a view clears the schematic and replays it in order: focus first
(`resolve_instance`), then each trace (`trace_driver`) and equipotential
(`load_equipotential`). Their starting terminals are resolved by name to the
provider's term ids by the host. An unresolvable item is skipped and
reported; the rest still shows. The camera is applied only if the
snapshot matches (geometry may differ otherwise; the view fits instead).

## Author identity

Taken, in order, from: `--author`, `NAJA_SCHEMATIC_AUTHOR`, `git config
user.name` in the review directory's repository. With none, writing is
refused (the review stays readable).

## Wire protocol

The host owns the files, as with `load_source` (a WASM viewer can't write
disk).

- push `review_response`:
  ```json
  { "response": "review_response",
    "author": "Jane Doe", "writable": true,      // false: no author, read-only
    "snapshot": "<snapshot-id>",                  // the loaded design's, or null
    "views": [ ... ],                             // the view files, as stored
    "threads": [ { "id": "...", "title": "...", "author": "...", "source": "human",
                   "created": "...", "view": null,
                   "snapshot": "...", "snapshot_stored": true,          // its netlist, in the review?
                   "anchor": { ... },                                   // as stored
                   "resolution": { "state": "exact",                    // | "other_snapshot" | "unresolved"
                                   "id_path": [0, 0], "path": ["u1", "u2"] },  // exact only: names now
                   "status": "open", "severity": "error", "labels": [],
                   "comments": [ {"id": "...", "author": "...", "date": "...", "text": "..."} ] } ] }
  ```
  Threads are folded (current status/severity/labels, comments in order).
  Sent after each `root_response` when a review directory is configured, and
  after every write, to every viewer.
- requests `review_add_thread {anchor, text, severity?, labels?, view?, title?}`
  (the host locates the anchor in the loaded design -- by its ids, or by
  names for a writer that has no ids -- and stores its ids with name labels;
  it must exist) and
  `review_add_event {thread, text?, status?, severity?, labels?}` -> the host
  writes the file(s) and pushes the updated `review_response`. A refused
  write is answered `{"response": "error", "request": <kind>, "message": ...}`
  to that viewer only. (`review_save_view` comes with saved views.)
- All carry `generation` like every other message; a stale write is dropped
  unanswered (`ViewerSession.answer()`).
- Without a review directory, or without an author, write requests fail and
  the viewer hides its write actions.

The viewer anchors by ids it already has: instance `id_path`s everywhere, a
term's `child_id` (`terms_response`, `expand_instance_terms`) and a net's
`net_id` (`nets_response`, both backends; a net row from an older host has
none and offers no comment). It matches threads to tree rows and schematic
boxes the same way, by ids, and only `exact` ones.

Shared cases for both backends: `tests/data/review_protocol_cases.json`.

## Diagnoses

`diagnosis_response` items are shown as unsaved threads (author = the
item's `source`). "Keep" writes one to the review as a real thread, after
which humans can reply to it. A tool can also write threads directly into
the directory, with no viewer running.

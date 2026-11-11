"""Netlist reviews: threads and saved views kept in a directory in git.

The format is specified in docs/review_format.md. Every write creates a new
file and no file is ever modified, so reviews made on two branches merge
without conflicts. This module reads and writes that directory and resolves
anchors against the loaded design; ViewerSession wires it to the viewer.
"""

import json
import logging
import os
import shutil
import subprocess
import threading
import uuid
from datetime import datetime, timedelta, timezone
from pathlib import Path

from najaeda import naja, netlist

from . import protocol

log = logging.getLogger(__name__)

FORMAT = 1
STATUSES = ("open", "resolved", "wontfix")
SEVERITIES = ("info", "warning", "error")
ANCHOR_KINDS = ("instance", "term", "net", "view")


class ReviewError(Exception):
    """A write the review refuses (bad request, no author, ...)."""


def new_id():
    return uuid.uuid4().hex


def is_review_id(value):
    # Ids name files: only what new_id() makes, never a path.
    return (isinstance(value, str) and len(value) == 32
            and all(c in "0123456789abcdef" for c in value))


def now():
    when = datetime.now(timezone.utc)
    return when.replace(microsecond=when.microsecond // 1000 * 1000)


def iso_date(when):
    return when.strftime("%Y-%m-%dT%H:%M:%S.") + f"{when.microsecond // 1000:03d}Z"


def file_stamp(when):
    return when.strftime("%Y%m%dT%H%M%S") + f"{when.microsecond // 1000:03d}Z"


def dump(path, document):
    """Write `document` as the format wants it (sorted keys, 2-space indent,
    trailing newline), refusing to overwrite: review files are never
    modified."""
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "x", encoding="utf-8") as f:
        json.dump(document, f, indent=2, sort_keys=True, ensure_ascii=False)
        f.write("\n")


def _load(path):
    try:
        with open(path, encoding="utf-8") as f:
            document = json.load(f)
    except (OSError, ValueError) as e:
        log.warning("Skipping unreadable review file %s: %s", path, e)
        return None
    if not isinstance(document, dict):
        log.warning("Skipping review file %s: not an object", path)
        return None
    return document


def is_snapshot_id(value):
    return (isinstance(value, str) and len(value) == 64
            and all(c in "0123456789abcdef" for c in value))


def default_author(directory, explicit=None):
    """`explicit`, else $NAJA_SCHEMATIC_AUTHOR, else `git config user.name`
    in `directory`'s repository; None when there's none."""
    if explicit:
        return explicit
    env = os.environ.get("NAJA_SCHEMATIC_AUTHOR")
    if env:
        return env
    cwd = Path(directory)
    while not cwd.is_dir() and cwd != cwd.parent:
        cwd = cwd.parent  # the review directory may not exist yet
    try:
        out = subprocess.run(["git", "config", "user.name"], cwd=cwd, capture_output=True,
                             text=True, timeout=5)
    except (OSError, subprocess.SubprocessError):
        return None
    name = out.stdout.strip()
    return name or None


def _is_id(value):
    return isinstance(value, int) and not isinstance(value, bool) and value >= 0


def _check_ids(anchor, key):
    value = anchor.get(key)
    if value is not None and not (isinstance(value, list) and all(_is_id(i) for i in value)):
        raise ReviewError(f"anchor: {key} must be a list of instance ids")
    return value


def check_anchor(anchor):
    """The anchor with only the keys its kind uses; raises ReviewError if
    it's malformed. Ids and names are both accepted here: a writer may name
    the object either way (see locate())."""
    if not isinstance(anchor, dict) or anchor.get("kind") not in ANCHOR_KINDS:
        raise ReviewError("anchor: kind must be one of " + ", ".join(ANCHOR_KINDS))
    kind = anchor["kind"]
    if kind == "view":
        if not is_review_id(anchor.get("view")):
            raise ReviewError("anchor: a view anchor needs a view id")
        return {"kind": "view", "view": anchor["view"]}
    clean = {"kind": kind}
    if _check_ids(anchor, "id_path") is not None:
        clean["id_path"] = list(anchor["id_path"])
    path = anchor.get("path")
    if path is not None:
        if not (isinstance(path, list) and all(isinstance(n, str) for n in path)):
            raise ReviewError("anchor: path must be a list of instance names")
        clean["path"] = list(path)
    if "id_path" not in clean and "path" not in clean:
        raise ReviewError("anchor: id_path or path is required")
    if kind in ("term", "net"):
        id_key, name_key = ("term_id", "terminal") if kind == "term" else ("net_id", "net")
        if anchor.get(id_key) is not None:
            if not _is_id(anchor[id_key]):
                raise ReviewError(f"anchor: {id_key} must be an id")
            clean[id_key] = anchor[id_key]
        if isinstance(anchor.get(name_key), str):
            clean[name_key] = anchor[name_key]
        if id_key not in clean and not clean.get(name_key):
            raise ReviewError(f"anchor: a {kind} anchor needs a {id_key} (or a {name_key} name)")
        bit = anchor.get("bit")
        if bit is not None:
            if not isinstance(bit, int) or isinstance(bit, bool):
                raise ReviewError("anchor: bit must be an integer")
            clean["bit"] = bit
    return clean


def locate(top, anchor, by_name=False):
    """The object `anchor` names in `top`, as a stored anchor: its ids
    (`id_path`, `term_id`/`net_id`, `bit`), which identify it, plus its
    names (`path`, `terminal`/`net`), which only label it. None if it isn't
    there. Stored anchors are only ever located by their ids; `by_name`
    lets a writer that knows names (an agent, a log adapter) name the object
    once, at write time, against the loaded design -- by the usual request
    rule, never through an anonymous "" level."""
    kind = anchor["kind"]
    lookup = anchor if by_name else {"id_path": anchor.get("id_path")}
    if not by_name and not isinstance(lookup["id_path"], list):
        return None
    design, chain = protocol.resolve_request_path(top, lookup)
    if design is None:
        return None
    found = {"kind": kind, "id_path": [i.getID() for i in chain],
             "path": [i.getName() for i in chain]}
    bit = anchor.get("bit")
    if kind == "term":
        if "term_id" in anchor:
            term = design.getTermByID(anchor["term_id"])
        else:
            term = design.getTerm(anchor["terminal"]) if by_name else None
        if term is None:
            return None
        if bit is not None and (not isinstance(term, naja.SNLBusTerm)
                                or term.getBusTermBit(bit) is None):
            return None
        found.update(term_id=term.getID(), terminal=term.getName())
    elif kind == "net":
        if "net_id" in anchor:
            net = design.getNetByID(anchor["net_id"])
        else:
            net = design.getNet(anchor["net"]) if by_name and anchor.get("net") else None
        if net is None:
            return None
        if bit is not None and (not isinstance(net, naja.SNLBusNet) or net.getBit(bit) is None):
            return None
        found.update(net_id=net.getID(), net=net.getName())  # "" when anonymous
    if bit is not None:
        found["bit"] = bit
    return found


def resolve_anchor(top, anchor, thread_snapshot, design_snapshot, views=()):
    """Where a stored anchor lands in the loaded design (see "Resolving an
    anchor" in docs/review_format.md): {"state": "exact", "id_path", "path"}
    (the object's names now) when the thread was written on this very
    netlist and its ids resolve; {"state": "other_snapshot"} when it was
    written on another one (never guessed across netlists: open that
    snapshot to see it); else {"state": "unresolved"}."""
    if anchor["kind"] == "view":
        return {"state": "exact" if anchor["view"] in views else "unresolved"}
    if thread_snapshot is None or thread_snapshot != design_snapshot:
        return {"state": "other_snapshot"}
    found = locate(top, anchor) if top is not None else None
    if found is None:
        return {"state": "unresolved"}
    return {"state": "exact", "id_path": found["id_path"], "path": found["path"]}


class Thread:
    def __init__(self, document, events):
        self.document = document
        self.events = sorted(events, key=lambda e: (str(e.get("date", "")), str(e.get("id", ""))))

    @property
    def id(self):
        return self.document["id"]

    def latest(self, key, default):
        value = default
        for event in self.events:
            if key in event:
                value = event[key]
        return value

    def to_json(self, resolution, stored):
        return {
            "id": self.id,
            "title": self.document.get("title", ""),
            "author": self.document.get("author", ""),
            "source": self.document.get("source", "human"),
            "created": self.document.get("created", ""),
            "view": self.document.get("view"),
            "snapshot": self.document.get("snapshot"),
            "snapshot_stored": stored,
            "anchor": self.document["anchor"],
            "resolution": resolution,
            "status": self.latest("status", "open"),
            "severity": self.latest("severity", "info"),
            "labels": self.latest("labels", []),
            "comments": [
                {"id": e.get("id", ""), "author": e.get("author", ""), "date": e.get("date", ""),
                 "text": e["text"]}
                for e in self.events if isinstance(e.get("text"), str)
            ],
        }


class Review:
    """One review directory. `author` writes new threads and events (None:
    read-only). The loaded design is identified by naja's content hash, and
    dumped as naja-if into the review the first time a thread is opened on
    it, so the review carries every netlist its threads' ids refer to.
    Reads always come from disk, so files pulled from git while the viewer
    runs show at the next load.

    The universe must not change between two calls without
    design_changed() (ViewerSession calls it on every root load): the hash
    is reused until then."""

    def __init__(self, directory, author=None):
        self.directory = Path(directory)
        self.author = author
        self._lock = threading.Lock()
        self._snapshot = None  # the loaded design's id
        self._last_write = None

    def _now(self):
        # Strictly increasing per writer, so its own events keep their order
        # even within one millisecond (ties across writers fall back to ids).
        with self._lock:
            when = now()
            if self._last_write is not None and when <= self._last_write:
                when = self._last_write + timedelta(milliseconds=1)
            self._last_write = when
            return when

    def design_changed(self):
        """Forget the loaded design's snapshot id: the next call hashes it again."""
        with self._lock:
            self._snapshot = None

    def design_snapshot(self, top):
        """The loaded design's snapshot id (None: no design): naja's content
        hash of the universe, once per design (see the class docstring);
        call with exclusive access to it."""
        if top is None:
            return None
        with self._lock:
            if self._snapshot is None:
                self._snapshot = naja.NLUniverse.get().getContentHash()
            return self._snapshot

    def snapshot_path(self, snapshot):
        return self.directory / "snapshots" / snapshot

    def _store_snapshot(self, snapshot):
        """Dump the loaded design into the review as `snapshot`, unless it's
        there already; call with exclusive access to the universe."""
        target = self.snapshot_path(snapshot)
        with self._lock:
            if target.is_dir() or self._snapshot != snapshot:
                return
            staging = target.with_name(f".{snapshot}-{new_id()}")
            target.parent.mkdir(parents=True, exist_ok=True)
            try:
                netlist.dump_naja_if(str(staging))
                staging.rename(target)  # atomic: never a half-written snapshot
            except OSError:
                if not target.is_dir():  # else another writer stored it first
                    raise
            finally:
                shutil.rmtree(staging, ignore_errors=True)

    def _is_stored(self, snapshot):
        return is_snapshot_id(snapshot) and self.snapshot_path(snapshot).is_dir()

    def latest_snapshot(self):
        """The stored snapshot of the most recent thread that has one, to
        open a review with no design given; None if there's none."""
        for thread in reversed(self.threads()):
            snapshot = thread.document.get("snapshot")
            if self._is_stored(snapshot):
                return snapshot
        return None

    # -- reading -------------------------------------------------------------

    def threads(self):
        threads = []
        root = self.directory / "threads"
        if not root.is_dir():
            return threads
        for folder in sorted(root.iterdir()):
            if not folder.is_dir():
                continue
            document = _load(folder / "thread.json")
            if document is None:
                continue
            if document.get("id") != folder.name:
                log.warning("Skipping review thread %s: its id doesn't match", folder)
                continue
            try:
                document["anchor"] = check_anchor(document.get("anchor"))
            except ReviewError as e:
                log.warning("Skipping review thread %s: %s", folder, e)
                continue
            events = [e for e in (_load(p) for p in sorted(folder.glob("*.json"))
                                  if p.name != "thread.json") if e is not None]
            threads.append(Thread(document, events))
        threads.sort(key=lambda t: (str(t.document.get("created", "")), t.id))
        return threads

    def views(self):
        views = []
        root = self.directory / "views"
        if root.is_dir():
            for path in sorted(root.glob("*.json")):
                document = _load(path)
                if document is not None and document.get("id") == path.stem:
                    views.append(document)
        views.sort(key=lambda v: (str(v.get("created", "")), v["id"]))
        return views

    def response(self, top):
        """The review_response message for the loaded design `top`."""
        design_snapshot = self.design_snapshot(top)
        views = self.views()
        view_ids = {v["id"] for v in views}
        threads = [
            t.to_json(resolve_anchor(top, t.document["anchor"], t.document.get("snapshot"),
                                     design_snapshot, view_ids),
                      self._is_stored(t.document.get("snapshot")))
            for t in self.threads()
        ]
        return {
            "response": "review_response",
            "author": self.author or "",
            "writable": bool(self.author),
            "snapshot": design_snapshot,
            "threads": threads,
            "views": views,
        }

    # -- writing -------------------------------------------------------------

    def _require_author(self):
        if not self.author:
            raise ReviewError("no author: pass --author or set NAJA_SCHEMATIC_AUTHOR")
        return self.author

    def _ensure_root(self):
        meta = self.directory / "review.json"
        if not meta.exists():
            try:
                dump(meta, {"format": FORMAT})
            except FileExistsError:
                pass  # another writer made it first
        # Snapshots are binary: no text diffs or merges (see
        # docs/review_format.md for keeping them in Git LFS).
        attributes = self.directory / ".gitattributes"
        if not attributes.exists():
            try:
                with open(attributes, "x", encoding="utf-8") as f:
                    f.write("snapshots/** binary\n")
            except FileExistsError:
                pass

    @staticmethod
    def _event(author, when, text=None, status=None, severity=None, labels=None):
        event = {"format": FORMAT, "id": new_id(), "date": iso_date(when), "author": author}
        if text is not None:
            if not isinstance(text, str):
                raise ReviewError("text must be a string")
            event["text"] = text
        if status is not None:
            if status not in STATUSES:
                raise ReviewError("status must be one of " + ", ".join(STATUSES))
            event["status"] = status
        if severity is not None:
            if severity not in SEVERITIES:
                raise ReviewError("severity must be one of " + ", ".join(SEVERITIES))
            event["severity"] = severity
        if labels is not None:
            if not (isinstance(labels, list) and all(isinstance(x, str) for x in labels)):
                raise ReviewError("labels must be a list of strings")
            event["labels"] = list(labels)
        return event

    def _write_event(self, thread_id, event, when):
        dump(self.directory / "threads" / thread_id / f"{file_stamp(when)}-{event['id']}.json",
             event)

    def add_thread(self, top, anchor, text, severity="info", labels=None, title="", view=None,
                   author=None, source="human"):
        """Open a thread on `anchor`, an object of the loaded design `top`
        named by ids or, failing those, names (see locate()); it's stored by
        its ids, with its names as labels. `author`/`source` override the
        review's author, for a tool writing its findings. Returns the
        thread id."""
        author = author or self._require_author()
        anchor = check_anchor(anchor)
        if anchor["kind"] != "view":
            by_name = "id_path" not in anchor or (anchor["kind"] == "term"
                                                  and "term_id" not in anchor) or (
                anchor["kind"] == "net" and "net_id" not in anchor)
            found = locate(top, anchor, by_name=by_name) if top is not None else None
            if found is None:
                raise ReviewError("anchor: no such object in the loaded design")
            anchor = found
            if sum(1 for _ in naja.NLUniverse.get().getUserDBs()) != 1:
                # The snapshot (the top DB's dump) wouldn't reload as the
                # universe the id hashes.
                raise ReviewError("the universe holds several design databases: "
                                  "a review snapshot stores only the top one")
        snapshot = self.design_snapshot(top) if anchor["kind"] != "view" else None
        when = self._now()
        thread_id = new_id()
        document = {
            "format": FORMAT,
            "id": thread_id,
            "created": iso_date(when),
            "author": author,
            "source": source,
            "snapshot": snapshot,
            "anchor": anchor,
            "title": title if isinstance(title, str) else "",
        }
        if view is not None:
            if not is_review_id(view):
                raise ReviewError("view: not a view id")
            document["view"] = view
        event = self._event(author, when, text=text, status="open", severity=severity,
                            labels=labels)
        self._ensure_root()
        if snapshot is not None:
            self._store_snapshot(snapshot)  # before the thread that names it
        dump(self.directory / "threads" / thread_id / "thread.json", document)
        self._write_event(thread_id, event, when)
        return thread_id

    def add_event(self, thread_id, text=None, status=None, severity=None, labels=None,
                  author=None):
        """Reply to a thread and/or change its status, severity or labels."""
        author = author or self._require_author()
        if not is_review_id(thread_id) or not (self.directory / "threads" / thread_id
                                                  / "thread.json").is_file():
            raise ReviewError(f"no thread {thread_id!r}")
        if text is None and status is None and severity is None and labels is None:
            raise ReviewError("nothing to add: give a text, status, severity or labels")
        when = self._now()
        event = self._event(author, when, text, status, severity, labels)
        self._write_event(thread_id, event, when)
        return event["id"]

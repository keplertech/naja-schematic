"""One viewer's state on the Python side, whatever carries its messages.

A ViewerSession answers the viewer's requests (protocol.handle_request()
against the live NLUniverse), keeps the host's pushes -- diagnoses and the
focused instance -- and re-sends them after every root_response (the viewer
clears both on each root load), and records the instance the viewer
selects. The notebook widget, the CLI servers and the embeddable
ViewerServer all wrap one; they only differ in how messages travel.

Universe access is split by who starts it:
- what the viewer starts (its requests, and resolving a selection sent by
  names only) runs through `run(fn)`, which the host can hook to serialize
  it with its own design operations (a lock, or a hop to its own thread);
- what the host starts (annotate(), show_instance(), selected) touches the
  universe directly, on the host's thread: call those where the host's own
  design operations are allowed. They never go through `run`, so a host
  holding its lock -- or on the very thread `run` hops to -- can call them
  without deadlocking.
"""
import json
import logging
import threading

from najaeda import naja

from . import protocol

log = logging.getLogger("naja_schematic")


def names_to_ids(names):
    """Instance-name path (top excluded) -> instance-id path, walking down
    from the top design; None if a name doesn't resolve. An anonymous
    instance has no name to walk by ("" never resolves): use its ids."""
    universe = naja.NLUniverse.get()
    design = universe.getTopDesign() if universe else None
    ids = []
    for name in names:
        inst = design.getInstance(name) if design and name else None
        if inst is None:
            return None
        ids.append(inst.getID())
        design = inst.getModel()
    return ids


def ids_to_names(ids):
    """Instance-id path -> instance names ("" for an anonymous level); None
    if an id doesn't resolve."""
    universe = naja.NLUniverse.get()
    design = universe.getTopDesign() if universe else None
    names = []
    for inst_id in ids:
        inst = design.getInstanceByID(inst_id) if design else None
        if inst is None:
            return None
        names.append(inst.getName())
        design = inst.getModel()
    return names


def is_id(value):
    return isinstance(value, int) and not isinstance(value, bool)


def instance_paths(target):
    """Normalize what show_instance() accepts to (names, ids): the instance's
    name path and id path, top excluded ([] = the top design). `target` is a
    najaeda netlist.Instance, a list/tuple of instance ids (ints, as najaeda's
    Instance.pathIDs), a list/tuple of names, or a single name for a child of
    the top design. A string is one name, never split on "/": escaped names
    can contain it. Ids reach anonymous instances; names can't. Raises
    ValueError if the path doesn't resolve in the loaded design."""
    if hasattr(target, "pathIDs"):  # najaeda.netlist.Instance
        ids = list(target.pathIDs)
    elif isinstance(target, str):
        ids = None
        names = [target]
    elif isinstance(target, (list, tuple)):
        if target and all(is_id(seg) for seg in target):
            ids = list(target)
        elif all(isinstance(seg, str) for seg in target):
            ids = None
            names = list(target)
        else:
            raise TypeError("expected a path of instance names (str) or of "
                            f"instance ids (int), got {list(target)!r}")
    else:
        raise TypeError(f"expected a najaeda Instance, a list of names or ids, "
                        f"or a name, got {type(target).__name__}")
    if ids is None:
        ids = names_to_ids(names)
        if ids is None:
            raise ValueError(f"no instance {names!r} in the loaded design")
    else:
        names = ids_to_names(ids)
        if names is None:
            raise ValueError(f"instance id path {ids} does not resolve in the loaded design")
    return names, ids


def instance_path(target):
    """The name path of what show_instance() accepts (see instance_paths())."""
    return instance_paths(target)[0]


def selected_instance(id_path, path):
    """A selection (id path and/or name path) as a najaeda netlist.Instance
    (the top design for []), or None if there is none or it no longer
    exists in the netlist."""
    if id_path is not None:
        ids = id_path
        if ids_to_names(ids) is None:
            return None
    elif path is not None:
        ids = names_to_ids(path)
        if ids is None:
            return None
    else:
        return None
    from najaeda import netlist
    return netlist.Instance(ids)


def lock_runner(lock):
    """A `run` for ViewerSession that holds `lock` (any context manager)
    around each call."""
    def run(fn):
        with lock:
            return fn()
    return run


class ViewerSession:
    """The Python side of one viewer (or of several showing the same view).

    `push(message)` sends a JSON string to the viewer(s) unasked; it may be
    None until a transport is attached. `run(fn)` returns fn(), called with
    exclusive access to the universe (default: fn() under a private RLock).
    `diagnosis` pre-loads diagnosis items, sent after the first root load.
    """

    def __init__(self, push=None, run=None, diagnosis=None):
        self._push = push
        self._run = run or lock_runner(threading.RLock())
        self._state_lock = threading.Lock()  # pushes and selection, not the universe
        self._diagnosis_push = (json.dumps(protocol.diagnosis_response(diagnosis))
                                if diagnosis is not None else None)
        self._focus_push = None
        self._select_callbacks = []
        self.selected_id_path = None
        self.selected_path = None

    # -- viewer -> host ----------------------------------------------------

    def answer(self, message):
        """Answer one JSON message from the viewer; returns the JSON
        strings to send back to that viewer, in order."""
        try:
            request = json.loads(message)
        except json.JSONDecodeError as e:
            log.error("Ignoring malformed viewer request %r: %s", message, e)
            return []
        if not isinstance(request, dict):
            log.error("Ignoring viewer message that is not an object: %r", message)
            return []
        if request.get("request") == "instance_selected":
            self._on_viewer_selection(request)
            return []  # a notification: no reply
        replies = []
        for reply in self._run(lambda: protocol.handle_request(request)):
            replies.append(json.dumps(reply))
            # The viewer clears its diagnoses and selection on every
            # root_response, so re-apply them after the root is (re)loaded.
            if reply.get("response") == "root_response":
                with self._state_lock:
                    replies.extend(p for p in (self._diagnosis_push, self._focus_push) if p)
        return replies

    def _on_viewer_selection(self, request):
        path, ids = request.get("path"), request.get("id_path")
        if not (isinstance(ids, list) and all(is_id(i) for i in ids)):
            # A viewer that only sends names: find the ids by them.
            ids = self._run(lambda: names_to_ids(path)) if isinstance(path, list) else None
        path = list(path) if isinstance(path, list) else None
        with self._state_lock:
            self.selected_path, self.selected_id_path = path, ids
            callbacks = list(self._select_callbacks)
        # Outside every lock: a callback may take the host's lock itself.
        for callback in callbacks:
            try:
                callback(ids, path)
            except Exception:
                log.exception("Error in a selection callback")

    def on_select(self, callback):
        """Call `callback(id_path, path)` on each selection the viewer
        reports, from the thread that answers the viewer, and with
        (None, None) when design_changed() clears it. Returns `callback`,
        for remove_select_callback()."""
        with self._state_lock:
            self._select_callbacks.append(callback)
        return callback

    def remove_select_callback(self, callback):
        with self._state_lock:
            self._select_callbacks.remove(callback)

    # -- host -> viewer ----------------------------------------------------

    def _send(self, message):
        if self._push is not None:
            self._push(message)

    def annotate(self, items):
        """Overlay diagnosis items on the view, now and after every reload
        (see protocol.diagnosis_response() for the item shape). Pass [] to
        clear."""
        message = json.dumps(protocol.diagnosis_response(items))
        with self._state_lock:
            self._diagnosis_push = message
        self._send(message)

    def show_instance(self, target):
        """Reveal, select and draw one instance, now and after every
        reload (see instance_paths() for what `target` can be)."""
        names, ids = instance_paths(target)
        message = json.dumps(protocol.focus_instance(names, ids))
        with self._state_lock:
            self._focus_push = message
        self._send(message)

    def design_changed(self, diagnosis=None, instance=None):
        """Tell the viewer(s) that the design was replaced or edited: they
        reload it from the root. The diagnoses and focused instance kept so
        far name the old design, so they are replaced by `diagnosis` and
        `instance` (resolved against the new design; None: none), sent
        after the new root loads. The selection is cleared: selection
        callbacks get (None, None)."""
        diagnosis_push = (json.dumps(protocol.diagnosis_response(diagnosis))
                          if diagnosis is not None else None)
        focus_push = None
        if instance is not None:
            names, ids = instance_paths(instance)
            focus_push = json.dumps(protocol.focus_instance(names, ids))
        with self._state_lock:
            self._diagnosis_push, self._focus_push = diagnosis_push, focus_push
            had_selection = self.selected_id_path is not None or self.selected_path is not None
            self.selected_path = self.selected_id_path = None
            callbacks = list(self._select_callbacks) if had_selection else []
        self._send(json.dumps(protocol.design_changed()))
        for callback in callbacks:
            try:
                callback(None, None)
            except Exception:
                log.exception("Error in a selection callback")

    @property
    def selected(self):
        """The viewer's selected instance as a najaeda netlist.Instance, or
        None (see selected_instance())."""
        with self._state_lock:
            ids, path = self.selected_id_path, self.selected_path
        return selected_instance(ids, path)

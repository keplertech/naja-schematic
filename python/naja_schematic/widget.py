"""Jupyter/Colab/VSCode-notebook view of the loaded netlist (anywidget).

The viewer runs in the notebook's output area; its requests come back to
this kernel over the widget comm channel and are answered by
protocol.handle_request() against the live NLUniverse -- so a view shows
the netlist as the notebook has built or edited it so far.
"""
import functools
import json
import logging

import anywidget
import traitlets
from najaeda import naja

from . import protocol
from ._bundle import STATIC_DIR, bundle_path

log = logging.getLogger("naja_schematic")


@functools.cache
def _widget_esm():
    bundle = bundle_path()
    if bundle is None:
        raise RuntimeError(
            "The naja-schematic viewer bundle is not part of this install. "
            "Install a released wheel (pip install naja-schematic), or build "
            "it with -DNAJA_SCHEMATIC_WASM_MODULE=ON and point "
            "NAJA_SCHEMATIC_BUNDLE at the resulting naja-schematic.js.")
    # The bundle is a classic script defining createNajaSchematic; the glue
    # appended after it is the ES module anywidget loads.
    return bundle.read_text() + "\n" + (STATIC_DIR / "widget.js").read_text()


def _names_to_ids(names):
    # Instance-name path (top excluded) -> najaeda child-id path, walking
    # down from the top design; None if a name doesn't resolve. An anonymous
    # instance has no name to walk by ("" never resolves): use its ids.
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


def _ids_to_names(ids):
    # Child-id path -> instance names ("" for an anonymous level); None if an
    # id doesn't resolve.
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


def _is_id(value):
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
        if target and all(_is_id(seg) for seg in target):
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
        ids = _names_to_ids(names)
        if ids is None:
            raise ValueError(f"no instance {names!r} in the loaded design")
    else:
        names = _ids_to_names(ids)
        if names is None:
            raise ValueError(f"instance id path {ids} does not resolve in the loaded design")
    return names, ids


def instance_path(target):
    """The name path of what show_instance() accepts (see instance_paths())."""
    return instance_paths(target)[0]


class Schematic(anywidget.AnyWidget):
    """An interactive naja-schematic view of the design in NLUniverse."""

    height = traitlets.Int(600).tag(sync=True)
    # The instance selected in the viewer -- clicked in its tree or
    # schematic, or set by show_instance() -- as its id path (top excluded,
    # najaeda's Instance.pathIDs) and its name path ("" for an anonymous
    # level). None until something is selected. Kernel-side only (not
    # synced): the viewer reports changes as instance_selected messages.
    # Observe them with on_select(), or traitlets' observe(..., names=
    # "selected_id_path") -- not "selected_path": anonymous siblings share
    # a name path, so moving between them wouldn't change it.
    selected_id_path = traitlets.List(default_value=None, allow_none=True)
    selected_path = traitlets.List(default_value=None, allow_none=True)

    def __init__(self, **kwargs):
        # Read before AnyWidget.__init__, which turns it into a synced trait.
        # Not a class attribute: that would read the bundle at import time
        # and fail the import when no bundle is installed.
        self._esm = _widget_esm()
        super().__init__(**kwargs)
        self._diagnosis_push = None
        self._focus_push = None
        self.on_msg(self._on_viewer_message)

    def _send_json(self, message):
        self.send({"json": message})

    def _on_viewer_message(self, _widget, content, _buffers):
        message = content.get("json") if isinstance(content, dict) else None
        if not isinstance(message, str):
            return
        try:
            request = json.loads(message)
        except json.JSONDecodeError as e:
            log.error("Ignoring malformed viewer request %r: %s", message, e)
            return
        if request.get("request") == "instance_selected":
            self._on_viewer_selection(request)
            return
        for reply in protocol.handle_request(request):
            self._send_json(json.dumps(reply))
            # The viewer clears its diagnoses and selection on every
            # root_response, so re-apply them after the root is (re)loaded.
            if reply.get("response") == "root_response":
                if self._diagnosis_push:
                    self._send_json(self._diagnosis_push)
                if self._focus_push:
                    self._send_json(self._focus_push)

    def _on_viewer_selection(self, request):
        path, ids = request.get("path"), request.get("id_path")
        if not (isinstance(ids, list) and all(_is_id(i) for i in ids)):
            # A viewer that only sends names: find the ids by them.
            ids = _names_to_ids(path) if isinstance(path, list) else None
        # Names first: on_select() observes the ids, and its callback may
        # read both.
        self.selected_path = list(path) if isinstance(path, list) else None
        self.selected_id_path = ids

    def annotate(self, items):
        """Overlay diagnosis items on the view (and keep them across reloads).

        `items`: a list of diagnosis dicts -- kind ("instance"|"net"),
        id_path (instance ids, root excluded; preferred) and/or path
        (instance names, root excluded; or a najaeda netlist.Instance,
        turned into both), terminal (nets only), severity
        ("info"|"warning"|"error"), message, source -- or a
        {"items": [...]} document, as File > Load Diagnosis JSON... reads.
        Anonymous instances can only be flagged by id_path (or an
        Instance). Pass [] to clear.
        """
        self._diagnosis_push = json.dumps(protocol.diagnosis_response(items))
        self._send_json(self._diagnosis_push)

    def show_instance(self, target):
        """Bring one hierarchical instance into view.

        `target`: a najaeda netlist.Instance, a list of instance ids or of
        instance names (top excluded; [] = the top design), or one name for
        a child of the top. Anonymous instances are reached by Instance or
        ids. The viewer opens the tree down to it, selects it, shows its
        properties, and draws it alone in the schematic with all its pins,
        ready to be extended pin by pin. Kept across view reloads.
        """
        names, ids = instance_paths(target)
        self._focus_push = json.dumps(protocol.focus_instance(names, ids))
        self._send_json(self._focus_push)

    @property
    def selected(self):
        """The instance selected in the viewer, as a najaeda netlist.Instance
        (the top design for a selected root), or None if nothing is selected
        or it no longer exists in the netlist."""
        if self.selected_id_path is not None:
            ids = self.selected_id_path
            if _ids_to_names(ids) is None:
                return None
        elif self.selected_path is not None:
            ids = _names_to_ids(self.selected_path)
            if ids is None:
                return None
        else:
            return None
        from najaeda import netlist
        return netlist.Instance(ids)

    def on_select(self, callback):
        """Call `callback(instance)` each time the viewer's selection changes
        (`instance` as returned by `selected`). Returns the traitlets handler,
        for `unobserve(handler, names="selected_id_path")`."""
        def handler(_change):
            callback(self.selected)
        self.observe(handler, names="selected_id_path")
        return handler


def show(instance=None, *, height=600, diagnosis=None):
    """Display a view of the design currently loaded with najaeda.

    Evaluate it as the last expression of a cell (or pass it to
    IPython.display.display). Requests are answered from the live netlist,
    so run show() again after editing the design to get a fresh view.
    `instance` starts the view on one instance, e.g. show(inst),
    show(["u1", "u2"]) or show([3, 0]) (see Schematic.show_instance).
    """
    # (anywidget itself turns on Colab's custom widget manager.)
    view = Schematic(height=height)
    if diagnosis is not None:
        view.annotate(diagnosis)
    if instance is not None:
        view.show_instance(instance)
    return view

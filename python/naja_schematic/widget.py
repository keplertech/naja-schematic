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
    # down from the top design; None if a name doesn't resolve.
    universe = naja.NLUniverse.get()
    design = universe.getTopDesign() if universe else None
    ids = []
    for name in names:
        inst = design.getInstance(name) if design else None
        if inst is None:
            return None
        ids.append(inst.getID())
        design = inst.getModel()
    return ids


def _ids_to_names(ids):
    universe = naja.NLUniverse.get()
    design = universe.getTopDesign() if universe else None
    names = []
    for inst_id in ids:
        inst = design.getInstanceByID(inst_id) if design else None
        if inst is None:
            raise ValueError(f"instance id path {list(ids)} does not resolve in the loaded design")
        names.append(inst.getName())
        design = inst.getModel()
    return names


def instance_path(target):
    """Normalize what show_instance() accepts to a list of instance names
    (top excluded): a najaeda netlist.Instance, a list/tuple of names, or a
    single name for a child of the top design. A string is one name, never
    split on "/": escaped names can contain it. The top design is []."""
    if hasattr(target, "pathIDs"):  # najaeda.netlist.Instance
        return _ids_to_names(target.pathIDs)
    if isinstance(target, str):
        return [target]
    if isinstance(target, (list, tuple)):
        return [str(name) for name in target]
    raise TypeError(f"expected a najaeda Instance, a list of names or a name, "
                    f"got {type(target).__name__}")


class Schematic(anywidget.AnyWidget):
    """An interactive naja-schematic view of the design in NLUniverse."""

    height = traitlets.Int(600).tag(sync=True)
    # Instance-name path (top excluded) of the instance selected in the
    # viewer -- clicked in its tree or schematic, or set by show_instance().
    # None until something is selected. Kernel-side only (not synced): the
    # viewer reports changes as instance_selected messages. Observe it with
    # on_select(), or traitlets' observe(..., names="selected_path").
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
            path = request.get("path")
            self.selected_path = list(path) if isinstance(path, list) else None
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

    def annotate(self, items):
        """Overlay diagnosis items on the view (and keep them across reloads).

        `items`: a list of diagnosis dicts -- kind ("instance"|"net"), path
        (instance names, root excluded), terminal (nets only), severity
        ("info"|"warning"|"error"), message, source -- or a
        {"items": [...]} document, as File > Load Diagnosis JSON... reads.
        Pass [] to clear.
        """
        self._diagnosis_push = json.dumps(protocol.diagnosis_response(items))
        self._send_json(self._diagnosis_push)

    def show_instance(self, target):
        """Bring one hierarchical instance into view.

        `target`: a najaeda netlist.Instance, a list of instance names (top
        excluded; [] = the top design), or one name for a child of the top.
        The viewer opens the tree down to it, selects it, shows its
        properties, and draws it alone in the schematic with all its pins,
        ready to be extended pin by pin. Kept across view reloads.
        """
        path = instance_path(target)
        if _names_to_ids(path) is None:
            raise ValueError(f"no instance {path!r} in the loaded design")
        self._focus_push = json.dumps(protocol.focus_instance(path))
        self._send_json(self._focus_push)

    @property
    def selected(self):
        """The instance selected in the viewer, as a najaeda netlist.Instance
        (the top design for a selected root), or None if nothing is selected
        or it no longer exists in the netlist."""
        if self.selected_path is None:
            return None
        ids = _names_to_ids(self.selected_path)
        if ids is None:
            return None
        from najaeda import netlist
        return netlist.Instance(ids)

    def on_select(self, callback):
        """Call `callback(instance)` each time the viewer's selection changes
        (`instance` as returned by `selected`). Returns the traitlets handler,
        for `unobserve(handler, names="selected_path")`."""
        def handler(_change):
            callback(self.selected)
        self.observe(handler, names="selected_path")
        return handler


def show(instance=None, *, height=600, diagnosis=None):
    """Display a view of the design currently loaded with najaeda.

    Evaluate it as the last expression of a cell (or pass it to
    IPython.display.display). Requests are answered from the live netlist,
    so run show() again after editing the design to get a fresh view.
    `instance` starts the view on one instance, e.g. show(inst) or
    show(["u1", "u2"]) (see Schematic.show_instance).
    """
    # (anywidget itself turns on Colab's custom widget manager.)
    view = Schematic(height=height)
    if diagnosis is not None:
        view.annotate(diagnosis)
    if instance is not None:
        view.show_instance(instance)
    return view

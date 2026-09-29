"""Jupyter/Colab/VSCode-notebook view of the loaded netlist (anywidget).

The viewer runs in the notebook's output area; its requests come back to
this kernel over the widget comm channel and are answered by
protocol.handle_request() against the live NLUniverse -- so a view shows
the netlist as the notebook has built or edited it so far.
"""
import functools

import anywidget
import traitlets

from ._bundle import STATIC_DIR, bundle_path
from .session import ViewerSession, instance_path, instance_paths, selected_instance  # noqa: F401


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
        # Requests are answered on the kernel thread, like the cells that
        # edit the design, so the session's default runner is enough.
        self._session = ViewerSession(push=self._send_json)
        self._session.on_select(self._on_viewer_selection)
        self.on_msg(self._on_viewer_message)

    def _send_json(self, message):
        self.send({"json": message})

    def _on_viewer_message(self, _widget, content, _buffers):
        message = content.get("json") if isinstance(content, dict) else None
        if not isinstance(message, str):
            return
        for reply in self._session.answer(message):
            self._send_json(reply)

    def _on_viewer_selection(self, ids, path):
        # Names first: on_select() observes the ids, and its callback may
        # read both.
        self.selected_path = path
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
        self._session.annotate(items)

    def show_instance(self, target):
        """Bring one hierarchical instance into view.

        `target`: a najaeda netlist.Instance, a list of instance ids or of
        instance names (top excluded; [] = the top design), or one name for
        a child of the top. Anonymous instances are reached by Instance or
        ids. The viewer opens the tree down to it, selects it, shows its
        properties, and draws it alone in the schematic with all its pins,
        ready to be extended pin by pin. Kept across view reloads.
        """
        self._session.show_instance(target)

    @property
    def selected(self):
        """The instance selected in the viewer, as a najaeda netlist.Instance
        (the top design for a selected root), or None if nothing is selected
        or it no longer exists in the netlist."""
        return selected_instance(self.selected_id_path, self.selected_path)

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

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


class Schematic(anywidget.AnyWidget):
    """An interactive naja-schematic view of the design in NLUniverse."""

    height = traitlets.Int(600).tag(sync=True)

    def __init__(self, **kwargs):
        # Read before AnyWidget.__init__, which turns it into a synced trait.
        # Not a class attribute: that would read the bundle at import time
        # and fail the import when no bundle is installed.
        self._esm = _widget_esm()
        super().__init__(**kwargs)
        self._diagnosis_push = None
        self.on_msg(self._on_viewer_message)

    def _send_json(self, message):
        self.send({"json": message})

    def _on_viewer_message(self, _widget, content, _buffers):
        message = content.get("json") if isinstance(content, dict) else None
        if not isinstance(message, str):
            return
        try:
            replies = protocol.handle_message(message)
        except json.JSONDecodeError as e:
            log.error("Ignoring malformed viewer request %r: %s", message, e)
            return
        for reply in replies:
            self._send_json(reply)
            # The viewer clears its diagnoses on every root_response, so
            # re-annotate after the root is (re)loaded.
            if self._diagnosis_push and '"root_response"' in reply:
                self._send_json(self._diagnosis_push)

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


def show(height=600, diagnosis=None):
    """Display a view of the design currently loaded with najaeda.

    Evaluate it as the last expression of a cell (or pass it to
    IPython.display.display). Requests are answered from the live netlist,
    so run show() again after editing the design to get a fresh view.
    """
    # (anywidget itself turns on Colab's custom widget manager.)
    view = Schematic(height=height)
    if diagnosis is not None:
        view.annotate(diagnosis)
    return view

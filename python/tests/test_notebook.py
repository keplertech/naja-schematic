"""Runs python/notebooks/colab_test.ipynb (the Open-in-Colab notebook) in a
real IPython kernel, so an API change that breaks it fails CI.

The kernel side is all that can run headless: there's no browser to render
the widget, so a cell spliced in mid-notebook plays the viewer against the
notebook's own `view` (requests in over the comm, replies captured).

By default it runs against the installed package (the wheel under test): the
notebook's install cell is skipped. With NAJA_SCHEMATIC_COLAB_CHECK set it
runs as Colab does instead: nothing preinstalled, the install cell fetches
the release from PyPI, which must be the version given (or any, for
"latest"). CI's `colab` job runs it that way, from outside python/ and with
--noconftest, since neither najaeda nor naja_schematic is installed yet.
"""
import json
import os
from pathlib import Path

import pytest

COLAB_VERSION = os.environ.get("NAJA_SCHEMATIC_COLAB_CHECK")

if COLAB_VERSION:
    # Nothing may skip here: a skipped check would pass the job.
    import nbclient
    import nbformat
    import ipykernel  # noqa: F401
else:
    nbformat = pytest.importorskip("nbformat")
    nbclient = pytest.importorskip("nbclient")
    pytest.importorskip("ipykernel")
    pytest.importorskip("anywidget")

NOTEBOOK = Path(__file__).parents[1] / "notebooks" / "colab_test.ipynb"
WIDGET_MIME = "application/vnd.jupyter.widget-view+json"

# Stands in for the viewer while the full adder is loaded: every request
# must be answered, and the viewer -> host selection must reach najaeda.
VIEWER_CHECK = r'''
import json
_sent = []
view.send = lambda content, buffers=None: _sent.append(json.loads(content["json"]))

def _ask(request):
    del _sent[:]
    view._on_viewer_message(view, {"json": json.dumps(request)}, [])
    return list(_sent)

replies = _ask({"request": "load_root"})
assert [r["response"] for r in replies] == ["root_response", "focus_instance"], replies
assert replies[0]["root"]["name"] == "fulladder", replies[0]
assert replies[1]["path"] == ["ha1", "carry_and"], replies[1]

(resolved,) = _ask({"request": "resolve_instance", "path": ["ha2", "sum_xor"]})
assert resolved["response"] == "instance_resolved" and resolved["found"], resolved

(props,) = _ask({"request": "get_properties", "kind": "instance", "path": ["ha1"]})
assert {"name": "Model", "value": "halfadder"} in props["properties"], props

assert _ask({"request": "instance_selected", "path": ["ha2"]}) == []
assert view.selected_path == ["ha2"]
assert view.selected.get_name() == "ha2"
'''


# Run right after the install cell when checking the PyPI release: what
# Colab got must be the expected version and carry the real viewer bundle.
RELEASE_CHECK = r'''
from naja_schematic._bundle import bundle_path
_expected = {expected!r}
assert _expected == "latest" or naja_schematic.__version__ == _expected, (
    naja_schematic.__version__, _expected)
assert bundle_path() is not None and bundle_path().stat().st_size > 100_000, bundle_path()
'''


@pytest.fixture
def notebook_env(tmp_path, monkeypatch):
    if COLAB_VERSION:
        monkeypatch.delenv("NAJA_SCHEMATIC_NOTEBOOK_TEST", raising=False)
        monkeypatch.delenv("NAJA_SCHEMATIC_BUNDLE", raising=False)
        return tmp_path
    from naja_schematic._bundle import bundle_path
    # Keeps the install cell from upgrading over the wheel under test.
    monkeypatch.setenv("NAJA_SCHEMATIC_NOTEBOOK_TEST", "1")
    # Without a built bundle (a source checkout), give the widget a stub:
    # nothing renders it here anyway. The kernel inherits the environment.
    if bundle_path() is None:
        stub = tmp_path / "naja-schematic.js"
        stub.write_text("var createNajaSchematic;")
        monkeypatch.setenv("NAJA_SCHEMATIC_BUNDLE", str(stub))
    return tmp_path


def splice_viewer_check(nb):
    # Right after the selection cells, before the design is edited.
    at = next(i for i, cell in enumerate(nb.cells)
              if cell.cell_type == "code" and "view.selected_path" in cell.source)
    nb.cells.insert(at + 1, nbformat.v4.new_code_cell(VIEWER_CHECK))


def test_colab_notebook_runs(notebook_env):
    nb = nbformat.read(NOTEBOOK, as_version=4)
    splice_viewer_check(nb)
    if COLAB_VERSION:
        install = next(i for i, cell in enumerate(nb.cells)
                       if cell.cell_type == "code")
        nb.cells.insert(install + 1, nbformat.v4.new_code_cell(
            RELEASE_CHECK.format(expected=COLAB_VERSION)))
    client = nbclient.NotebookClient(
        nb, kernel_name="python3", timeout=600,
        resources={"metadata": {"path": str(notebook_env)}})
    # Raises CellExecutionError, with the failing cell, on any exception.
    client.execute()

    code_cells = [c for c in nb.cells if c.cell_type == "code"]
    first = "".join(o.get("text", "") for o in code_cells[0].outputs)
    assert "naja-schematic" in first
    if not COLAB_VERSION:
        # The install cell must not have reinstalled over the package under test.
        assert "Successfully installed" not in first

    views = [c for c in code_cells
             if any(WIDGET_MIME in o.get("data", {}) for o in c.outputs)]
    shows = [c for c in code_cells if "show(" in c.source]
    assert len(views) == len(shows) >= 4

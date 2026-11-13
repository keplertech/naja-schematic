"""Headless SVG export (render.py): export_svg()/export_svgs() on the
session's design, `naja-schematic render`, and `--py-primitives`.

The drawing tests need the naja-schematic-render executable (platform
wheels, or $NAJA_SCHEMATIC_RENDER): skipped without one, unless
NAJA_SCHEMATIC_REQUIRE_RENDER=1 (CI), where a missing renderer fails."""
import json
import os
import subprocess
import sys
from pathlib import Path

import pytest

from naja_schematic import RenderError, export_svg, export_svgs
from naja_schematic import render

DATA = Path(__file__).parent / "data"


def _have_renderer():
    try:
        render.renderer_path()
        return True
    except RenderError:
        return False


needs_renderer = pytest.mark.skipif(
    not _have_renderer() and os.environ.get("NAJA_SCHEMATIC_REQUIRE_RENDER") != "1",
    reason="no naja-schematic-render (set NAJA_SCHEMATIC_RENDER)")


def _texts(svg):
    # The <text> contents: instance names, pin names, port flags.
    import re
    return set(re.findall(r">([^<>]+)</text>", svg))


# -- target resolution (no renderer) -------------------------------------------

def test_instance_text_is_matched_against_the_design(top):
    assert render.parse_instance_arg("u_sub") == [0]
    assert render.parse_instance_arg("u_sub/u_and") == [0, 0]
    assert render.parse_instance_arg("[0, 0]") == [0, 0]
    assert render.parse_instance_arg('["u_sub", "u_and"]') == [0, 0]
    assert render.parse_instance_arg('{"id_path": [0]}') == [0]
    with pytest.raises(ValueError, match="no instance"):
        render.parse_instance_arg("u_sub/nope")
    with pytest.raises(ValueError):
        render.parse_instance_arg("[")


def test_pin_text_forms(top):
    ids, term, bit = render.parse_pin_arg("u_sub/u_and:O")
    assert (list(ids), bit) == ([0, 0], None)
    assert render.parse_pin_arg("q")[0] == ()                 # a top-level port
    assert render.parse_pin_arg("d[1]")[2] == 1               # a bit of a top-level bus
    assert render.parse_pin_arg(":clk") == render.parse_pin_arg("clk")
    assert render.parse_pin_arg('{"path": ["u_sub"], "terminal": "a"}')[0] == (0,)
    with pytest.raises(ValueError, match="no pin"):
        render.parse_pin_arg("d")                              # a whole bus
    with pytest.raises(ValueError, match="no pin"):
        render.parse_pin_arg("u_sub:nope")
    with pytest.raises(ValueError, match="no pin"):
        render.parse_pin_arg("q[0]")                           # not a bus


def test_drawing_pushes(top):
    pushes = render.drawing_pushes(["u_sub", ("u_sub", "a")], instances=[["u_sub", "u_and"]],
                                   traces=[([], "q")])
    kinds = [p["response"] for p in pushes]
    assert kinds == ["design_changed", "focus_instance", "focus_instance", "show_net",
                     "show_net", "export_svg"]
    assert "add" not in pushes[1] and pushes[2]["add"] is True       # first alone, then added
    assert pushes[2]["id_path"] == [0, 0]
    assert "trace" not in pushes[3] and pushes[3]["label"] == "u_sub/a (net)"
    assert pushes[4]["trace"] is True and pushes[4]["label"] == "q"
    with pytest.raises(RenderError, match="nothing to draw"):
        render.drawing_pushes()
    with pytest.raises(ValueError):
        render.drawing_pushes(instances=["nope"])
    with pytest.raises(ValueError, match="top design"):
        render.drawing_pushes(instances=[[]])


def test_missing_renderer_is_explained(monkeypatch, tmp_path):
    monkeypatch.setenv("NAJA_SCHEMATIC_RENDER", str(tmp_path / "none"))
    with pytest.raises(RenderError, match="NAJA_SCHEMATIC_RENDER"):
        render.renderer_path()


# -- drawing --------------------------------------------------------------------

@needs_renderer
def test_export_svg_draws_instances_nets_and_traces(top, tmp_path):
    out = tmp_path / "view.svg"
    svg = export_svg(["r"], out, traces=[(["r"], "D")], nets=[([], "clk")])
    assert svg.startswith("<svg") or svg.startswith("<?xml")
    assert out.read_text() == svg
    texts = _texts(svg)
    # The flop, and the cone back through u_sub's LUT2 to the d ports.
    assert {"r", "u_and"} <= texts
    assert any(t.startswith("d") for t in texts)


@needs_renderer
def test_export_svgs_one_process_several_drawings(top, tmp_path):
    a, b = tmp_path / "a.svg", tmp_path / "b.svg"
    svgs = export_svgs([{"instances": ["r"], "path": a},
                        {"traces": [(["u_sub", "u_and"], "O")], "path": b}])
    assert len(svgs) == 2 and a.read_text() == svgs[0] and b.read_text() == svgs[1]
    # Each drawing starts from a clean view.
    assert "u_and" not in _texts(svgs[0]) and "r" not in _texts(svgs[1])
    assert "u_and" in _texts(svgs[1])


@needs_renderer
def test_a_bad_target_draws_and_writes_nothing(top, tmp_path):
    good, bad = tmp_path / "good.svg", tmp_path / "bad.svg"
    with pytest.raises(ValueError, match="job 1"):
        export_svgs([{"instances": ["r"], "path": good},
                     {"nets": [(["r"], "nope")], "path": bad}])
    assert not good.exists() and not bad.exists()


# -- the CLI, in its own process (its own universe) ---------------------------------

def _cli(*args, cwd=DATA):
    return subprocess.run([sys.executable, "-m", "naja_schematic", *args], cwd=cwd,
                          capture_output=True, text=True, timeout=120)


PY_DESIGN = ("--verilog", "py_design.v", "--py-primitives", "py_prims.py")


@needs_renderer
def test_render_cli_with_python_primitives(tmp_path):
    out = tmp_path / "cone.svg"
    result = _cli("render", *PY_DESIGN, "--trace", "u_ff:D", "--instance", "u_ff",
                  "--svg", str(out))
    assert result.returncode == 0, result.stderr
    texts = _texts(out.read_text())
    # The cone crosses both Python-defined AND gates back to the ports.
    assert {"u_ff", "u_and", "u_and2", "a", "b"} <= texts


@needs_renderer
def test_render_cli_fails_on_any_unresolved_target(tmp_path):
    out = tmp_path / "x.svg"
    result = _cli("render", *PY_DESIGN, "--trace", "y", "--net", "u_nope:A",
                  "--instance", "u_ff/x", "--svg", str(out))
    assert result.returncode != 0
    assert "--net u_nope:A" in result.stderr and "--instance u_ff/x" in result.stderr
    assert not out.exists()


def test_render_cli_needs_something_to_draw(tmp_path):
    result = _cli("render", *PY_DESIGN, "--svg", str(tmp_path / "x.svg"))
    assert result.returncode == 2 and "nothing to draw" in result.stderr


def test_serve_loads_python_primitives():
    requests = "\n".join(json.dumps({"request": "resolve_instance", "path": [name]})
                         for name in ("u_and", "u_ff")) + "\n"
    result = subprocess.run([sys.executable, "-m", "naja_schematic", *PY_DESIGN, "--stdio"],
                            cwd=DATA, input=requests, capture_output=True, text=True,
                            timeout=120)
    replies = [json.loads(line) for line in result.stdout.splitlines()]
    assert [r["instance"]["primitive_type"] for r in replies] == ["and", "dff"], result.stderr


def test_py_primitives_by_module_name(monkeypatch, tmp_path):
    from naja_schematic import server
    monkeypatch.syspath_prepend(str(DATA))
    calls = []
    module = type(sys)("fake_prims")
    module.load = calls.append
    monkeypatch.setitem(sys.modules, "fake_prims", module)
    server.load_python_primitives("fake_prims", "db")
    assert calls == ["db"]
    with pytest.raises(SystemExit, match="no file"):
        server.load_python_primitives(str(tmp_path / "missing.py"), "db")
    with pytest.raises(SystemExit, match="cannot import"):
        server.load_python_primitives("no_such_module_here", "db")

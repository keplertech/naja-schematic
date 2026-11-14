"""Headless SVG and PNG export (render.py): export_svg()/export_svgs()/
export_png() on the session's design, the view options (instance labels,
fanout grid and summaries), `naja-schematic render`, and `--py-primitives`.

The drawing tests need the naja-schematic-render executable (platform
wheels, or $NAJA_SCHEMATIC_RENDER): skipped without one, unless
NAJA_SCHEMATIC_REQUIRE_RENDER=1 (CI), where a missing renderer fails."""
import json
import os
import subprocess
import sys
from pathlib import Path

import pytest

from naja_schematic import RenderError, export_png, export_svg, export_svgs
from naja_schematic import protocol, render

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
    assert kinds == ["design_changed", "view_options", "focus_instance", "focus_instance",
                     "show_net", "show_net", "export_svg"]
    # Every drawing sets every view option: none carries over from the last.
    assert {k: v for k, v in pushes[1].items() if k != "response"} == \
        protocol.DEFAULT_VIEW_OPTIONS
    assert "add" not in pushes[2] and pushes[3]["add"] is True       # first alone, then added
    assert pushes[3]["id_path"] == [0, 0]
    assert "trace" not in pushes[4] and pushes[4]["label"] == "u_sub/a (net)"
    assert pushes[5]["trace"] is True and pushes[5]["label"] == "q"
    both = render.drawing_pushes(["r"], view_options={"instance_label": "both"}, png_scale=3)
    assert both[1]["instance_label"] == "both" and both[1]["max_sinks"] == 32
    assert [p["response"] for p in both[-2:]] == ["export_svg", "export_png"]
    assert both[-1]["scale"] == 3.0
    png_only = render.drawing_pushes(["r"], svg=False, png_scale=1)
    assert png_only[-1]["response"] == "export_png" and png_only[-2]["response"] != "export_svg"
    with pytest.raises(ValueError, match="instance_label"):
        render.drawing_pushes(["r"], view_options={"instance_label": "cell"})
    with pytest.raises(ValueError, match="scale"):
        render.drawing_pushes(["r"], png_scale=0)
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
    assert out.read_text(encoding="utf-8") == svg
    texts = _texts(svg)
    # The flop, and the cone back through u_sub's LUT2 to the d ports.
    assert {"r", "u_and"} <= texts
    assert any(t.startswith("d") for t in texts)


@needs_renderer
def test_export_svgs_one_process_several_drawings(top, tmp_path):
    a, b = tmp_path / "a.svg", tmp_path / "b.svg"
    svgs = export_svgs([{"instances": ["r"], "path": a},
                        {"traces": [(["u_sub", "u_and"], "O")], "path": b}])
    assert len(svgs) == 2 and a.read_text(encoding="utf-8") == svgs[0] and b.read_text(encoding="utf-8") == svgs[1]
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


def _png_size(data):
    assert data[:8] == b"\x89PNG\r\n\x1a\n" and data[12:16] == b"IHDR"
    return int.from_bytes(data[16:20], "big"), int.from_bytes(data[20:24], "big")


def _svg_size(svg):
    import re
    w, h = re.search(r'<svg[^>]* width="([\d.]+)" height="([\d.]+)"', svg).groups()
    return float(w), float(h)


@needs_renderer
def test_export_png_is_the_svg_drawing_scaled(top, tmp_path):
    import math
    import zlib
    svg = export_svg(["r"])
    out = tmp_path / "r.png"
    png = export_png(["r"], out, scale=2.5)
    assert out.read_bytes() == png
    w, h = _svg_size(svg)
    assert _png_size(png) == (math.ceil(w * 2.5), math.ceil(h * 2.5))
    # A valid zlib stream of filtered RGB rows, not all background.
    at, idat = 8, b""
    while at < len(png):
        n = int.from_bytes(png[at:at + 4], "big")
        if png[at + 4:at + 8] == b"IDAT":
            idat += png[at + 8:at + 8 + n]
        at += 12 + n
    rows = zlib.decompress(idat)
    pw, ph = _png_size(png)
    assert len(rows) == ph * (1 + 3 * pw)
    assert len(set(rows)) > 2


@needs_renderer
def test_instance_labels(top):
    names = _texts(export_svg(["r"]))
    assert "r" in names and "FDRE" not in names
    both = _texts(export_svg(["r"], view_options={"instance_label": "both"}))
    assert {"r", "FDRE"} <= both
    model = _texts(export_svg(["r"], view_options={"instance_label": "model"}))
    assert "FDRE" in model and "r" not in model


def test_session_keeps_view_options_across_reloads(top):
    from naja_schematic.session import ViewerSession
    sent = []
    session = ViewerSession(push=sent.append, view_options={"max_sinks": 4})
    session.view_options(instance_label="both")
    assert json.loads(sent[-1]) == {"response": "view_options", "instance_label": "both",
                                    "generation": 0}
    replies = [json.loads(r) for r in session.answer(json.dumps({"request": "load_root"}))]
    kept = [r for r in replies if r["response"] == "view_options"]
    assert len(kept) == 1 and kept[0]["max_sinks"] == 4 and kept[0]["instance_label"] == "both"
    with pytest.raises(ValueError):
        session.view_options(group_by="color")
    with pytest.raises(TypeError):
        session.view_options(colour="red")


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
    texts = _texts(out.read_text(encoding="utf-8"))
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


WIDE = ("--verilog", "wide_fanout.v", "--xilinx")


@needs_renderer
def test_render_cli_gathers_a_wide_fanout(tmp_path):
    svg_out, png_out = tmp_path / "net.svg", tmp_path / "net.png"
    result = _cli("render", *WIDE, "--net", "drv:O", "--max-sinks", "8", "--sink-rows", "4",
                  "--instance-label", "both", "--svg", str(svg_out), "--png", str(png_out),
                  "--scale", "2")
    assert result.returncode == 0, result.stderr
    svg = svg_out.read_text(encoding="utf-8")
    texts = _texts(svg)
    # The driver, the first 8 sinks, and the other 38 in two summaries
    # (the 6 LUTs are past the first 8 too): by model, the default.
    assert {"drv", "LUT1"} <= texts
    assert "FDRE \u00d732" in texts and "LUT1 \u00d76" in texts
    assert not {f"g{i}" for i in range(6)} & texts
    w, h = _svg_size(svg)
    assert _png_size(png_out.read_bytes()) == (int(w * 2 + 0.999), int(h * 2 + 0.999))

    by_module = tmp_path / "module.svg"
    result = _cli("render", *WIDE, "--net", "drv:O", "--max-sinks", "8", "--group-by",
                  "hierarchy", "--svg", str(by_module))
    assert result.returncode == 0, result.stderr
    texts = _texts(by_module.read_text(encoding="utf-8"))
    assert any(t.startswith("u_bank \u00d7") for t in texts)

    # No limit: every sink drawn.
    every = tmp_path / "every.svg"
    result = _cli("render", *WIDE, "--net", "drv:O", "--max-sinks", "0", "--svg", str(every))
    assert result.returncode == 0, result.stderr
    assert {f"g{i}" for i in range(6)} <= _texts(every.read_text(encoding="utf-8"))


def test_render_cli_output_flags(tmp_path):
    result = _cli("render", *PY_DESIGN, "--instance", "u_ff")
    assert result.returncode == 2 and "--svg and/or --png" in result.stderr
    result = _cli("render", *PY_DESIGN, "--instance", "u_ff", "--svg", str(tmp_path / "x.svg"),
                  "--scale", "2")
    assert result.returncode == 2 and "--scale applies to --png" in result.stderr
    result = _cli("render", *PY_DESIGN, "--instance", "u_ff", "--png", str(tmp_path / "x.png"),
                  "--group-by", "colour")
    assert result.returncode == 2 and "--group-by" in result.stderr

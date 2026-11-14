"""Host styling: style_response items, resolved by the session (style()),
kept across root loads, and drawn into SVG exports with no post-processing.

The drawing tests need the naja-schematic-render executable (see
test_render.py): skipped without one, unless NAJA_SCHEMATIC_REQUIRE_RENDER=1."""
import json
import os
import re
import subprocess
import sys
from pathlib import Path

import pytest

from naja_schematic import RenderError, protocol, render, style_response
from naja_schematic.session import ViewerSession

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


# -- items (no design needed) --------------------------------------------------

def test_colors_are_normalized():
    assert protocol.style_color("#FF8800") == "#ff8800"
    assert protocol.style_color((255, 136, 0)) == "#ff8800"
    assert protocol.style_color((255, 136, 0, 64)) == "#ff880040"
    assert protocol.style_color("#ff8800ff") == "#ff8800"
    with pytest.raises(ValueError):
        protocol.style_color("orange")
    with pytest.raises(TypeError):
        protocol.style_color((256, 0, 0))


def test_malformed_items_raise():
    with pytest.raises(ValueError, match="kind"):
        protocol.style_response([{"kind": "wire", "path": []}])
    with pytest.raises(ValueError, match="terminal or term_id"):
        protocol.style_response([{"kind": "net", "path": ["u1"]}])
    with pytest.raises(ValueError, match="unknown"):
        protocol.style_response([{"kind": "instance", "path": ["u1"], "colour": "#000000"}])
    with pytest.raises(ValueError, match="anonymous"):
        protocol.style_response([{"kind": "instance", "path": [""]}])
    with pytest.raises(ValueError, match="width"):
        protocol.style_response([{"kind": "instance", "path": ["u1"], "width": 0}])
    with pytest.raises(ValueError, match="list of instance names"):
        protocol.style_response([{"kind": "instance", "path": "u1/u2"}])


# -- resolved against the design (tests/data/design.v) -------------------------

def test_a_net_item_is_sent_on_its_driver(top):
    # u_sub/y is a hierarchical pin: the viewer draws the net's leaf pins
    # and top-level ports, so the item goes on its driver, u_sub/u_and.O,
    # whichever pin named it.
    expected = {"kind": "net", "id_path": [0, 0], "path": ["u_sub", "u_and"],
                "color": "#1f77b4"}
    for item in ({"kind": "net", "path": ["u_sub"], "terminal": "y", "color": "#1f77b4"},
                 {"kind": "net", "target": (["u_sub", "u_and"], "O"), "color": "#1f77b4"},
                 {"kind": "net", "target": (["r"], "D"), "color": (31, 119, 180)}):
        [got] = style_response([item])["items"]
        assert {k: got[k] for k in expected} == expected
        assert "term_id" in got and "bit" not in got


def test_a_bus_net_item_is_sent_per_bit_on_the_ports(top):
    items = style_response([{"kind": "net", "path": [], "terminal": "d", "width": 3}])["items"]
    assert [(i["id_path"], i.get("bit"), i["width"]) for i in items] == [([], 0, 3.0),
                                                                         ([], 1, 3.0)]
    [one] = style_response([{"kind": "net", "path": [], "terminal": "d", "bit": 1}])["items"]
    assert one["bit"] == 1


def test_instances_and_pins_resolve_or_raise(top):
    [inst] = style_response([{"target": ["u_sub", "u_and"], "fill": "#2ca02c40",
                               "label": "sink"}])["items"]
    assert inst == {"kind": "instance", "id_path": [0, 0], "path": ["u_sub", "u_and"],
                    "fill": "#2ca02c40", "label": "sink"}
    [pin] = style_response([{"kind": "term", "path": ["r"], "terminal": "D",
                             "color": "#9467bd"}])["items"]
    assert pin["terminal"] == "D" and pin["path"] == ["r"]
    with pytest.raises(ValueError, match="no instance"):
        style_response([{"path": ["nope"], "fill": "#ffffff"}])
    with pytest.raises(ValueError, match="no terminal"):
        style_response([{"kind": "term", "path": ["r"], "terminal": "nope"}])


def test_targets_in_the_text_and_json_forms(top):
    # What a style JSON file can say: the render CLI's --net/--instance text,
    # or a pin as [instance, terminal(, bit)] or {"path", "terminal", "bit"}.
    expected = {"kind": "net", "id_path": [0, 0], "path": ["u_sub", "u_and"]}
    for target in ("u_sub/u_and:O", "r:D", "u_sub:y", [["u_sub", "u_and"], "O"],
                   [[0, 0], "O"], ["r", "D"], {"path": ["r"], "terminal": "D"},
                   {"id_path": [0, 0], "terminal": "O"}):
        [got] = style_response([{"kind": "net", "target": target, "color": "#1f77b4"}])["items"]
        assert {k: got[k] for k in expected} == expected, target
    # A top-level port: its name alone, or [[], name(, bit)]. Sent on the
    # net's driver, which a trace's partial net lists too: the input port
    # itself, but r's Q for the output q (a receiver).
    for target in ("clk", [[], "clk"], ":clk"):
        [got] = style_response([{"kind": "net", "target": target}])["items"]
        assert got["id_path"] == [] and "bit" not in got, target
    for target in ("q", [[], "q"], ":q"):
        [got] = style_response([{"kind": "net", "target": target}])["items"]
        assert got["path"] == ["r"] and "bit" not in got, (target, got)
    for target in ("d[1]", [[], "d", 1], {"path": [], "terminal": "d", "bit": 1}):
        [got] = style_response([{"kind": "term", "target": target}])["items"]
        assert (got["id_path"], got["bit"]) == ([], 1), target
    # An instance: "u1/u2" text (matched against the design), or JSON text.
    for target in ("u_sub/u_and", '["u_sub", "u_and"]', "[0, 0]"):
        [got] = style_response([{"kind": "instance", "target": target}])["items"]
        assert (got["id_path"], got["path"]) == ([0, 0], ["u_sub", "u_and"]), target


def test_bad_targets_name_the_item(top):
    for target, error in (("u_sub/nope:O", "no pin"), ([[], "nope"], "no terminal"),
                          ("d", "a bus"), ([["nope"], "O"], "no instance")):
        with pytest.raises(ValueError, match=f"style item 0: target .*{error}"):
            style_response([{"kind": "net", "target": target}])
    with pytest.raises(ValueError, match="style item 1: target 'u_sub/x'.*no instance"):
        style_response([{"target": "u_sub"}, {"target": "u_sub/x"}])
    with pytest.raises(TypeError, match="style item 0"):
        style_response([{"kind": "net", "target": 3.5}])


def test_style_is_pushed_now_and_after_each_root(top):
    sent = []
    session = ViewerSession(push=sent.append)
    session.style([{"kind": "net", "path": [], "terminal": "q", "color": "#ff7f0e"}])
    assert [json.loads(m)["response"] for m in sent] == ["style_response"]
    replies = [json.loads(m) for m in session.answer(json.dumps({"request": "load_root"}))]
    assert [r["response"] for r in replies] == ["root_response", "style_response"]
    session.design_changed()
    replies = [json.loads(m) for m in session.answer(json.dumps({"request": "load_root"}))]
    assert [r["response"] for r in replies] == ["root_response"]  # it named the old design


# -- drawn, with no SVG post-processing ------------------------------------------

def _rgb(hexa):
    return "rgb(%d,%d,%d)" % tuple(int(hexa[i:i + 2], 16) for i in (1, 3, 5))


# The colors of a fanout-fix report: the enable net orange, each driver
# copy's net blue/green/purple, its sinks tinted in the same color.
COPIES = {"drv_0": "#1f77b4", "drv_1": "#2ca02c", "drv_2": "#9467bd"}
SINKS = {"drv_0": ["s0", "s1"], "drv_1": ["s2", "s3"], "drv_2": ["s4", "s5"]}
FANOUT_STYLE = {"items": [
    {"kind": "net", "path": [], "terminal": "en", "color": "#ff7f0e", "width": 3, "label": "en"},
    *({"kind": "net", "path": [drv], "terminal": "O", "color": color, "width": 2.5}
      for drv, color in COPIES.items()),
    *({"kind": "instance", "path": [sink], "fill": color + "55"}
      for drv, color in COPIES.items() for sink in SINKS[drv]),
]}


def _blend(hexa, alpha=0x55, under=(214, 216, 220)):
    # A translucent fill over the boxes' default fill, as the viewer tints it.
    a = alpha / 255
    rgb = [int(hexa[i:i + 2], 16) for i in (1, 3, 5)]
    return "rgb(%d,%d,%d)" % tuple(int(c * a + u * (1 - a) + 0.5) for c, u in zip(rgb, under))


@needs_renderer
def test_render_cli_draws_a_fanout_report_in_its_colors(tmp_path):
    style = tmp_path / "style.json"
    style.write_text(json.dumps(FANOUT_STYLE))
    out = tmp_path / "fanout.svg"
    result = subprocess.run(
        [sys.executable, "-m", "naja_schematic", "render", "--verilog", "fanout.v", "--xilinx",
         "--style", str(style), "--net", "en", *(a for d in COPIES for a in ("--net", f"{d}:O")),
         "--svg", str(out)],
        cwd=DATA, capture_output=True, text=True, timeout=120)
    assert result.returncode == 0, result.stderr
    svg = out.read_text()
    strokes = set(re.findall(r'stroke="(rgb\([^)]*\))" stroke-width="([\d.]+)"', svg))
    assert (_rgb("#ff7f0e"), "3") in strokes
    for color in COPIES.values():
        assert (_rgb(color), "2.5") in strokes
    fills = set(re.findall(r'fill="(rgb\([^)]*\))"', svg))
    for color in COPIES.values():
        assert _blend(color) in fills
    assert ">en</text>" in svg


@needs_renderer
def test_render_cli_style_file_targets(tmp_path):
    # The file form documented in the README: targets as --net/--instance
    # text and as a JSON [path, terminal] list.
    style = tmp_path / "style.json"
    style.write_text(json.dumps({"items": [
        {"kind": "net", "target": [[], "en"], "color": "#ff7f0e", "width": 3},
        {"kind": "net", "target": "drv_0:O", "color": "#1f77b4", "width": 2.5},
        {"kind": "instance", "target": "s0", "fill": "#1f77b455"},
    ]}))
    out = tmp_path / "fanout.svg"
    result = subprocess.run(
        [sys.executable, "-m", "naja_schematic", "render", "--verilog", "fanout.v", "--xilinx",
         "--style", str(style), "--net", "en", "--net", "drv_0:O", "--svg", str(out)],
        cwd=DATA, capture_output=True, text=True, timeout=120)
    assert result.returncode == 0, result.stderr
    svg = out.read_text()
    strokes = set(re.findall(r'stroke="(rgb\([^)]*\))" stroke-width="([\d.]+)"', svg))
    assert (_rgb("#ff7f0e"), "3") in strokes and (_rgb("#1f77b4"), "2.5") in strokes
    assert _blend("#1f77b4") in set(re.findall(r'fill="(rgb\([^)]*\))"', svg))


@needs_renderer
def test_render_cli_refuses_a_bad_style(tmp_path):
    style = tmp_path / "style.json"
    style.write_text(json.dumps([{"kind": "net", "path": ["nope"], "terminal": "O"}]))
    out = tmp_path / "x.svg"
    result = subprocess.run(
        [sys.executable, "-m", "naja_schematic", "render", "--verilog", "fanout.v", "--xilinx",
         "--style", str(style), "--net", "en", "--svg", str(out)],
        cwd=DATA, capture_output=True, text=True, timeout=120)
    assert result.returncode != 0 and "no instance" in result.stderr
    assert not out.exists()

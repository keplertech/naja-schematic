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
from najaeda import naja
from protocol_cases import build_design, check_case, load_spec, substitute

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


# -- SVG structure and export decorations ------------------------------------------

def _groups(svg, cls):
    import xml.etree.ElementTree as ET
    ns = "{http://www.w3.org/2000/svg}"
    root = ET.fromstring(svg.encode("utf-8"))  # well-formed XML, or this raises
    return [g for g in root.iter(ns + "g") if g.get("class") == cls]


def _group_texts(g):
    return {t.text for t in g.iter("{http://www.w3.org/2000/svg}text")}


@needs_renderer
def test_svg_groups_name_every_net_instance_and_port(top):
    svg = export_svg(["r"], traces=[(["r"], "D")], nets=[([], "clk")])
    instances = {g.get("data-path"): g for g in _groups(svg, "instance")}
    # The flop and the LUT the cone crosses, by path, ids and model.
    assert {"r", "u_sub/u_and"} <= set(instances)
    r = instances["r"]
    assert r.get("data-id-path") == ",".join(map(str, render.parse_instance_arg("r")))
    assert r.get("data-model") == "FDRE"
    assert "r" in _group_texts(r) and "D" in _group_texts(r)  # its name and pins
    lut = instances["u_sub/u_and"]
    assert lut.get("data-id-path") == ",".join(map(str, render.parse_instance_arg("u_sub/u_and")))
    assert lut.get("data-model") == "LUT2"
    assert lut.get("data-path-json") is None  # plain names: no JSON needed
    # The module frame around the LUT.
    assert "u_sub" in {g.get("data-path") for g in _groups(svg, "module")}

    # Every top-level port flag drawn.
    ports = {g.get("data-name"): g.get("data-direction") for g in _groups(svg, "port")}
    assert ports.get("clk") == "input" and {"d[0]", "d[1]"} <= set(ports), ports

    # Every net is a group naming its driver, holding its wires.
    nets = _groups(svg, "net")
    assert nets and all(g.get("data-driver") for g in nets)
    drivers = {g.get("data-driver"): g for g in nets}
    assert "clk" in drivers                                   # a top-level port
    assert "u_sub/u_and:O" in drivers                         # an instance pin
    assert drivers["u_sub/u_and:O"].get("data-driver-id-path") == \
        lut.get("data-id-path")
    assert all(any(e.tag.endswith(("line", "polyline")) for e in g.iter()) for g in nets)


@needs_renderer
def test_net_names_across_hierarchy_and_bus_bits(top):
    svg = export_svg(["r"], traces=[(["r"], "D")], nets=[([], "clk")])
    nets = {g.get("data-driver"): g.get("data-net") for g in _groups(svg, "net")}
    # u_and drives sub's n (named at its own level, not top's s); a bus
    # port's bit names its bit net.
    assert nets["u_sub/u_and:O"] == "n"
    assert nets["clk"] == "clk"
    assert nets["d[0]"] == "d[0]" and nets["d[1]"] == "d[1]"


@needs_renderer
def test_png_text_has_arrows_and_symbols(top):
    # A character no font has is drawn as the replacement glyph: a label is
    # drawn right when its PNG differs from that.
    def png(text):
        return export_png(["r"], title=text)
    missing = png("a  b")
    for symbol in "→←↔⇒≤≥≠±µΩ°…×αΔ✓":
        assert png(f"a {symbol} b") != missing, f"U+{ord(symbol):04X} has no glyph"


@needs_renderer
def test_export_decorations(top, tmp_path):
    import math
    plain = export_svg(["r"], nets=[([], "clk")])
    assert not _groups(plain, "title") and not _groups(plain, "legend")
    decorated = export_svg(["r"], nets=[([], "clk")], title="Clock of r",
                           caption="line 1\nline 2", legend=True)
    assert _group_texts(_groups(decorated, "title")[0]) == {"Clock of r"}
    assert _group_texts(_groups(decorated, "caption")[0]) == {"line 1", "line 2"}
    entries = [g.get("data-label") for g in _groups(decorated, "legend-entry")]
    assert entries == ["clk (net)"], entries
    assert _svg_size(decorated)[1] > _svg_size(plain)[1] + 30
    # The drawing itself is the same: same groups.
    for cls in ("net", "instance", "port"):
        assert len(_groups(decorated, cls)) == len(_groups(plain, cls))
    # The PNG has the same page.
    png = export_png(["r"], nets=[([], "clk")], title="Clock of r", caption="line 1\nline 2",
                     legend=True)
    w, h = _svg_size(decorated)
    assert _png_size(png) == (math.ceil(w), math.ceil(h))
    # hide_ui_glyphs: r's open pins lose their stubs.
    hidden = export_svg(["r"], nets=[([], "clk")], hide_ui_glyphs=True)
    def circles(svg):
        return sum(1 for g in _groups(svg, "instance") for e in g.iter() if e.tag.endswith("circle"))
    assert circles(hidden) == 0 < circles(plain)
    # export_svgs takes them per job.
    svgs = export_svgs([{"instances": ["r"], "title": "A"}, {"instances": ["r"]}])
    assert _groups(svgs[0], "title") and not _groups(svgs[1], "title")


def test_decoration_view_options():
    message = protocol.view_options(title="T", caption="", legend=True, hide_ui_glyphs=False)
    assert message == {"response": "view_options", "title": "T", "caption": "", "legend": True,
                       "hide_ui_glyphs": False}
    for bad in ({"legend": "yes"}, {"hide_ui_glyphs": 1}, {"title": 3}):
        with pytest.raises(ValueError):
            protocol.view_options(bad)
    assert set(protocol.EXPORT_DECORATIONS) <= set(protocol.DEFAULT_VIEW_OPTIONS)


def test_decoration_flags():
    from naja_schematic.server import add_view_arguments, view_options_from_args
    import argparse
    parser = argparse.ArgumentParser()
    add_view_arguments(parser)
    assert view_options_from_args(parser.parse_args([])) is None
    args = parser.parse_args(["--title", "T", "--caption", "C", "--legend", "--hide-ui-glyphs"])
    assert view_options_from_args(args) == {"title": "T", "caption": "C", "legend": True,
                                            "hide_ui_glyphs": True}


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
    # The driver, the first 8 sinks in natural name order (g0..g5, s0, s1),
    # and the other 38 flops (s2..s9, u_bank's 30) in one summary: by model,
    # the default.
    assert {"drv", "LUT1", "s0", "s1"} <= texts
    assert {f"g{i}" for i in range(6)} <= texts
    assert "FDRE \u00d738" in texts and "s2" not in texts
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


# -- group_by "role": the receiving pin's naja term role ------------------------

ROLE_SPEC = load_spec("role_protocol_cases.json")


@pytest.fixture(scope="module")
def roles(top):
    # NLUniverse is process-global: build next to the session's design and
    # make it the top design for this module only.
    u = naja.NLUniverse.get()
    previous = u.getTopDesign()
    designs = build_design(ROLE_SPEC["design"])
    u.setTopDesign(designs[ROLE_SPEC["design"]["top"]])
    yield designs
    u.setTopDesign(previous)
    designs[ROLE_SPEC["design"]["top"]].getDB().destroy()


@pytest.mark.parametrize("case", ROLE_SPEC["cases"], ids=[c["name"] for c in ROLE_SPEC["cases"]])
def test_term_roles_shared_protocol_case(roles, case):
    replies = protocol.handle_request(substitute(case["request"], roles))
    check_case(case, json.loads(json.dumps(replies)))


def test_term_role_names_cover_every_naja_role():
    # Every non-data role naja has is sent by name (termRoleName() in C++).
    data = {"DataInput", "DataOutput", "Other"}
    naja_roles = {r for r in dir(naja.SNLTermRole) if not r.startswith("_")} - data
    assert naja_roles <= set(protocol._TERM_ROLE_NAMES)


def test_group_by_values():
    assert protocol.view_options(group_by="role")["group_by"] == "role"
    assert protocol.view_options(group_by="pin")["group_by"] == "pin"
    with pytest.raises(ValueError, match="group_by"):
        protocol.view_options(group_by="name")


ROLE_DESIGN = ("--verilog", "role_fanout.v", "--py-primitives", "py_prims.py")


def _summaries(texts):
    # {"clock": 12, ...} from the "<what> ×<n>" summary labels.
    out = {}
    for t in texts:
        what, sep, n = t.rpartition(" \u00d7")
        if sep and n.isdigit():
            out[what] = int(n)
    return out


@needs_renderer
def test_render_cli_groups_sinks_by_role(tmp_path):
    # 34 sinks: 12 clock, 6 reset, 5 enable pins of flops, and 7 flop D
    # pins plus 4 AND inputs with no role (data). 2 drawn, 32 gathered.
    out = tmp_path / "role.svg"
    result = _cli("render", *ROLE_DESIGN, "--net", "drv:Y", "--max-sinks", "2",
                  "--group-by", "role", "--svg", str(out))
    assert result.returncode == 0, result.stderr
    groups = _summaries(_texts(out.read_text(encoding="utf-8")))
    assert set(groups) == {"clock", "reset", "enable", "data"}, groups
    assert sum(groups.values()) == 32
    assert 10 <= groups["clock"] <= 12 and 4 <= groups["reset"] <= 6
    assert 3 <= groups["enable"] <= 5 and 9 <= groups["data"] <= 11

    # "pin": by pin name, the clock pins together.
    out = tmp_path / "pin.svg"
    result = _cli("render", *ROLE_DESIGN, "--net", "drv:Y", "--max-sinks", "2",
                  "--group-by", "pin", "--svg", str(out))
    assert result.returncode == 0, result.stderr
    groups = _summaries(_texts(out.read_text(encoding="utf-8")))
    assert set(groups) == {"clock", "R", "L", "D", "A"}, groups
    assert sum(groups.values()) == 32


def test_render_cli_output_flags(tmp_path):
    result = _cli("render", *PY_DESIGN, "--instance", "u_ff")
    assert result.returncode == 2 and "--svg and/or --png" in result.stderr
    result = _cli("render", *PY_DESIGN, "--instance", "u_ff", "--svg", str(tmp_path / "x.svg"),
                  "--scale", "2")
    assert result.returncode == 2 and "--scale applies to --png" in result.stderr
    result = _cli("render", *PY_DESIGN, "--instance", "u_ff", "--png", str(tmp_path / "x.png"),
                  "--group-by", "colour")
    assert result.returncode == 2 and "--group-by" in result.stderr



def test_convergence_outline_option():
    assert protocol.DEFAULT_VIEW_OPTIONS["convergence_outline"] is True
    assert protocol.view_options(convergence_outline=False)["convergence_outline"] is False
    with pytest.raises(ValueError):
        protocol.view_options(convergence_outline="no")
    from naja_schematic.server import add_view_arguments, view_options_from_args
    import argparse
    parser = argparse.ArgumentParser()
    add_view_arguments(parser)
    assert view_options_from_args(parser.parse_args(["--no-convergence-outline"])) == \
        {"convergence_outline": False}
    assert "applied when this flag is not given" in " ".join(parser.format_help().split())


# -- a fanout fix report: net labels, legend, net names, symbols ---------------

FIX = ("--verilog", "fanout_fix.v", "--xilinx")
FIX_NETS = ("--net", "en", "--net", "en_reg:Q", "--net", "en_reg_fo1:Q", "--net", "en_reg_fo2:Q",
            "--style", "fanout_fix_style.json")


def _floats(text):
    import re
    return [float(v) for v in re.findall(r"-?\d+(?:\.\d+)?(?:e-?\d+)?", text or "")]


def _bbox(e):
    # An element's box in SVG coordinates (x0, y0, x1, y1), or None.
    tag = e.tag.rsplit("}", 1)[-1]
    f = lambda k: float(e.get(k))  # noqa: E731
    if tag == "rect":
        return f("x"), f("y"), f("x") + f("width"), f("y") + f("height")
    if tag == "line":
        return min(f("x1"), f("x2")), min(f("y1"), f("y2")), max(f("x1"), f("x2")), max(f("y1"), f("y2"))
    if tag == "circle":
        return f("cx") - f("r"), f("cy") - f("r"), f("cx") + f("r"), f("cy") + f("r")
    if tag in ("polygon", "polyline", "path"):
        v = _floats(e.get("points") if tag != "path" else e.get("d"))
        xs, ys = v[0::2], v[1::2]
        return (min(xs), min(ys), max(xs), max(ys)) if xs and ys else None
    if tag == "text":
        # y is the baseline: the glyphs go about one font size above it.
        size = f("font-size")
        return f("x"), f("y") - size, f("x") + f("textLength"), f("y") + 0.25 * size
    return None


def _overlap(a, b, eps=0.5):
    return a[0] < b[2] - eps and b[0] < a[2] - eps and a[1] < b[3] - eps and b[1] < a[3] - eps


def _label_boxes(svg):
    # Each net group's style label: its <text> holding data-label.
    boxes = {}
    for g in _groups(svg, "net"):
        label = g.get("data-label")
        for t in g.iter("{http://www.w3.org/2000/svg}text"):
            if label and t.text == label:
                boxes[label] = _bbox(t)
    return boxes


def _assert_labels_clear(svg, labels):
    boxes = _label_boxes(svg)
    assert set(labels) <= set(boxes), boxes
    shapes = [(g.get("data-path") or g.get("data-name"), _bbox(e))
              for cls in ("instance", "port") for g in _groups(svg, cls) for e in g.iter()
              if _bbox(e) is not None]
    for label, box in boxes.items():
        hits = [name for name, b in shapes if _overlap(box, b)]
        assert not hits, f"{label!r} at {box} overlaps {hits}"
    # Nor do two labels overlap.
    items = list(boxes.items())
    for i, (la, a) in enumerate(items):
        assert not any(_overlap(a, b) for _, b in items[i + 1:]), la


@needs_renderer
def test_net_labels_never_sit_under_a_box_or_a_port_flag(tmp_path):
    out = tmp_path / "fix.svg"
    result = _cli("render", *FIX, *FIX_NETS, "--instance-label", "both", "--hide-ui-glyphs",
                  "--svg", str(out))
    assert result.returncode == 0, result.stderr
    _assert_labels_clear(out.read_text(encoding="utf-8"),
                         ["en (shared input)", "en_reg → 4 loads", "en_reg_fo1 → 4 loads",
                          "en_reg_fo2 → 4 loads"])

    # Gate symbols, whose output is level with their name's row.
    style = tmp_path / "style.json"
    style.write_text(json.dumps({"items": [
        {"kind": "net", "target": "u_and:Y", "color": "#d62728", "label": "first AND, a long label"},
        {"kind": "net", "target": "u_and2:Y", "color": "#2ca02c", "label": "second AND"},
        {"kind": "net", "target": "a", "color": "#1f77b4", "label": "input a"}]}), encoding="utf-8")
    out = tmp_path / "gates.svg"
    result = _cli("render", *PY_DESIGN, "--trace", "u_ff:D", "--net", "a", "--style", str(style),
                  "--svg", str(out))
    assert result.returncode == 0, result.stderr
    _assert_labels_clear(out.read_text(encoding="utf-8"),
                         ["first AND, a long label", "second AND", "input a"])


def _color(e):
    return e.get("stroke") if e.get("stroke") not in (None, "none") else e.get("fill")


@needs_renderer
def test_legend_lists_the_colors_drawn(tmp_path):
    out = tmp_path / "fix.svg"
    result = _cli("render", *FIX, *FIX_NETS, "--legend", "--svg", str(out))
    assert result.returncode == 0, result.stderr
    svg = out.read_text(encoding="utf-8")
    entries = [(g.get("data-label"),
                next(_color(e) for e in g.iter() if e.tag.endswith("line")))
               for g in _groups(svg, "legend-entry")]
    # One entry per net: the style's label and color, not also the trace's
    # (the style color is what every wire shows).
    assert [label for label, _ in entries] == [
        "en (shared input)", "en_reg → 4 loads", "en_reg_fo1 → 4 loads",
        "en_reg_fo2 → 4 loads"]
    drawn = {_color(e) for g in _groups(svg, "net") for e in g.iter() if e.tag.endswith("line")}
    assert {color for _, color in entries} == drawn
    assert len(set(entries)) == len(entries)

    # Without a style, each net is its trace: its label and color.
    result = _cli("render", *FIX, "--net", "en", "--net", "en_reg:Q", "--legend", "--svg", str(out))
    assert result.returncode == 0, result.stderr
    svg = out.read_text(encoding="utf-8")
    labels = [g.get("data-label") for g in _groups(svg, "legend-entry")]
    assert labels == ["en (net)", "en_reg/Q (net)"]
    drawn = {_color(e) for g in _groups(svg, "net") for e in g.iter() if e.tag.endswith("line")}
    legend = {next(_color(e) for e in g.iter() if e.tag.endswith("line"))
              for g in _groups(svg, "legend-entry")}
    assert legend == drawn


@needs_renderer
def test_net_groups_are_named_by_their_net(tmp_path):
    out = tmp_path / "fix.svg"
    result = _cli("render", *FIX, *FIX_NETS, "--svg", str(out))
    assert result.returncode == 0, result.stderr
    nets = {g.get("data-driver"): g.get("data-net")
            for g in _groups(out.read_text(encoding="utf-8"), "net")}
    # The net, not the driver's pin (which stays data-driver).
    assert nets == {"en": "en", "en_reg:Q": "en_q", "en_reg_fo1:Q": "en_q_fo1",
                    "en_reg_fo2:Q": "en_q_fo2"}

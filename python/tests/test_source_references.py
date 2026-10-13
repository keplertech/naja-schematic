"""Source references across the protocol: the has_sources flags, get_sources
and load_source (see docs/source_information.md).

The request/reply cases live in tests/data/source_protocol_cases.json,
shared with tests/provider/LocalSNLProviderProtocolTest.cpp, so protocol.py
and LocalSNLProvider are held to the same answers.
"""
import json
import os
import time

import pytest
from najaeda import naja

from naja_schematic import protocol
from naja_schematic.protocol import handle_request
from naja_schematic.server import build_arg_parser, main
from naja_schematic.session import ViewerSession
from protocol_cases import CASES_DIR, check_case, load_spec, load_verilog_design, substitute

SPEC = load_spec("source_protocol_cases.json")
SOURCE_ROOT = str(CASES_DIR / SPEC["source_root"])
SOURCE_PATHS = protocol.SourcePaths(
    (SOURCE_ROOT,), tuple((prefix, str(CASES_DIR / to)) for prefix, to in SPEC["source_map"]))


@pytest.fixture(scope="module")
def sources(top):
    # NLUniverse is process-global: build next to the session's design and
    # make it the top design for this module only.
    u = naja.NLUniverse.get()
    previous = u.getTopDesign()
    designs = load_verilog_design(SPEC["design"], SOURCE_ROOT)
    u.setTopDesign(designs[SPEC["design"]["top"]])
    yield designs
    u.setTopDesign(previous)


@pytest.mark.parametrize("case", SPEC["cases"], ids=[c["name"] for c in SPEC["cases"]])
def test_shared_protocol_case(sources, case):
    replies = handle_request(substitute(case["request"], sources), SOURCE_PATHS)
    # Through JSON, like the wire, so both sides compare the same thing.
    check_case(case, json.loads(json.dumps(replies)))


class _NoSources:
    def hasSourceReferences(self):
        return False

    def hasSourceLoc(self):
        return False

    def getSourceDeclaration(self):
        return None


def test_flag_is_only_sent_when_set():
    assert "has_sources" not in protocol.with_sources({}, protocol.has_sources(_NoSources()))
    assert protocol.with_sources({}, True) == {"has_sources": True}


def test_resolved_instance_and_schematic_boxes_carry_the_flag(sources):
    (resolved,) = handle_request({"request": "resolve_instance", "id_path": [0, 0]})
    assert resolved["instance"]["has_sources"] is True
    # g0's output on net y: one box, for g0.
    (net,) = handle_request({"request": "load_equipotential", "path": [0, 0], "term_id": 0})
    (occ,) = net["occurrences"]
    assert occ["path"][-1][0] == "g0" and occ["has_sources"] is True
    assert "source_loc" not in occ


class _LegacyOnly:
    # What an object loaded by a frontend predating typed references
    # answers: nothing but the unclassified legacy location.
    def hasSourceReferences(self):
        return False

    def getSourceDeclaration(self):
        return None

    def getSourceOrigins(self):
        return []

    def hasSourceLoc(self):
        return True

    def getSourceLoc(self):
        return ("a.sv", 3, 5, 4, 0)


def test_legacy_location_when_no_typed_reference():
    assert protocol.object_sources(_LegacyOnly()) == [
        {"role": "legacy", "file": "a.sv", "line": 3, "column": 5, "end_line": 4,
         "end_column": 0, "provider": "", "language": "", "representation": ""}]
    assert protocol.has_sources(_LegacyOnly())


def test_resolve_source_file(tmp_path, monkeypatch):
    first, second = tmp_path / "first", tmp_path / "second"
    (first / "rtl").mkdir(parents=True)
    (second / "rtl").mkdir(parents=True)
    (second / "rtl" / "a.v").write_text("second")
    (first / "rtl" / "b.v").write_text("first")
    (second / "rtl" / "b.v").write_text("second")
    roots = protocol.SourcePaths((str(first), str(second)))
    assert protocol.resolve_source_file("rtl/a.v", roots) == str(second / "rtl" / "a.v")
    assert protocol.resolve_source_file("rtl/b.v", roots) == str(first / "rtl" / "b.v")  # in order
    absolute = str(second / "rtl" / "a.v")
    assert protocol.resolve_source_file(absolute, protocol.SourcePaths((str(first),))) == absolute
    assert protocol.resolve_source_file("rtl/c.v", roots) is None
    assert protocol.resolve_source_file("", roots) is None
    monkeypatch.chdir(second)
    assert protocol.resolve_source_file("rtl/a.v") == "rtl/a.v"  # the current directory last


def test_source_maps(tmp_path):
    checkout = tmp_path / "checkout"
    (checkout / "rtl").mkdir(parents=True)
    (checkout / "rtl" / "a.v").write_text("x")
    local = str(checkout / "rtl" / "a.v")
    maps = lambda *m: protocol.SourcePaths(maps=m)  # noqa: E731
    # A prefix of the recorded path, at a path boundary, with or without "/".
    assert protocol.resolve_source_file("/synth/flow/rtl/a.v", maps(("/synth/flow", str(checkout)))) == local
    assert protocol.resolve_source_file("/synth/flow/rtl/a.v", maps(("/synth/flow/", str(checkout) + "/"))) == local
    # Not in the middle of a name: /synth/flowX is another directory.
    assert protocol.resolve_source_file("/synth/flowX/rtl/a.v", maps(("/synth/flow", str(checkout)))) is None
    # In order; a map that leads nowhere falls through to the next one.
    assert protocol.resolve_source_file(
        "/synth/flow/rtl/a.v", maps(("/synth", "/nowhere"), ("/synth/flow", str(checkout)))) == local
    # Relative paths too, and a map is tried before roots.
    assert protocol.resolve_source_file(
        "build/rtl/a.v", protocol.SourcePaths(("/nowhere",), (("build", str(checkout)),))) == local


def test_large_file_sends_the_lines_around_the_reference(tmp_path, monkeypatch):
    path = tmp_path / "big.v"
    path.write_text("".join(f"line {n}\n" for n in range(1, 2001)))
    monkeypatch.setattr(protocol, "MAX_SOURCE_BYTES", 100)
    monkeypatch.setattr(protocol, "SOURCE_CONTEXT_LINES", 10)
    text, first_line, truncated = protocol.read_source_window(str(path), 1000, 1002)
    assert truncated and first_line == 990
    assert text.splitlines() == [f"line {n}" for n in range(990, 1013)]
    # Near the top, the window starts at line 1; an unknown end is the line.
    text, first_line, _ = protocol.read_source_window(str(path), 3, 0)
    assert first_line == 1 and text.splitlines()[-1] == "line 13"


def test_session_reads_sources_under_its_roots(sources):
    request = json.dumps({"request": "load_source", "kind": "instance",
                          "id_path": [0, 0], "role": "declaration"})
    (reply,) = ViewerSession(source_roots=[SOURCE_ROOT]).answer(request)
    assert json.loads(reply)["found"] is True
    (reply,) = ViewerSession().answer(request)
    assert json.loads(reply)["found"] is False


def test_cli_source_roots():
    args = build_arg_parser().parse_args(
        ["--verilog", "d.v", "--source-root", "rtl", "--source-root", "/abs"])
    assert args.source_roots == ["rtl", "/abs"]
    assert build_arg_parser().parse_args(["--verilog", "d.v"]).source_roots == []


def test_stale_files(sources, tmp_path):
    # A file modified after the design was loaded is reported stale: its
    # ranges were recorded before the change.
    (tmp_path / "rtl").mkdir()
    copy = tmp_path / "rtl" / "alu.v"
    copy.write_text((CASES_DIR / "sources" / "rtl" / "alu.v").read_text())
    request = json.dumps({"request": "load_source", "kind": "instance",
                          "id_path": [0, 0], "role": "origin", "index": 0})
    session = ViewerSession(source_roots=[str(tmp_path)])
    stale = lambda: json.loads(session.answer(request)[0])["stale"]  # noqa: E731
    assert stale() is False
    later = time.time() + 5
    os.utime(copy, (later, later))
    assert stale() is True
    # design_changed() starts a new design: its ranges are newer than the file.
    os.utime(copy, (time.time(), time.time()))
    session.design_changed()
    assert stale() is False
    # Unknown load time (a bare SourcePaths): never stale.
    (reply,) = handle_request(json.loads(request), protocol.SourcePaths((str(tmp_path),)))
    assert reply["found"] is True and reply["stale"] is False


def test_cli_source_maps():
    args = build_arg_parser().parse_args(
        ["--verilog", "d.v", "--source-map", "/synth/flow=/work/flow", "--source-map", "a=b=c"])
    assert args.source_maps == [("/synth/flow", "/work/flow"), ("a", "b=c")]  # split at the first "="
    for bad in ("nomap", "=/work"):
        with pytest.raises(SystemExit):
            build_arg_parser().parse_args(["--verilog", "d.v", "--source-map", bad])


def test_session_source_maps(sources):
    request = json.dumps({"request": "load_source", "kind": "instance",
                          "id_path": [0, 2], "role": "origin", "index": 0})
    (reply,) = ViewerSession(source_maps=[("/synth/machine", SOURCE_ROOT)]).answer(request)
    assert json.loads(reply)["found"] is True
    (reply,) = ViewerSession().answer(request)
    assert json.loads(reply)["found"] is False


def test_cli_yosys_src_is_opt_in():
    assert build_arg_parser().parse_args(["--verilog", "d.v", "--yosys-src"]).yosys_src
    assert not build_arg_parser().parse_args(["--verilog", "d.v"]).yosys_src
    with pytest.raises(SystemExit):
        main(["--sv", "a.sv", "--yosys-src"])

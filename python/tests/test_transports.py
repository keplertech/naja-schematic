import asyncio
import io
import json
import logging
import socket
import subprocess
import sys
import urllib.request
from pathlib import Path

import pytest

from naja_schematic import server

DIAGNOSIS = [{"kind": "instance", "path": ["u_sub"], "severity": "warning",
              "message": "check me", "source": "test"}]


def test_stdio_answers_and_pushes_diagnosis_after_root(top):
    stdin = io.StringIO(json.dumps({"request": "load_root"}) + "\n\nnot json\n"
                        + json.dumps({"request": "bogus"}) + "\n")
    stdout = io.StringIO()
    server.serve_stdio(DIAGNOSIS, stdin=stdin, stdout=stdout)
    replies = [json.loads(line) for line in stdout.getvalue().splitlines()]
    assert [r["response"] for r in replies] == ["root_response", "diagnosis_response"]
    assert replies[1]["items"] == DIAGNOSIS


def _free_port():
    with socket.socket() as s:
        s.bind(("localhost", 0))
        return s.getsockname()[1]


def test_websocket_server_serves_page_and_protocol(top, tmp_path, monkeypatch):
    websockets = pytest.importorskip("websockets")
    from websockets.asyncio.client import connect

    bundle = tmp_path / "naja-schematic.js"
    bundle.write_text("var createNajaSchematic = () => Promise.resolve({});")
    monkeypatch.setenv("NAJA_SCHEMATIC_BUNDLE", str(bundle))
    port = _free_port()

    async def scenario():
        task = asyncio.create_task(server.serve("localhost", port, DIAGNOSIS))
        base = f"http://localhost:{port}"
        for _ in range(100):  # wait for the listener
            try:
                with socket.create_connection(("localhost", port)):
                    break
            except OSError:
                await asyncio.sleep(0.05)

        def get(path):
            try:
                with urllib.request.urlopen(base + path) as r:
                    return r.status, r.headers["Content-Type"], r.read()
            except urllib.error.HTTPError as e:
                return e.code, None, e.read()

        status, ctype, body = await asyncio.to_thread(get, "/")
        assert status == 200 and ctype.startswith("text/html") and b"createNajaSchematic" in body
        status, _, body = await asyncio.to_thread(get, "/naja-schematic.js")
        assert status == 200 and body == bundle.read_bytes()
        status, _, _ = await asyncio.to_thread(get, "/../server.py")
        assert status == 404

        async with connect(f"ws://localhost:{port}/ws") as ws:
            await ws.send(json.dumps({"request": "load_root"}))
            first = json.loads(await ws.recv())
            second = json.loads(await ws.recv())
        task.cancel()
        return first, second

    first, second = asyncio.run(scenario())
    assert first["response"] == "root_response"
    assert second == {"response": "diagnosis_response", "items": DIAGNOSIS, "generation": 0}


def test_cli_rejects_liberty_with_systemverilog():
    with pytest.raises(SystemExit):
        server.main(["--sv", "a.sv", "--liberty", "x.lib"])


@pytest.mark.parametrize("argv", [
    ["--vhdl", "a.vhd", "--verilog", "b.v"],
    ["--vhdl", "a.vhd", "--sv", "b.sv"],
    ["--vhdl", "a.vhd", "--liberty", "cells.lib"],
    ["--vhdl", "a.vhd", "--yosys-src"],
    ["--vhdl", "a.vhd", "-D", "X"],
    ["--verilog", "b.v", "--top", "t"],
])
def test_cli_rejects_flags_that_do_not_apply(argv, capsys):
    with pytest.raises(SystemExit) as e:
        server.main(argv)
    assert e.value.code == 2


@pytest.mark.parametrize("flag, files, top", [
    ("--vhdl", ["vhdl/leaf.vhd", "vhdl/top.vhd"], "top"),
    ("--sv", ["sv/operators.sv"], "ops"),
    ("--sv", ["sv/two_tops.sv"], "second"),
])
def test_cli_loads_design(flag, files, top, tmp_path):
    # In a subprocess: loading replaces the session's design. Run from
    # tmp_path, where naja writes its diagnostics report.
    data = Path(__file__).parents[2] / "tests" / "data"
    argv = [flag, *(str(data / f) for f in files), "--top", top]
    script = (
        "import sys\n"
        "from naja_schematic import server\n"
        "args = server.build_arg_parser().parse_args(sys.argv[1:])\n"
        "print(server.load_design(args).getName())\n")
    run = subprocess.run([sys.executable, "-c", script, *argv], cwd=tmp_path,
                         capture_output=True, text=True)
    assert run.returncode == 0, run.stderr
    assert run.stdout.strip().splitlines()[-1] == top


def test_load_progress_logs_each_stage_and_long_ones_periodically(caplog):
    now = [0.0]
    progress = server.LoadProgress("Verilog", interval=2.0, clock=lambda: now[0])
    with caplog.at_level(logging.INFO, logger="naja_schematic"):
        assert progress(stage="parse", detail="big.v", current=0, total=0) is True
        now[0] = 1.0
        progress(stage="parse", detail="big.v", current=10, total=0)   # too soon
        now[0] = 3.5
        progress(stage="parse", detail="big.v", current=20, total=0)
        progress(stage="construct", detail="", current=0, total=40)
        now[0] = 6.0
        progress(stage="construct", detail="top", current=30, total=40)
        progress(stage="done", detail="", current=0, total=0)          # not logged
    assert [r.getMessage() for r in caplog.records] == [
        "Verilog: parsing (big.v)",
        "Verilog: parsing, 20 (big.v)",
        "Verilog: building the netlist",
        "Verilog: building the netlist, 30/40 (top)",
    ]


def test_cli_load_reports_progress(tmp_path):
    data = Path(__file__).parents[2] / "tests" / "data"
    script = (
        "import logging, sys\n"
        "from naja_schematic import server\n"
        "logging.basicConfig(stream=sys.stderr, format='%(message)s', level=logging.INFO)\n"
        "args = server.build_arg_parser().parse_args(sys.argv[1:])\n"
        "server.load_design(args)\n")
    run = subprocess.run([sys.executable, "-c", script, "--sv", str(data / "sv/operators.sv")],
                         cwd=tmp_path, capture_output=True, text=True)
    assert run.returncode == 0, run.stderr
    for stage in ("parsing", "elaborating", "building the netlist"):
        assert f"SystemVerilog: {stage}" in run.stderr

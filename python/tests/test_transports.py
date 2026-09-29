import asyncio
import io
import json
import socket
import urllib.request

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
    assert second == {"response": "diagnosis_response", "items": DIAGNOSIS}


def test_cli_rejects_liberty_with_systemverilog():
    with pytest.raises(SystemExit):
        server.main(["--sv", "a.sv", "--liberty", "x.lib"])

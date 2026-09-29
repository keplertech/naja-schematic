import json
import socket
import threading
import time
import urllib.error
import urllib.request

import pytest

pytest.importorskip("websockets")
from websockets.exceptions import ConnectionClosed, InvalidStatus  # noqa: E402
from websockets.sync.client import connect  # noqa: E402

from naja_schematic import ViewerServer  # noqa: E402
from naja_schematic.session import ViewerSession  # noqa: E402

LOAD_ROOT = json.dumps({"request": "load_root"})


@pytest.fixture
def bundle(tmp_path, monkeypatch):
    path = tmp_path / "naja-schematic.js"
    path.write_text("var createNajaSchematic = () => Promise.resolve({});")
    monkeypatch.setenv("NAJA_SCHEMATIC_BUNDLE", str(path))
    return path


@pytest.fixture
def server(top, bundle):
    with ViewerServer() as srv:
        yield srv


def ws_url(srv, token=None):
    return f"ws://127.0.0.1:{srv.port}/ws?token={token or srv.token}"


def recv(ws, timeout=5):
    return json.loads(ws.recv(timeout=timeout))


def test_port_zero_binds_a_free_port_and_serves_the_page(server, bundle):
    assert server.running and server.port != 0
    assert server.url == f"http://127.0.0.1:{server.port}/?token={server.token}"
    with urllib.request.urlopen(server.url) as r:
        assert b"createNajaSchematic" in r.read()
    with urllib.request.urlopen(f"http://127.0.0.1:{server.port}/naja-schematic.js") as r:
        assert r.read() == bundle.read_bytes()


def test_answers_from_the_loaded_design(server):
    with connect(ws_url(server)) as ws:
        ws.send(LOAD_ROOT)
        assert recv(ws)["root"]["name"] == "top"


@pytest.mark.parametrize("token", ["", "wrong"])
def test_rejects_a_bad_token(server, token):
    url = f"ws://127.0.0.1:{server.port}/ws" + (f"?token={token}" if token else "")
    with pytest.raises(InvalidStatus) as e:
        connect(url)
    assert e.value.response.status_code == 403


def test_rejects_other_origins(top, bundle):
    with ViewerServer(allowed_origins=["vscode-webview://abc"]) as srv:
        with pytest.raises(InvalidStatus):
            connect(ws_url(srv), origin="https://evil.example")
        for origin in (f"http://127.0.0.1:{srv.port}", f"http://localhost:{srv.port}",
                       "vscode-webview://abc"):
            with connect(ws_url(srv), origin=origin) as ws:
                ws.send(LOAD_ROOT)
                assert recv(ws)["response"] == "root_response"


def test_token_can_be_turned_off(top, bundle):
    with ViewerServer(token=None) as srv:
        assert "token" not in srv.url
        with connect(f"ws://127.0.0.1:{srv.port}/ws") as ws:
            ws.send(LOAD_ROOT)
            assert recv(ws)["response"] == "root_response"


def test_pushes_reach_every_viewer_and_follow_each_root(server):
    items = [{"kind": "instance", "path": ["u_sub"], "severity": "error", "message": "m"}]
    with connect(ws_url(server)) as a, connect(ws_url(server)) as b:
        # Both connected: wait until the server has registered them.
        for ws in (a, b):
            ws.send(LOAD_ROOT)
            assert recv(ws)["response"] == "root_response"
        server.annotate(items)
        server.show_instance(["u_sub", "u_and"])
        for ws in (a, b):
            assert recv(ws) == {"response": "diagnosis_response", "items": items}
            assert recv(ws) == {"response": "focus_instance", "path": ["u_sub", "u_and"],
                                "id_path": [0, 0]}
        a.send(LOAD_ROOT)
        assert [recv(a)["response"] for _ in range(3)] == [
            "root_response", "diagnosis_response", "focus_instance"]


def test_requests_wait_for_the_host_lock(server):
    with connect(ws_url(server)) as ws:
        with server.lock:
            ws.send(LOAD_ROOT)
            with pytest.raises(TimeoutError):
                ws.recv(timeout=0.3)
        assert recv(ws)["response"] == "root_response"


def test_host_lock_and_run_hooks(top, bundle):
    lock = threading.Lock()
    with ViewerServer(lock=lock) as srv:
        assert srv.lock is lock
        with connect(ws_url(srv)) as ws:
            with lock:
                ws.send(LOAD_ROOT)
                with pytest.raises(TimeoutError):
                    ws.recv(timeout=0.3)
            assert recv(ws)["response"] == "root_response"

    calls = []

    def run(fn):
        calls.append(threading.current_thread().name)
        return fn()

    with ViewerServer(run=run) as srv:
        assert srv.lock is None
        with connect(ws_url(srv)) as ws:
            ws.send(LOAD_ROOT)
            assert recv(ws)["response"] == "root_response"
    assert len(calls) == 1 and calls[0].startswith("naja-schematic-request")

    with pytest.raises(ValueError):
        ViewerServer(lock=lock, run=run)


def test_selection_callback_gets_ids_and_names_and_may_take_the_lock(server):
    seen = threading.Event()
    selections = []

    def on_select(id_path, path):
        with server.lock:  # no lock is held here: no deadlock
            selections.append((id_path, path))
        seen.set()

    server.on_select(on_select)
    with connect(ws_url(server)) as ws:
        # A viewer that only sends names: the ids are found by them.
        ws.send(json.dumps({"request": "instance_selected", "path": ["u_sub", "u_and"]}))
        assert seen.wait(5)
    assert selections == [([0, 0], ["u_sub", "u_and"])]
    assert server.selected_id_path == [0, 0]
    assert server.selected.get_model_name() == "LUT2"


def test_stop_closes_viewers_and_frees_the_port(top, bundle):
    srv = ViewerServer().start()
    port = srv.port
    with connect(ws_url(srv)) as ws:
        started = time.monotonic()
        srv.stop()
        assert time.monotonic() - started < 2
        srv.stop()  # twice is fine
        assert not srv.running
        with pytest.raises(ConnectionClosed):
            ws.recv(timeout=5)
    with pytest.raises(urllib.error.URLError):
        urllib.request.urlopen(f"http://127.0.0.1:{port}/", timeout=2)
    srv.annotate([])  # a push with nobody to send to is a no-op


def test_stop_does_not_wait_for_a_request_stuck_in_the_host(top, bundle):
    release = threading.Event()

    def run(fn):
        release.wait(10)
        return fn()

    srv = ViewerServer(run=run).start()
    try:
        with connect(ws_url(srv)) as ws:
            ws.send(LOAD_ROOT)
            time.sleep(0.2)  # let the request reach `run`
            started = time.monotonic()
            srv.stop()
            assert time.monotonic() - started < 2
            assert not srv.running
            with pytest.raises(ConnectionClosed):
                ws.recv(timeout=5)
    finally:
        release.set()


def test_start_reports_a_port_in_use(top, bundle):
    with socket.socket() as busy:
        busy.bind(("127.0.0.1", 0))
        busy.listen()
        with pytest.raises(OSError):
            ViewerServer(port=busy.getsockname()[1]).start()


def test_session_ignores_malformed_messages(top):
    session = ViewerSession()
    assert session.answer("not json") == []
    assert session.answer("[1, 2]") == []
    assert session.answer(json.dumps({"request": "bogus"})) == []


def test_design_changed_reaches_every_viewer(server):
    cleared = []
    server.on_select(lambda ids, path: cleared.append((ids, path)))
    server.annotate([{"kind": "instance", "path": ["u_sub"], "severity": "error", "message": "old"}])
    with connect(ws_url(server)) as a, connect(ws_url(server)) as b:
        for ws in (a, b):
            ws.send(LOAD_ROOT)
            assert [recv(ws)["response"] for _ in range(2)] == ["root_response",
                                                                "diagnosis_response"]
        a.send(json.dumps({"request": "instance_selected", "id_path": [0], "path": ["u_sub"]}))
        a.send(LOAD_ROOT)  # in order after the selection: wait for it
        assert recv(a)["response"] == "root_response" and recv(a)
        server.design_changed(instance=["u_sub"])
        for ws in (a, b):
            assert recv(ws) == {"response": "design_changed"}
        # The old diagnoses are gone; the new focus follows the root.
        b.send(LOAD_ROOT)
        assert [recv(b)["response"] for _ in range(2)] == ["root_response", "focus_instance"]
    assert server.selected_id_path is None
    assert cleared == [([0], ["u_sub"]), (None, None)]

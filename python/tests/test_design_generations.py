"""Design generations: nothing a viewer asked about or selected in a design
the host has since replaced may reach the replacement.

The test netlist is loaded once per process (NLUniverse is global), so these
tests "replace" it with itself: every id path the viewer knew still resolves
afterwards. That is the hard case -- a replacement design that reuses
instance ids -- since only the generation, not a failed lookup, can tell the
stale request apart.
"""
import json
import threading

import pytest

from naja_schematic.session import ViewerSession

U_AND = {"id_path": [0, 0], "path": ["u_sub", "u_and"]}


def request(name, generation=None, **fields):
    message = {"request": name, **fields}
    if generation is not None:
        message["generation"] = generation
    return json.dumps(message)


def select(generation=None, target=U_AND):
    return request("instance_selected", generation, **target)


def properties(generation=None):
    return request("get_properties", generation, kind="instance", **U_AND)


def decoded(replies):
    return [json.loads(r) for r in replies]


class ManualDispatch:
    """A session `dispatch` that holds notifications until flush(), so a test
    decides exactly when they are delivered."""

    def __init__(self):
        self.queue = []

    def __call__(self, fn):
        self.queue.append(fn)

    def flush(self):
        while self.queue:
            self.queue.pop(0)()


@pytest.fixture
def session(top):
    pushed, selections, dispatch = [], [], ManualDispatch()
    s = ViewerSession(push=lambda m: pushed.append(json.loads(m)), dispatch=dispatch)
    s.on_select(lambda ids, path: selections.append((ids, path)))
    s.pushed, s.selections, s.dispatch = pushed, selections, dispatch
    return s


# -- every message carries its generation ------------------------------------

def test_replies_and_pushes_carry_the_generation(session):
    (root,) = decoded(session.answer(request("load_root")))
    assert root["generation"] == 0
    assert session.design_changed() == 1
    assert session.pushed[-1] == {"response": "design_changed", "generation": 1}
    session.annotate([])
    assert session.pushed[-1]["generation"] == 1
    replies = decoded(session.answer(request("load_root", 1)))
    assert [(r["response"], r["generation"]) for r in replies] == [
        ("root_response", 1), ("diagnosis_response", 1)]


# -- delayed selections -------------------------------------------------------

def test_a_selection_made_for_the_old_design_is_dropped(session):
    session.answer(select(0))
    session.dispatch.flush()
    assert session.selections == [([0, 0], ["u_sub", "u_and"])]

    session.design_changed()
    session.dispatch.flush()
    # Arrives late, naming ids the new design still has: dropped anyway.
    assert session.answer(select(0)) == []
    session.dispatch.flush()
    assert session.selected_id_path is None and session.selected is None
    assert session.selections == [([0, 0], ["u_sub", "u_and"]), (None, None)]

    session.answer(select(1))  # made for the current design
    session.dispatch.flush()
    assert session.selected_id_path == [0, 0]
    assert session.selections[-1] == ([0, 0], ["u_sub", "u_and"])


def test_a_selection_notification_queued_before_design_changed_is_not_delivered(session):
    session.answer(select(0))  # accepted, notification not delivered yet
    session.design_changed()
    session.dispatch.flush()
    # The host never hears of the old design's selection after replacing it.
    assert session.selections == [(None, None)]
    assert session.selected_id_path is None


def test_a_selection_answered_just_before_design_changed_is_not_committed(top):
    # The host replaces the design between the moment the selection is
    # checked (inside run) and the moment it is recorded.
    selections = []
    holder = {}

    def run(fn):
        result = fn()
        holder["session"].design_changed()
        return result

    session = holder["session"] = ViewerSession(run=run)
    session.on_select(lambda ids, path: selections.append((ids, path)))
    session.answer(select(0))
    assert session.selected_id_path is None
    assert selections == []


def test_an_unstamped_selection_is_taken_for_the_current_design(session):
    # A viewer predating generations: nothing to check it against.
    session.design_changed()
    session.answer(select())
    session.dispatch.flush()
    assert session.selections == [([0, 0], ["u_sub", "u_and"])]


# -- queued requests ----------------------------------------------------------

def test_queries_made_for_the_old_design_get_no_answer(session):
    (reply,) = decoded(session.answer(properties(0)))
    assert reply["response"] == "properties_response" and reply["properties"]

    session.design_changed()
    assert session.answer(properties(0)) == []
    assert session.answer(request("load_terms", 0, gui_id=3,
                                  design_ref={"db_id": 1, "library_id": 1, "design_id": 1})) == []
    assert session.answer(request("load_root", 0)) == []

    (reply,) = decoded(session.answer(properties(1)))
    assert reply["generation"] == 1 and reply["properties"]


def test_a_root_for_the_new_design_brings_only_the_new_pushes(session):
    session.annotate([{"kind": "instance", "path": ["u_sub"], "severity": "error", "message": "old"}])
    session.design_changed(instance=["u_sub", "u_and"])
    replies = decoded(session.answer(request("load_root", 1)))
    assert [(r["response"], r["generation"]) for r in replies] == [
        ("root_response", 1), ("focus_instance", 1)]


def test_a_malformed_generation_is_rejected(session):
    assert session.answer(request("load_root", "zero")) == []
    assert session.answer(request("load_root", True)) == []


# -- through ViewerServer: requests queued behind the host lock, several viewers

websockets = pytest.importorskip("websockets")
from websockets.sync.client import connect  # noqa: E402

from naja_schematic import ViewerServer  # noqa: E402


class WatchedLock:
    """A reentrant lock that sets `contended` when a thread has to wait for
    it -- how a test knows a viewer request is queued behind the host."""

    def __init__(self):
        self._lock = threading.RLock()
        self.contended = threading.Event()

    def __enter__(self):
        if not self._lock.acquire(blocking=False):
            self.contended.set()
            self._lock.acquire()
        return self

    def __exit__(self, *exc):
        self._lock.release()


class Selections:
    def __init__(self):
        self.calls = []
        self._cond = threading.Condition()

    def __call__(self, ids, path):
        with self._cond:
            self.calls.append((ids, path))
            self._cond.notify_all()

    def wait_for(self, n, timeout=5):
        with self._cond:
            assert self._cond.wait_for(lambda: len(self.calls) >= n, timeout), self.calls
        return list(self.calls)


@pytest.fixture
def bundle(tmp_path, monkeypatch):
    path = tmp_path / "naja-schematic.js"
    path.write_text("var createNajaSchematic;")
    monkeypatch.setenv("NAJA_SCHEMATIC_BUNDLE", str(path))


def viewer(server):
    ws = connect(f"ws://127.0.0.1:{server.port}/ws?token={server.token}")
    ws.send(request("load_root"))
    assert recv(ws)["response"] == "root_response"
    return ws


def recv(ws):
    return json.loads(ws.recv(timeout=5))


def test_requests_queued_behind_the_swap_are_dropped(top, bundle):
    lock = WatchedLock()
    selections = Selections()
    with ViewerServer(lock=lock) as server, viewer(server) as ws:
        server.on_select(selections)
        with server.replacing_design():
            # Made for generation 0, they wait for the lock while the host
            # replaces the design and starts generation 1.
            ws.send(properties(0))
            ws.send(select(0))
            assert lock.contended.wait(5)
        assert server.generation == 1

        assert recv(ws) == {"response": "design_changed", "generation": 1}
        ws.send(request("load_root", 1))
        # Requests are answered in order: had either stale one been answered,
        # its reply would come before this root.
        assert (recv(ws)["response"], server.selected_id_path) == ("root_response", None)
        ws.send(select(1))
        assert selections.wait_for(1) == [([0, 0], ["u_sub", "u_and"])]


def test_a_replacement_failing_midway_starts_no_generation(top, bundle):
    with ViewerServer() as server:
        with pytest.raises(RuntimeError):
            with server.replacing_design():
                raise RuntimeError("load failed")
        assert server.generation == 0

    with ViewerServer(run=lambda fn: fn()) as server:
        with pytest.raises(RuntimeError, match="needs a lock"):
            with server.replacing_design():
                pass


def test_each_viewer_is_held_to_the_current_generation(top, bundle):
    selections = Selections()
    with ViewerServer() as server, viewer(server) as fast, viewer(server) as slow:
        server.on_select(selections)
        with server.replacing_design():
            pass
        for ws in (fast, slow):
            assert recv(ws) == {"response": "design_changed", "generation": 1}

        # `slow` acts on the old design before processing design_changed:
        # its selection and query are dropped. Its reload proves the worker
        # got past them (one worker, requests answered in order).
        slow.send(select(0))
        slow.send(properties(0))
        slow.send(request("load_root", 1))
        assert recv(slow)["response"] == "root_response"
        assert server.selected_id_path is None

        # `fast` reloaded first and selects in the new design.
        fast.send(request("load_root", 1))
        assert recv(fast)["response"] == "root_response"
        fast.send(select(1))
        assert selections.wait_for(1) == [([0, 0], ["u_sub", "u_and"])]
        assert server.selected_id_path == [0, 0]

        # A second replacement: `fast`'s selection goes, both reload again.
        server.design_changed()
        assert selections.wait_for(2)[-1] == (None, None)
        for ws in (fast, slow):
            assert recv(ws) == {"response": "design_changed", "generation": 2}

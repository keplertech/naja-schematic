"""Netlist reviews (docs/review_format.md): the directory format, anchor
resolution and the viewer's review_* requests."""
import json

import pytest
from najaeda import naja

from naja_schematic.review import Review, ReviewError, resolve_anchor, snapshot_hash
from naja_schematic.session import ViewerSession

# What a viewer sends: ids, plus names it happens to know.
U_AND = {"kind": "instance", "id_path": [0, 0], "path": ["u_sub", "u_and"]}
# What an agent knowing only names sends: located once, at write time.
PIN_A = {"kind": "term", "path": ["u_sub"], "terminal": "a"}
TOP_D1 = {"kind": "term", "path": [], "terminal": "d", "bit": 1}


def design():
    return naja.NLUniverse.get().getTopDesign()


def sub():
    return design().getInstanceByID(0).getModel()


def anonymous_nets(owner):
    return [n for n in owner.getNets() if not n.getName()]


@pytest.fixture
def review(top, tmp_path):
    return Review(tmp_path / ".naja-review", "Jane")


def files(directory):
    return sorted(str(p.relative_to(directory)) for p in directory.rglob("*") if p.is_file())


def test_thread_files_and_folding(review):
    tid = review.add_thread(design(), U_AND, "LUT looks wrong", severity="error",
                            labels=["lut"], title="u_and")
    review.add_event(tid, text="Agreed", author="John")
    review.add_event(tid, status="resolved")

    names = files(review.directory)
    assert "review.json" in names
    assert ".gitattributes" in names
    assert f"threads/{tid}/thread.json" in names
    assert len([n for n in names if n.startswith(f"threads/{tid}/")]) == 4
    assert any(n.startswith("snapshots/") and n.endswith("snl.mf") for n in names)

    [thread] = review.response(design())["threads"]
    assert thread["status"] == "resolved"
    assert thread["severity"] == "error"
    assert thread["labels"] == ["lut"]
    assert [(c["author"], c["text"]) for c in thread["comments"]] == [
        ("Jane", "LUT looks wrong"), ("John", "Agreed")]
    assert thread["snapshot_stored"] is True
    assert thread["resolution"] == {"state": "exact", "id_path": [0, 0],
                                    "path": ["u_sub", "u_and"]}


def test_files_are_never_rewritten(review):
    tid = review.add_thread(design(), PIN_A, "first")
    before = {n: (review.directory / n).read_bytes() for n in files(review.directory)}
    review.add_event(tid, text="second")
    after = {n: (review.directory / n).read_bytes() for n in files(review.directory)}
    assert {n: after[n] for n in before} == before
    assert len(after) == len(before) + 1


def test_two_writers_never_share_a_file(top, tmp_path):
    # Two clones writing on two branches: their files never collide, so the
    # merge is a union of the two directories.
    a = Review(tmp_path / "a", "A")
    b = Review(tmp_path / "b", "B")
    a.add_thread(design(), U_AND, "from A")
    b.add_thread(design(), U_AND, "from B")
    shared = set(files(a.directory)) & set(files(b.directory))
    # Only the review's metadata, and the snapshot of the design both wrote
    # on: same netlist, same content-addressed files, byte for byte.
    assert {n for n in shared if not n.startswith("snapshots/")} == {"review.json",
                                                                    ".gitattributes"}
    assert all((a.directory / n).read_bytes() == (b.directory / n).read_bytes()
               for n in shared)


def test_stored_anchor_is_ids_with_name_labels(review):
    tid = review.add_thread(design(), PIN_A, "x")
    stored = json.loads((review.directory / "threads" / tid / "thread.json").read_text())
    assert stored["anchor"] == {"kind": "term", "id_path": [0], "path": ["u_sub"],
                                "term_id": sub().getTerm("a").getID(), "terminal": "a"}
    snapshot = stored["snapshot"]
    assert snapshot == snapshot_hash(review.snapshot_path(snapshot))
    assert review.latest_snapshot() == snapshot


def test_stored_names_are_only_labels(review):
    # A stored anchor is located by its ids: wrong labels don't matter...
    tid = review.add_thread(design(), PIN_A, "x")
    path = review.directory / "threads" / tid / "thread.json"
    document = json.loads(path.read_text())
    document["anchor"].update(path=["renamed"], terminal="renamed")
    path.write_text(json.dumps(document))
    [thread] = review.response(design())["threads"]
    assert thread["resolution"] == {"state": "exact", "id_path": [0], "path": ["u_sub"]}
    # ...and names alone never locate a stored anchor.
    del document["anchor"]["term_id"]
    document["anchor"]["terminal"] = "a"
    path.write_text(json.dumps(document))
    [thread] = review.response(design())["threads"]
    assert thread["resolution"] == {"state": "unresolved"}


def test_bus_bit_anchor(review):
    review.add_thread(design(), TOP_D1, "d[1]")
    [thread] = review.response(design())["threads"]
    assert thread["anchor"]["term_id"] == design().getTerm("d").getID()
    assert thread["anchor"]["bit"] == 1
    assert thread["resolution"]["state"] == "exact"
    with pytest.raises(ReviewError):
        review.add_thread(design(), {**TOP_D1, "bit": 7}, "no such bit")


def test_anonymous_net_by_id(review):
    # The unnamed tie-off nets can't be named: only an id reaches one.
    net = anonymous_nets(design())[1]
    tid = review.add_thread(design(), {"kind": "net", "id_path": [], "net_id": net.getID()},
                            "tie-off")
    [thread] = review.response(design())["threads"]
    assert thread["id"] == tid
    assert thread["anchor"] == {"kind": "net", "id_path": [], "path": [],
                                "net_id": net.getID(), "net": ""}
    assert thread["resolution"]["state"] == "exact"


def test_anonymous_instance_by_id(review):
    # u_sub's assign is an anonymous instance (id 1): no name path reaches it.
    assert sub().getInstanceByID(1).getName() == ""
    review.add_thread(design(), {"kind": "instance", "id_path": [0, 1]}, "assign")
    [thread] = review.response(design())["threads"]
    assert thread["anchor"]["path"] == ["u_sub", ""]
    assert thread["resolution"] == {"state": "exact", "id_path": [0, 1], "path": ["u_sub", ""]}
    with pytest.raises(ReviewError):
        review.add_thread(design(), {"kind": "instance", "path": ["u_sub", ""]}, "x")


@pytest.mark.parametrize("anchor", [
    {"kind": "instance", "path": ["nope"]},
    {"kind": "instance", "id_path": [9]},
    {"kind": "term", "path": ["u_sub"], "terminal": "nope"},
    {"kind": "term", "id_path": [0], "term_id": 99},
    {"kind": "net", "id_path": [], "net_id": 99},
    {"kind": "net", "id_path": []},
    {"kind": "bogus", "path": []},
    {"kind": "view", "view": "../../etc"},
])
def test_bad_anchor_refused(review, anchor):
    with pytest.raises(ReviewError):
        review.add_thread(design(), anchor, "x")
    assert not (review.directory / "threads").exists()


def test_bad_thread_id_refused(review):
    with pytest.raises(ReviewError):
        review.add_event("../../review", text="x")


def test_no_author_is_read_only(top, tmp_path):
    review = Review(tmp_path / "r", None)
    with pytest.raises(ReviewError):
        review.add_thread(design(), U_AND, "x")
    assert review.response(design())["writable"] is False


def test_resolution_rules(top):
    snap = "a" * 64
    assert resolve_anchor(design(), U_AND, snap, snap) == {
        "state": "exact", "id_path": [0, 0], "path": ["u_sub", "u_and"]}
    # Written on another netlist: never guessed, by ids or by names.
    assert resolve_anchor(design(), U_AND, "b" * 64, snap) == {"state": "other_snapshot"}
    assert resolve_anchor(design(), U_AND, None, snap) == {"state": "other_snapshot"}
    assert resolve_anchor(design(), {**U_AND, "id_path": [5]}, snap, snap) == {
        "state": "unresolved"}


def test_thread_on_another_snapshot(review):
    tid = review.add_thread(design(), U_AND, "x")
    # As if written on another netlist, whose snapshot isn't in the review.
    other = "f" * 64
    path = review.directory / "threads" / tid / "thread.json"
    path.write_text(path.read_text().replace(review.design_snapshot(design()), other))
    [thread] = review.response(design())["threads"]
    assert thread["snapshot"] == other
    assert thread["snapshot_stored"] is False
    assert thread["resolution"] == {"state": "other_snapshot"}


def test_snapshot_identifies_the_netlist(review):
    first = review.design_snapshot(design())
    review.design_changed()
    assert review.design_snapshot(design()) == first  # deterministic dump
    top = design()
    net = naja.SNLScalarNet.create(top, "review_test_extra")
    try:
        review.design_changed()
        assert review.design_snapshot(top) != first
    finally:
        net.destroy()
        review.design_changed()


def test_malformed_files_are_skipped(review):
    review.add_thread(design(), U_AND, "kept")
    broken = review.directory / "threads" / ("0" * 32)
    broken.mkdir()
    (broken / "thread.json").write_text("{ not json")
    (review.directory / "threads" / "stray.txt").write_text("ignored")
    assert [t["comments"][0]["text"] for t in review.response(design())["threads"]] == ["kept"]


# -- through the session -----------------------------------------------------

def answer(session, **request):
    return [json.loads(r) for r in session.answer(json.dumps(request))]


def test_review_pushed_after_root(review):
    review.add_thread(design(), U_AND, "x")
    replies = answer(ViewerSession(review=review), request="load_root")
    assert [r["response"] for r in replies] == ["root_response", "review_response"]
    assert len(replies[1]["threads"]) == 1
    assert replies[1]["writable"] is True


def test_viewer_writes(review):
    pushed = []
    session = ViewerSession(push=pushed.append, review=review)
    assert answer(session, request="review_add_thread", anchor=PIN_A, text="hello",
                  severity="warning") == []
    [thread] = json.loads(pushed[-1])["threads"]
    assert thread["severity"] == "warning"
    assert answer(session, request="review_add_event", thread=thread["id"],
                  text="reply", status="resolved") == []
    [thread] = json.loads(pushed[-1])["threads"]
    assert thread["status"] == "resolved"
    assert [c["text"] for c in thread["comments"]] == ["hello", "reply"]


def test_viewer_write_without_push_replies(review):
    [reply] = answer(ViewerSession(review=review), request="review_add_thread", anchor=U_AND,
                     text="x")
    assert reply["response"] == "review_response"


def test_refused_writes_are_errors(top, review):
    session = ViewerSession(review=review)
    [reply] = answer(session, request="review_add_thread", anchor={"kind": "instance",
                                                                    "path": ["nope"]}, text="x")
    assert reply["response"] == "error" and reply["request"] == "review_add_thread"
    [reply] = answer(ViewerSession(), request="review_add_event", thread="0" * 32, text="x")
    assert reply["response"] == "error"


def test_stale_write_is_dropped(review):
    session = ViewerSession(review=review)
    session.design_changed()
    assert answer(session, request="review_add_thread", anchor=U_AND, text="x",
                  generation=0) == []
    assert not (review.directory / "threads").exists()

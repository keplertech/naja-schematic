import json

import pytest

pytest.importorskip("anywidget")
from naja_schematic import widget  # noqa: E402


@pytest.fixture
def view(tmp_path, monkeypatch):
    bundle = tmp_path / "naja-schematic.js"
    bundle.write_text("var createNajaSchematic;")
    monkeypatch.setenv("NAJA_SCHEMATIC_BUNDLE", str(bundle))
    widget._widget_esm.cache_clear()
    v = widget.Schematic(height=300)
    sent = []
    monkeypatch.setattr(v, "send", lambda content, buffers=None: sent.append(content))
    yield v, sent
    widget._widget_esm.cache_clear()


def test_esm_is_bundle_plus_glue(view):
    v, _ = view
    assert v._esm.startswith("var createNajaSchematic;")
    assert "export default { render }" in v._esm


def test_viewer_requests_are_answered_over_the_comm(top, view):
    v, sent = view
    v._on_viewer_message(v, {"json": json.dumps({"request": "load_root"})}, [])
    (reply,) = sent
    assert json.loads(reply["json"])["root"]["name"] == "top"


def test_annotate_pushes_now_and_after_each_root(top, view):
    v, sent = view
    items = [{"kind": "instance", "path": ["u_sub"], "severity": "error", "message": "m"}]
    v.annotate(items)
    v._on_viewer_message(v, {"json": json.dumps({"request": "load_root"})}, [])
    kinds = [json.loads(m["json"])["response"] for m in sent]
    assert kinds == ["diagnosis_response", "root_response", "diagnosis_response"]


def test_missing_bundle_is_a_clear_error(tmp_path, monkeypatch):
    monkeypatch.setenv("NAJA_SCHEMATIC_BUNDLE", str(tmp_path / "missing.js"))
    widget._widget_esm.cache_clear()
    with pytest.raises(RuntimeError, match="viewer bundle"):
        widget.Schematic()
    widget._widget_esm.cache_clear()


def sent_kinds(sent):
    return [json.loads(m["json"])["response"] for m in sent]


@pytest.mark.parametrize("target", [["u_sub", "u_and"], ("u_sub", "u_and")])
def test_show_instance_pushes_focus(top, view, target):
    v, sent = view
    v.show_instance(target)
    (msg,) = sent
    assert json.loads(msg["json"]) == {"response": "focus_instance", "path": ["u_sub", "u_and"],
                                       "id_path": [0, 0]}


def test_show_instance_accepts_a_najaeda_instance(top, view):
    from najaeda import netlist
    v, sent = view
    v.show_instance(netlist.get_instance_by_path(["u_sub", "u_and"]))
    assert json.loads(sent[0]["json"])["path"] == ["u_sub", "u_and"]


def test_show_instance_rejects_unknown_paths(top, view):
    v, sent = view
    with pytest.raises(ValueError, match="nope"):
        v.show_instance(["u_sub", "nope"])
    with pytest.raises(ValueError):  # a string is one name, never split
        v.show_instance("u_sub/u_and")
    with pytest.raises(TypeError):
        v.show_instance(42)
    assert sent == []


def test_show_instance_takes_a_string_as_one_name(top, view):
    v, sent = view
    v.show_instance("u_sub")
    assert json.loads(sent[0]["json"])["path"] == ["u_sub"]


def test_focus_is_reapplied_after_each_root(top, view):
    v, sent = view
    v.show_instance("u_sub")
    v._on_viewer_message(v, {"json": json.dumps({"request": "load_root"})}, [])
    assert sent_kinds(sent) == ["focus_instance", "root_response", "focus_instance"]


def test_viewer_selection_is_a_najaeda_instance(top, view):
    from najaeda import netlist
    v, sent = view
    assert v.selected is None
    seen = []
    v.on_select(seen.append)

    select = {"request": "instance_selected", "path": ["u_sub", "u_and"]}
    v._on_viewer_message(v, {"json": json.dumps(select)}, [])
    assert sent == []  # a notification: nothing is sent back
    assert v.selected_path == ["u_sub", "u_and"]
    assert v.selected == netlist.get_instance_by_path(["u_sub", "u_and"])
    assert v.selected.get_model_name() == "LUT2"
    assert seen == [v.selected]

    v._on_viewer_message(v, {"json": json.dumps({"request": "instance_selected", "path": []})}, [])
    assert v.selected.is_top()
    assert len(seen) == 2


def test_selection_that_no_longer_resolves_is_none(top, view):
    v, _ = view
    v.selected_path = ["gone"]
    assert v.selected is None

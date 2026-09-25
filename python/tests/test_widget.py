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

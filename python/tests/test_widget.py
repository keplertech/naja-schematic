import json

import pytest

pytest.importorskip("anywidget")
from najaeda import netlist  # noqa: E402
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
                                       "id_path": [0, 0], "generation": 0}


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


def test_viewer_selection_is_an_snl_path(top, view):
    v, sent = view
    assert v.selected is None
    seen = []
    v.on_select(seen.append)

    select = {"request": "instance_selected", "path": ["u_sub", "u_and"]}
    v._on_viewer_message(v, {"json": json.dumps(select)}, [])
    assert sent == []  # a notification: nothing is sent back
    assert v.selected_path == ["u_sub", "u_and"]
    assert v.selected.getInstanceIDs() == v.selected_id_path == [0, 0]
    assert v.selected.getTailInstance().getName() == "u_and"
    assert v.selected.getModel().getName() == "LUT2"
    assert seen == [v.selected]

    v._on_viewer_message(v, {"json": json.dumps({"request": "instance_selected", "path": []})}, [])
    assert v.selected.empty()  # the top design
    assert len(seen) == 2


def test_selection_that_no_longer_resolves_is_none(top, view):
    v, _ = view
    v.selected_path = ["gone"]
    assert v.selected is None


def test_design_changed_reloads_the_view_with_new_pushes(top, view):
    v, sent = view
    seen = []
    v.on_select(seen.append)
    v.annotate([{"kind": "instance", "path": ["u_sub"], "severity": "error", "message": "old"}])
    v.show_instance("u_sub")
    v._on_viewer_message(v, {"json": json.dumps({"request": "instance_selected", "path": ["u_sub"]})}, [])
    sent.clear()

    new_items = [{"kind": "instance", "path": ["u_sub"], "severity": "info", "message": "new"}]
    v.design_changed(diagnosis=new_items)
    assert sent_kinds(sent) == ["design_changed"]
    assert v.selected_id_path is None and v.selected is None
    assert seen[-1] is None  # on_select hears the selection go away

    # The viewer reloads: only the new diagnoses follow the root, no focus.
    sent.clear()
    v._on_viewer_message(v, {"json": json.dumps({"request": "load_root"})}, [])
    assert sent_kinds(sent) == ["root_response", "diagnosis_response"]
    assert json.loads(sent[1]["json"])["items"] == new_items


def test_design_changed_resolves_the_new_focus_first(top, view):
    v, sent = view
    with pytest.raises(ValueError):
        v.design_changed(instance=["nope"])
    assert sent == []  # nothing changed
    v.design_changed(instance=["u_sub", "u_and"])
    v._on_viewer_message(v, {"json": json.dumps({"request": "load_root"})}, [])
    assert sent_kinds(sent) == ["design_changed", "root_response", "focus_instance"]


def test_export_svg_asks_the_view_and_keeps_its_answer(top, view, tmp_path):
    v, sent = view
    got = []
    out = tmp_path / "view.svg"
    v.export_svg(out, got.append)
    assert [json.loads(m["json"]) for m in sent] == [{"response": "export_svg", "generation": 0}]
    del sent[:]
    v._on_viewer_message(v, {"json": json.dumps(
        {"request": "svg_exported", "svg": "<svg/>", "generation": 0})}, [])
    assert sent == []  # a notification: no reply
    assert got == ["<svg/>"] and v.svg == "<svg/>" and out.read_text() == "<svg/>"
    # One-shot: another rendering of the same view answering too is ignored.
    v._on_viewer_message(v, {"json": json.dumps({"request": "svg_exported", "svg": "<svg>2</svg>"})}, [])
    assert got == ["<svg/>"] and v.svg == "<svg/>"


def test_export_svg_for_a_replaced_design_is_dropped(top, view):
    v, _ = view
    got = []
    v.export_svg(callback=got.append)
    v.design_changed()
    v._on_viewer_message(v, {"json": json.dumps(
        {"request": "svg_exported", "svg": "<svg/>", "generation": 0})}, [])
    assert got == [] and v.svg is None


def test_export_svg_waiting_is_asked_again_after_each_root(top, view):
    v, sent = view
    v.export_svg()
    del sent[:]
    v._on_viewer_message(v, {"json": json.dumps({"request": "load_root"})}, [])
    assert sent_kinds(sent) == ["root_response", "export_svg"]
    v._on_viewer_message(v, {"json": json.dumps({"request": "svg_exported", "svg": "<svg/>"})}, [])
    del sent[:]
    v._on_viewer_message(v, {"json": json.dumps({"request": "load_root"})}, [])
    assert sent_kinds(sent) == ["root_response"]


def pushed(sent):
    return [json.loads(m["json"]) for m in sent]


def test_show_net_pushes_the_pin_by_ids(top, view):
    v, sent = view
    v.show_net((["u_sub", "u_and"], "O"), color=(255, 136, 0))
    assert pushed(sent) == [{"response": "show_net", "id_path": [0, 0],
                             "path": ["u_sub", "u_and"], "term_id": 2,
                             "label": "u_sub/u_and/O (net)", "color": "#ff8800",
                             "add": True, "generation": 0}]


@pytest.mark.parametrize("make, expected", [
    (lambda: netlist.get_instance_by_path(["u_sub", "u_and"]).get_term("I1"),
     ([0, 0], 1, None, "u_sub/u_and/I1 (net)")),
    (lambda: netlist.get_top().get_term("d").get_bit(1), ([], 1, 1, "d[1] (net)")),
    (lambda: netlist.get_top().get_net("s"), ([0], 2, None, "s (net)")),
    (lambda: netlist.get_instance_by_path(["u_sub"]).get_net("n"),
     ([0, 0], 2, None, "u_sub/n (net)")),
    (lambda: netlist.get_top().get_term("clk").get_equipotential(), ([], 0, None, "clk (net)")),
    (lambda: ([], "d", 0), ([], 1, 0, "d[0] (net)")),
    (lambda: ("u_sub", 2), ([0], 2, None, "u_sub/y (net)")),
])
def test_show_net_targets(top, view, make, expected):
    v, sent = view
    v.show_net(make())
    (msg,) = pushed(sent)
    assert (msg["id_path"], msg["term_id"], msg.get("bit"), msg["label"]) == expected


def test_show_net_takes_naja_objects(top, view):
    from najaeda import naja
    v, sent = view
    design = naja.NLUniverse.get().getTopDesign()
    sub = design.getInstance("u_sub")
    u_and = sub.getModel().getInstance("u_and")
    occurrence = naja.SNLOccurrence(naja.SNLPath(naja.SNLPath(), sub),
                                    u_and.getInstTerm(u_and.getModel().getTerm("O")))
    v.show_net(occurrence)
    v.show_net(sub.getInstTerm(sub.getModel().getTerm("y")))
    v.show_net(design.getTerm("clk"))
    v.show_net(naja.SNLEquipotential(design.getTerm("q")))
    assert [(m["id_path"], m["term_id"]) for m in pushed(sent)] == [
        ([0, 0], 2), ([0], 2), ([], 0), ([], 2)]


@pytest.mark.parametrize("target, error, match", [
    (([], "d"), ValueError, "is a bus"),               # a whole bus
    (([], "d", 5), ValueError, "no bit 5"),
    (([], "clk", 0), ValueError, "not a bus"),
    (([], "nope"), ValueError, "no terminal 'nope'"),
    ((["u_sub", "nope"], "a"), ValueError, "no instance"),
    (lambda: netlist.get_top().get_net("d"), ValueError, "is a bus"),
    ("u_sub", TypeError, "expected"),                  # an instance, not a pin
    (42, TypeError, "expected"),
])
def test_show_net_rejects_what_is_not_one_pin(top, view, target, error, match):
    v, sent = view
    with pytest.raises(error, match=match):
        v.show_net(target() if callable(target) else target)
    assert sent == []


def test_show_net_rejects_colors_the_viewer_would(top, view):
    v, sent = view
    for color, error in [("#000000", ValueError), ("#f8f8f8", ValueError),
                         ("orange", ValueError), ((300, 0, 0), TypeError)]:
        with pytest.raises(error):
            v.show_net(([], "clk"), color=color)
    assert sent == []


def test_shown_nets_follow_the_focus_after_each_root(top, view):
    v, sent = view
    v.show_instance("u_sub")
    v.show_net(([], "clk"))
    v.show_net(([], "q"), label="out")
    sent.clear()
    v._on_viewer_message(v, {"json": json.dumps({"request": "load_root"})}, [])
    msgs = pushed(sent)
    assert [m["response"] for m in msgs] == [
        "root_response", "focus_instance", "show_net", "show_net"]
    assert [m["label"] for m in msgs[2:]] == ["clk (net)", "out"]


def test_show_instance_and_replacing_net_reset_the_kept_nets(top, view):
    v, sent = view
    v.show_instance("u_sub")
    v.show_net(([], "clk"))
    v.show_net(([], "q"), add=False)  # clears the view: the focus goes too
    sent.clear()
    v._on_viewer_message(v, {"json": json.dumps({"request": "load_root"})}, [])
    msgs = pushed(sent)
    assert [m["response"] for m in msgs] == ["root_response", "show_net"]
    assert msgs[1]["term_id"] == 2 and msgs[1]["add"] is False

    v.show_instance("u_sub")  # clears the view: the nets go
    sent.clear()
    v._on_viewer_message(v, {"json": json.dumps({"request": "load_root"})}, [])
    assert sent_kinds(sent) == ["root_response", "focus_instance"]


def test_design_changed_drops_the_kept_nets(top, view):
    v, sent = view
    v.show_net(([], "clk"))
    v.design_changed()
    sent.clear()
    v._on_viewer_message(v, {"json": json.dumps({"request": "load_root"})}, [])
    assert sent_kinds(sent) == ["root_response"]

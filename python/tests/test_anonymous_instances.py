"""Anonymous instances (empty name) across the protocol: addressed by
id_path, told apart from their anonymous siblings, at any depth.

The request/reply cases live in tests/data/anonymous_protocol_cases.json,
shared with tests/provider/LocalSNLProviderProtocolTest.cpp, so protocol.py
and LocalSNLProvider are held to the same answers.
"""
import json
from pathlib import Path

import pytest
from najaeda import naja

from naja_schematic import protocol
from naja_schematic.protocol import handle_request

CASES_FILE = Path(__file__).resolve().parents[2] / "tests" / "data" / "anonymous_protocol_cases.json"
SPEC = json.loads(CASES_FILE.read_text())

_DIRECTIONS = {"input": naja.SNLTerm.Direction.Input,
               "output": naja.SNLTerm.Direction.Output,
               "inout": naja.SNLTerm.Direction.InOut}


def build_design(spec):
    """Build the spec'd netlist with the raw naja API in a DB of its own;
    returns {design name: design}. Instances are created in list order, so
    their ids are their list indices."""
    u = naja.NLUniverse.get()
    db = naja.NLDB.create(u)
    designs = {}
    prims = naja.NLLibrary.createPrimitives(db, "anon_prims")
    for p in spec["primitives"]:
        d = naja.SNLDesign.createPrimitive(prims, p["name"])
        for name, direction in p["terms"]:
            naja.SNLScalarTerm.create(d, _DIRECTIONS[direction], name)
        designs[p["name"]] = d
    lib = naja.NLLibrary.create(db, "anon_work")
    for spec_d in spec["designs"]:
        d = naja.SNLDesign.create(lib, spec_d["name"])
        for name, direction in spec_d["terms"]:
            naja.SNLScalarTerm.create(d, _DIRECTIONS[direction], name)
        instances = []
        for index, (name, model) in enumerate(spec_d["instances"]):
            inst = (naja.SNLInstance.create(d, designs[model], name) if name
                    else naja.SNLInstance.create(d, designs[model]))
            assert inst.getID() == index
            instances.append(inst)
        for n in spec_d["nets"]:
            net = naja.SNLScalarNet.create(d, n["name"])
            d.getScalarTerm(n["term"]).setNet(net)
            for index, term in n["pins"]:
                inst = instances[index]
                inst.getInstTerm(inst.getModel().getScalarTerm(term)).setNet(net)
        designs[spec_d["name"]] = d
    return designs


@pytest.fixture(scope="module")
def anon(top):
    # NLUniverse is process-global: build next to the session's design and
    # make it the top design for this module only.
    u = naja.NLUniverse.get()
    previous = u.getTopDesign()
    designs = build_design(SPEC["design"])
    u.setTopDesign(designs[SPEC["design"]["top"]])
    yield designs
    u.setTopDesign(previous)


def design_ref(design):
    return {"db_id": design.getDB().getID(), "library_id": design.getLibrary().getID(),
            "design_id": design.getID()}


def substitute(value, designs):
    if isinstance(value, str) and value.startswith("$design:"):
        return design_ref(designs[value[len("$design:"):]])
    if isinstance(value, dict):
        return {k: substitute(v, designs) for k, v in value.items()}
    if isinstance(value, list):
        return [substitute(v, designs) for v in value]
    return value


def matches_unordered(expected, actual, where):
    # Each expected element matched to a different actual element.
    if not isinstance(actual, list) or len(actual) != len(expected):
        return f"{where}: expected {expected!r}, got {actual!r}"
    left = list(actual)
    for e in expected:
        hit = next((i for i, a in enumerate(left) if matches(e, a) is None), None)
        if hit is None:
            return f"{where}: nothing left matches {e!r} in {actual!r}"
        left.pop(hit)
    return None


def matches(expected, actual, where="reply"):
    """Every key of `expected` is in `actual` with that value (objects
    recursively, lists element-wise with the same length). Returns the
    first mismatch as a string, or None."""
    if isinstance(expected, dict):
        if not isinstance(actual, dict):
            return f"{where}: expected an object, got {actual!r}"
        for key, value in expected.items():
            if key not in actual:
                return f"{where}: missing {key!r}"
            if (err := matches(value, actual[key], f"{where}.{key}")):
                return err
        return None
    if isinstance(expected, list):
        if not isinstance(actual, list) or len(actual) != len(expected):
            return f"{where}: expected {expected!r}, got {actual!r}"
        for i, (e, a) in enumerate(zip(expected, actual)):
            if (err := matches(e, a, f"{where}[{i}]")):
                return err
        return None
    # bool is an int in Python: don't let True match 1.
    if expected != actual or isinstance(expected, bool) != isinstance(actual, bool):
        return f"{where}: expected {expected!r}, got {actual!r}"
    return None


@pytest.mark.parametrize("case", SPEC["cases"], ids=[c["name"] for c in SPEC["cases"]])
def test_shared_protocol_case(anon, case):
    replies = handle_request(substitute(case["request"], anon))
    # Through JSON, like the wire, so both sides compare the same thing.
    replies = json.loads(json.dumps(replies))
    expect = [dict(e) for e in case["expect"]]
    for key in case.get("unordered", []):
        expected_list = expect[0].pop(key)
        err = matches_unordered(expected_list, replies[0].get(key) if replies else None,
                                f"replies[0].{key}")
        assert err is None, err
    assert (err := matches(expect, replies, "replies")) is None, err
    for key in case.get("absent", []):
        assert key not in replies[0], f"unexpected {key!r} in {replies[0]}"


def test_resolved_ids_address_the_same_instance(anon):
    # What resolve_instance answers is what the viewer sends back (selection,
    # properties): the round trip lands on the same object.
    top = anon["top_anon"]
    for ids in ([0], [1], [1, 0], [1, 1], [2, 1]):
        (resolved,) = handle_request({"request": "resolve_instance", "id_path": ids})
        (props,) = handle_request({"request": "get_properties", "kind": "instance",
                                   "path": resolved["path"], "id_path": resolved["id_path"]})
        inst = top
        for i in ids:
            inst = (inst.getModel() if isinstance(inst, naja.SNLInstance) else inst).getInstanceByID(i)
        assert {"name": "ID", "value": str(inst.getID())} in props["properties"]
        assert {"name": "Model", "value": inst.getModel().getName()} in props["properties"]


def test_diagnosis_item_from_a_najaeda_instance(anon):
    from najaeda import netlist
    inst = netlist.Instance([1, 0])
    (item,) = protocol.diagnosis_response(
        [{"kind": "instance", "path": inst, "severity": "error", "message": "m"}])["items"]
    assert item["id_path"] == [1, 0]
    assert item["path"] == ["", ""]
    assert item["severity"] == "error"
    json.dumps(item)  # plain JSON, ready to push


def test_diagnosis_items_pass_through_untouched(anon):
    items = [{"kind": "instance", "path": ["a/b"], "message": "by name"},
             {"kind": "net", "path": [""], "id_path": [0], "terminal": "i", "message": "by id"}]
    assert protocol.diagnosis_response(items)["items"] == items


def test_focus_instance_message_carries_both_paths():
    assert protocol.focus_instance(["", "u_leaf"], [1, 2]) == {
        "response": "focus_instance", "path": ["", "u_leaf"], "id_path": [1, 2]}
    # Names only, as before id paths.
    assert protocol.focus_instance(["u_sub"]) == {"response": "focus_instance", "path": ["u_sub"]}


# ---------------------------------------------------------------------------
# Notebook widget: focus -> selection round trip
# ---------------------------------------------------------------------------

@pytest.fixture
def view(anon, tmp_path, monkeypatch):
    widget = pytest.importorskip("naja_schematic.widget")
    bundle = tmp_path / "naja-schematic.js"
    bundle.write_text("var createNajaSchematic;")
    monkeypatch.setenv("NAJA_SCHEMATIC_BUNDLE", str(bundle))
    widget._widget_esm.cache_clear()
    v = widget.Schematic(height=300)
    sent = []
    monkeypatch.setattr(v, "send", lambda content, buffers=None: sent.append(content))
    yield v, sent
    widget._widget_esm.cache_clear()


def play_viewer(v, focus):
    """What the viewer does with a focus_instance push (AppLogic.cpp): ask
    the host to resolve it, then report the resolved instance as selected."""
    request = {"request": "resolve_instance"}
    for key in ("path", "id_path"):
        if key in focus:
            request[key] = focus[key]
    (resolved,) = handle_request(request)
    assert resolved["found"], resolved
    v._on_viewer_message(v, {"json": json.dumps({
        "request": "instance_selected",
        "path": resolved["path"], "id_path": resolved["id_path"]})}, [])
    return resolved


@pytest.mark.parametrize("ids", [[0], [1], [1, 0], [1, 1], [2, 1], []])
def test_focus_selection_round_trip(view, ids):
    from najaeda import netlist
    v, sent = view
    target = netlist.Instance(ids)
    v.show_instance(target)
    focus = json.loads(sent[-1]["json"])
    assert focus["id_path"] == ids
    resolved = play_viewer(v, focus)
    assert resolved["id_path"] == ids
    assert v.selected_id_path == ids
    assert v.selected == target


def test_anonymous_siblings_are_distinct_selections(view):
    from najaeda import netlist
    v, _ = view
    seen = []
    v.on_select(seen.append)
    for ids in ([0], [1], [0]):
        v._on_viewer_message(v, {"json": json.dumps(
            {"request": "instance_selected", "path": [""], "id_path": ids})}, [])
    # Same name path each time, yet three selection changes.
    assert v.selected_path == [""]
    assert seen == [netlist.Instance([0]), netlist.Instance([1]), netlist.Instance([0])]


def test_show_instance_accepts_an_id_path(view):
    v, sent = view
    v.show_instance([1, 1])
    assert json.loads(sent[-1]["json"]) == {
        "response": "focus_instance", "path": ["", ""], "id_path": [1, 1],
        "generation": 0}
    v.show_instance(("a/b",))
    assert json.loads(sent[-1]["json"])["id_path"] == [2]


def test_show_instance_rejects_what_cannot_name_one_instance(view):
    v, sent = view
    with pytest.raises(ValueError):
        v.show_instance([""])        # which anonymous sibling?
    with pytest.raises(ValueError):
        v.show_instance([0, 99])
    with pytest.raises(TypeError):
        v.show_instance([0, "u_leaf"])  # ids and names mixed
    with pytest.raises(TypeError):
        v.show_instance([True])
    assert sent == []


def test_selection_from_a_names_only_viewer_still_resolves(view):
    from najaeda import netlist
    v, _ = view
    v._on_viewer_message(v, {"json": json.dumps(
        {"request": "instance_selected", "path": ["u_mid", "u_leaf"]})}, [])
    assert v.selected_id_path == [3, 2]
    assert v.selected == netlist.Instance([3, 2])

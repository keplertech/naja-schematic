import ast
import json
import subprocess
import sys
from pathlib import Path
from types import SimpleNamespace

from najaeda import naja

from naja_schematic import protocol
from naja_schematic.protocol import handle_message, handle_request


def root(top):
    (reply,) = handle_request({"request": "load_root"})
    assert reply["response"] == "root_response"
    return reply["root"]


def top_terms(top):
    ref = root(top)["design_ref"]
    (reply,) = handle_request({"request": "load_terms", "design_ref": ref})
    return {t["name"]: t for t in reply["children"]}


def test_load_root(top):
    r = root(top)
    assert r["name"] == "top"
    assert r["has_instances"] and r["has_primitives"] and r["has_terms"] and r["has_nets"]


def test_design_requests(top):
    ref = root(top)["design_ref"]
    (inst,) = handle_request({"request": "load_instances", "design_ref": ref, "gui_id": 7})
    assert inst["response"] == "instances_response" and inst["gui_id"] == 7
    assert [c["name"] for c in inst["children"]] == ["u_sub"]

    (prims,) = handle_request({"request": "load_primitives", "design_ref": ref})
    assert [(c["name"], c["model_name"]) for c in prims["children"]] == [("r", "FDRE")]

    terms = top_terms(top)
    assert terms["d"]["msb"] == 1 and terms["d"]["lsb"] == 0
    assert terms["q"]["direction"] == 1

    (nets,) = handle_request({"request": "load_nets", "design_ref": ref})
    names = [n["name"] for n in nets["children"]]
    assert "s" in names
    # The implicit 1'b0/1'b1 tie-offs on r.CE/r.R are filtered out.
    assert "" not in names


def test_instance_requests_echo_the_instance_path(top):
    # The viewer tags these requests with the box's instance path, a list of
    # names (one can contain "/"), and matches the reply by it.
    (inst,) = handle_request({"request": "load_instances", "design_ref": root(top)["design_ref"]})
    sub_ref = inst["children"][0]["design_ref"]
    path = ["u_sub", "a/b"]
    (terms,) = handle_request({"request": "expand_instance_terms",
                               "instance_path": path, "design_ref": sub_ref})
    assert terms["response"] == "expanded_instance_terms"
    assert terms["instance_path"] == path and terms["terms"]
    (internals,) = handle_request({"request": "load_instance_internals",
                                   "instance_path": path, "design_ref": sub_ref})
    assert internals["response"] == "instance_internals_response"
    assert internals["instance_path"] == path


def test_unknown_design_ref_is_an_error_reply(top):
    bad = {"db_id": 99, "library_id": 99, "design_id": 99}
    (reply,) = handle_request({"request": "load_terms", "design_ref": bad, "gui_id": 3})
    assert reply == {"response": "load_terms_response", "gui_id": 3}


def test_equipotential_and_trace(top):
    q = top_terms(top)["q"]
    (eq,) = handle_request({"request": "load_equipotential", "path": [], "term_id": q["child_id"]})
    assert eq["response"] == "equipotential_response"
    assert [o["path"][-1][2] for o in eq["occurrences"]] == ["FDRE"]

    (trace,) = handle_request({"request": "trace_driver", "path": [], "term_id": q["child_id"]})
    assert trace["response"] == "trace_driver_response"
    # q is driven by a flop: the cone stops right there.
    assert len(trace["equipotentials"]) == 1 and not trace["truncated"]
    assert "trace_id" not in trace

    # The viewer's trace_id comes back, so it knows which trace this is.
    (trace,) = handle_request({"request": "trace_driver", "path": [], "term_id": q["child_id"],
                               "trace_id": 7})
    assert trace["trace_id"] == 7


def test_get_properties(top):
    (reply,) = handle_request({"request": "get_properties", "kind": "instance", "path": ["u_sub"]})
    assert reply["subject"] == "u_sub"
    assert {"name": "Model", "value": "sub"} in reply["properties"]

    (reply,) = handle_request({"request": "get_properties", "kind": "term", "path": [],
                               "terminal": "d", "bit": 1})
    assert {"name": "Bit", "value": "1"} in reply["properties"]

    (reply,) = handle_request({"request": "get_properties", "kind": "instance", "path": ["nope"]})
    assert reply["properties"] == []


def test_unknown_request_is_ignored(top):
    assert handle_request({"request": "bogus"}) == []


def test_handler_exception_is_contained(top, monkeypatch):
    def boom(u, request):
        raise ValueError("boom")
    monkeypatch.setitem(protocol._HANDLERS, "load_root", boom)
    assert handle_request({"request": "load_root"}) == []


def test_no_universe(monkeypatch):
    fake_naja = SimpleNamespace(NLUniverse=SimpleNamespace(get=lambda: None))
    monkeypatch.setattr(protocol, "naja", fake_naja)
    assert handle_request({"request": "load_root"}) == [{"response": "root_response", "gui_id": 0}]
    assert handle_request({"request": "load_terms", "design_ref": {}}) == []


def test_handle_message_round_trips_json(top):
    (reply,) = handle_message(json.dumps({"request": "load_root"}))
    assert json.loads(reply)["root"]["name"] == "top"


def test_diagnosis_response_accepts_document_or_list():
    item = {"kind": "instance", "path": ["u_sub"], "severity": "error", "message": "m"}
    assert protocol.diagnosis_response([item]) == {"response": "diagnosis_response", "items": [item]}
    assert protocol.diagnosis_response({"items": [item]})["items"] == [item]


def test_resolve_instance(top):
    (reply,) = handle_request({"request": "resolve_instance", "path": ["u_sub", "u_and"]})
    assert reply["response"] == "instance_resolved" and reply["found"]
    inst = reply["instance"]
    assert [level[0] for level in inst["path"]] == ["u_sub", "u_and"]
    assert [level[2] for level in inst["path"]] == ["sub", "LUT2"]
    assert not inst["has_instances"]
    assert inst["primitive_type"] == "unknown"  # LUT2 isn't a named gate
    assert {t["name"] for t in inst["terms"]} == {"I0", "I1", "O"}

    (reply,) = handle_request({"request": "resolve_instance", "path": ["u_sub"]})
    assert reply["instance"]["has_instances"]  # u_and is a visible primitive
    # Ids are the ones every other request addresses the instance with.
    (props,) = handle_request({"request": "get_properties", "kind": "instance", "path": ["u_sub"]})
    assert {"name": "Model", "value": "sub"} in props["properties"]


def test_resolve_instance_top_and_unknown(top):
    (reply,) = handle_request({"request": "resolve_instance", "path": []})
    assert reply["found"] and "instance" not in reply

    (reply,) = handle_request({"request": "resolve_instance", "path": ["u_sub", "nope"]})
    assert reply == {"response": "instance_resolved", "path": ["u_sub", "nope"], "found": False}


def test_instance_selected_is_a_notification(top):
    assert handle_request({"request": "instance_selected", "path": ["u_sub"]}) == []


def test_focus_instance_message():
    assert protocol.focus_instance(["u_sub", "u_and"]) == {
        "response": "focus_instance", "path": ["u_sub", "u_and"]}


def test_get_primitive_type_uses_naja_modeling_not_names():
    checks = ("isAssign", "isSequential", "isAddSub", "isSubtract", "isMultiply",
              "isShift", "isShiftRight", "isShiftArithmetic", "isDivMod", "isMux",
              "isInv", "isBuf", "isAnd", "isNand", "isOr", "isNor", "isXor", "isXnor",
              "isConst0", "isConst1")

    output = SimpleNamespace(getDirection=lambda: naja.SNLTerm.Direction.Output)

    def model(name, *true_checks, outputs=1):
        return SimpleNamespace(getName=lambda: name,
                               getBitTerms=lambda: [output] * outputs,
                               **{c: (lambda c=c: c in true_checks) for c in checks})
    assert protocol.get_primitive_type(None) == "unknown"
    assert protocol.get_primitive_type(model("x", "isAssign")) == "assign"
    assert protocol.get_primitive_type(model("x", "isSequential")) == "dff"
    assert protocol.get_primitive_type(model("ND2", "isNand")) == "nand"
    assert protocol.get_primitive_type(model("OAI", "isXnor")) == "xnor"
    assert protocol.get_primitive_type(model("TIEL", "isConst0")) == "tie0"
    assert protocol.get_primitive_type(model("TIEH", "isConst1")) == "tie1"
    # The name never matters: an unmodelled "AND2" is just a box.
    assert protocol.get_primitive_type(model("AND2_X1")) == "unknown"
    # A multi-output cell never reaches the truth-table checks (naja throws).
    assert protocol.get_primitive_type(model("FA", "isXor", outputs=2)) == "unknown"
    assert protocol.get_primitive_type(model("DFF2", "isSequential", outputs=2)) == "dff"
    # A mux at any width: a bus mux is one model with an output per bit.
    assert protocol.get_primitive_type(model("MX2", "isMux")) == "mux"
    assert protocol.get_primitive_type(model("MX2_W4", "isMux", outputs=4)) == "mux"
    # Word-level operators, at any width too.
    assert protocol.get_primitive_type(model("ADD", "isAddSub", outputs=4)) == "add"
    assert protocol.get_primitive_type(model("SUB", "isAddSub", "isSubtract", outputs=4)) == "sub"
    assert protocol.get_primitive_type(model("MUL", "isMultiply", outputs=4)) == "mul"
    assert protocol.get_primitive_type(model("SHL", "isShift", outputs=4)) == "shl"
    assert protocol.get_primitive_type(model("SHR", "isShift", "isShiftRight", outputs=4)) == "shr"
    assert protocol.get_primitive_type(
        model("SRA", "isShift", "isShiftRight", "isShiftArithmetic", outputs=4)) == "ashr"
    assert protocol.get_primitive_type(model("DIV", "isDivMod", outputs=8)) == "divmod"


REPO_DATA = Path(__file__).parents[2] / "tests" / "data"


def _primitive_types_in_subprocess(load):
    # Run in a subprocess, which keeps this design out of the session's
    # universe. `load` is an NLDB method call on `db`. Returns
    # {model name: primitive_type} over the whole hierarchy.
    script = (
        "from najaeda import naja\n"
        "from naja_schematic import protocol\n"
        "db = naja.NLDB.create(naja.NLUniverse.create())\n"
        "naja.NLUniverse.get().setTopDB(db)\n"
        f"db.{load}\n"
        "types = {}\n"
        "def walk(d):\n"
        "    for i in d.getInstances():\n"
        "        m = i.getModel()\n"
        "        types[m.getName()] = protocol.get_primitive_type(m)\n"
        "        if not m.isPrimitive(): walk(m)\n"
        "walk(naja.NLUniverse.get().getTopDesign())\n"
        "print(sorted(types.items()))\n")
    run = subprocess.run([sys.executable, "-c", script],
                         capture_output=True, text=True)
    assert run.returncode == 0, run.stderr
    return dict(ast.literal_eval(run.stdout.strip().splitlines()[-1]))


def test_get_primitive_type_of_word_level_operators():
    # The same SystemVerilog, and answers, as LocalSNLProvider's
    # WordLevelOperatorsGetTheirPrimitiveType test.
    types = _primitive_types_in_subprocess(
        f"loadSystemVerilog([{str(REPO_DATA / 'sv' / 'operators.sv')!r}])")
    assert types == {
        "naja_add__w4": "add", "naja_sub__w4": "sub", "naja_mul__w4": "mul",
        "naja_shift__w4_c2_r0_a0": "shl", "naja_shift__w4_c2_r1_a0": "shr",
        "naja_shift__w4_c2_r1_a1": "ashr", "naja_divmod__u_w4": "divmod",
        "naja_mux2__w4": "mux", "naja_mux2__w1": "mux",
    }


def test_get_primitive_type_of_vhdl():
    vhdl = [str(REPO_DATA / "vhdl" / f) for f in ("top.vhd", "leaf.vhd")]
    types = _primitive_types_in_subprocess(
        f"loadVHDL({vhdl!r}, diagnostics_report_path=None)")
    assert types["naja_mux2__w1"] == "mux"
    assert types["leaf"] == "unknown"  # hierarchical

import json
from types import SimpleNamespace

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

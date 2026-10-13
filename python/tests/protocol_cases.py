"""Runner for the wire-protocol case files shared with the C++ side
(tests/data/*_protocol_cases.json, also run by
tests/provider/LocalSNLProviderProtocolTest.cpp), so protocol.py and
LocalSNLProvider are held to the same answers. See a case file's "comment"
for the format.
"""
import json
import os
from pathlib import Path

from najaeda import naja

CASES_DIR = Path(__file__).resolve().parents[2] / "tests" / "data"

_DIRECTIONS = {"input": naja.SNLTerm.Direction.Input,
               "output": naja.SNLTerm.Direction.Output,
               "inout": naja.SNLTerm.Direction.InOut}


def load_spec(name):
    return json.loads((CASES_DIR / name).read_text())


def _create_terms(design, terms):
    # [name, direction] for a scalar term, [name, direction, msb, lsb] for a bus.
    for term in terms:
        if len(term) == 4:
            naja.SNLBusTerm.create(design, _DIRECTIONS[term[1]], term[2], term[3], term[0])
        else:
            naja.SNLScalarTerm.create(design, _DIRECTIONS[term[1]], term[0])


def build_design(spec):
    """Build the spec'd netlist with the raw naja API in a DB of its own;
    returns {design name: design}. Instances are created in list order, so
    their ids are their list indices."""
    u = naja.NLUniverse.get()
    db = naja.NLDB.create(u)
    designs = {}
    prims = naja.NLLibrary.createPrimitives(db, "case_prims")
    for p in spec["primitives"]:
        d = naja.SNLDesign.createPrimitive(prims, p["name"])
        _create_terms(d, p["terms"])
        designs[p["name"]] = d
    lib = naja.NLLibrary.create(db, "case_work")
    for spec_d in spec["designs"]:
        d = naja.SNLDesign.create(lib, spec_d["name"])
        _create_terms(d, spec_d["terms"])
        instances = []
        for index, (name, model) in enumerate(spec_d["instances"]):
            inst = (naja.SNLInstance.create(d, designs[model], name) if name
                    else naja.SNLInstance.create(d, designs[model]))
            assert inst.getID() == index
            instances.append(inst)
        for n in spec_d["nets"]:
            if "msb" in n:
                naja.SNLBusNet.create(d, n["msb"], n["lsb"], n["name"])
                continue
            net = naja.SNLScalarNet.create(d, n["name"])
            if "term" in n:
                d.getScalarTerm(n["term"]).setNet(net)
            for index, term in n.get("pins", []):
                inst = instances[index]
                inst.getInstTerm(inst.getModel().getScalarTerm(term)).setNet(net)
        designs[spec_d["name"]] = d
    return designs


def load_verilog_design(spec, root):
    """Load the spec'd gate-level Verilog (`spec["verilog"]`, relative to
    `root`, which is the current directory while loading so recorded paths
    stay relative) into a DB of its own, Yosys src decoding on; returns
    {design name: design} for the top and every model below it."""
    db = naja.NLDB.create(naja.NLUniverse.get())
    previous = os.getcwd()
    os.chdir(root)
    try:
        db.loadVerilog([spec["verilog"]], yosys_source_attributes=True)
    finally:
        os.chdir(previous)
    top = next(d for lib in db.getLibraries() for d in lib.getSNLDesigns()
               if d.getName() == spec["top"])
    designs = {}
    pending = [top]
    while pending:
        design = pending.pop()
        if design.getName() not in designs:
            designs[design.getName()] = design
            pending.extend(inst.getModel() for inst in design.getInstances())
    return designs


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


def check_case(case, replies):
    """Assert that `replies` (dicts, already through JSON) are what `case`
    expects."""
    expect = [dict(e) for e in case["expect"]]
    for key in case.get("unordered", []):
        expected_list = expect[0].pop(key)
        err = matches_unordered(expected_list, replies[0].get(key) if replies else None,
                                f"replies[0].{key}")
        assert err is None, err
    assert (err := matches(expect, replies, "replies")) is None, err
    for key in case.get("absent", []):
        assert key not in replies[0], f"unexpected {key!r} in {replies[0]}"

"""The structure of a net across the hierarchy ("graph" on an
equipotential body), from naja's equipotential graph.

The request/reply cases live in tests/data/crossing_protocol_cases.json
(a netlist built here) and crossing_assign_protocol_cases.json (Verilog),
shared with tests/provider/LocalSNLProviderProtocolTest.cpp, so protocol.py
and LocalSNLProvider are held to the same answers.
"""
import json

import pytest
from najaeda import naja

from naja_schematic.protocol import handle_request
from protocol_cases import (CASES_DIR, build_design, check_case, load_spec,
                            load_verilog_design, substitute)

SPECS = {name: load_spec(name) for name in ("crossing_protocol_cases.json",
                                            "crossing_assign_protocol_cases.json")}


@pytest.fixture(scope="module", params=sorted(SPECS))
def suite(request, top):
    # NLUniverse is process-global: build next to the session's design and
    # make it the top design for this suite only.
    spec = SPECS[request.param]
    u = naja.NLUniverse.get()
    previous = u.getTopDesign()
    if "verilog" in spec["design"]:
        designs = load_verilog_design(spec["design"], CASES_DIR / spec["source_root"])
    else:
        designs = build_design(spec["design"])
    u.setTopDesign(designs[spec["design"]["top"]])
    yield spec, designs
    u.setTopDesign(previous)
    # Leave the universe with one design DB, as the review tests expect.
    designs[spec["design"]["top"]].getDB().destroy()


def test_shared_protocol_cases(suite):
    spec, designs = suite
    for case in spec["cases"]:
        replies = handle_request(substitute(case["request"], designs))
        # Through JSON, like the wire, so both sides compare the same thing.
        try:
            check_case(case, json.loads(json.dumps(replies)))
        except AssertionError as error:
            raise AssertionError(f"{case['name']}: {error}") from None


def test_bus_bit_nets_keep_their_type_after_a_graph(suite):
    # An occurrence's "net" keeps its "[bit]" after the reply's graph was
    # built: najaeda 0.8.0's getGraph() typed a bus-net bit as a scalar net
    # while its result lived (fixed in 0.8.1, the minimum).
    spec, _ = suite
    if "verilog" not in spec["design"]:
        pytest.skip("no bus in this design")
    reply = handle_request({"request": "load_equipotential", "path": [3], "term_id": 1})[0]
    assert {o["net"] for o in reply["occurrences"]} == {"b[0]", "d[0]"}

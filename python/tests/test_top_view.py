"""load_top_view: the top design drawn as a sheet (its ports, and its
contents when small enough).

The request/reply cases live in tests/data/top_view_protocol_cases.json,
shared with tests/provider/LocalSNLProviderProtocolTest.cpp, so protocol.py
and LocalSNLProvider are held to the same answers.
"""
import json

import pytest
from najaeda import naja

from naja_schematic.protocol import handle_request
from protocol_cases import CASES_DIR, check_case, load_spec, load_verilog_design, substitute

SPEC = load_spec("top_view_protocol_cases.json")


@pytest.fixture(scope="module")
def designs(top):
    # NLUniverse is process-global: load next to the session's design and
    # make it the top design for this module only.
    u = naja.NLUniverse.get()
    previous = u.getTopDesign()
    loaded = load_verilog_design(SPEC["design"], CASES_DIR / SPEC["source_root"])
    u.setTopDesign(loaded[SPEC["design"]["top"]])
    yield loaded
    u.setTopDesign(previous)
    # Leave the universe with one design DB, as the review tests expect.
    loaded[SPEC["design"]["top"]].getDB().destroy()


@pytest.mark.parametrize("case", SPEC["cases"], ids=[c["name"] for c in SPEC["cases"]])
def test_shared_protocol_case(designs, case):
    replies = handle_request(substitute(case["request"], designs))
    # Through JSON, like the wire, so both sides compare the same thing.
    check_case(case, json.loads(json.dumps(replies)))


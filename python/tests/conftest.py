from pathlib import Path

import pytest
from najaeda import netlist

DATA = Path(__file__).parent / "data"


@pytest.fixture(scope="session")
def top():
    # NLUniverse is process-global: load the test design once for all tests.
    netlist.load_primitives("xilinx")
    return netlist.load_verilog(str(DATA / "design.v"))

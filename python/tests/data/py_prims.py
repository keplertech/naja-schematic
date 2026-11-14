"""Primitives built in Python, as kepler-formal's py_tech_files do: a test
of `--py-primitives` and of drawing such a design (test_render.py). No
Liberty, so nothing but this module describes them."""
from najaeda import naja

INPUT = naja.SNLTerm.Direction.Input
OUTPUT = naja.SNLTerm.Direction.Output


def load(db):
    lib = naja.NLLibrary.createPrimitives(db, "py_prims")

    and2 = naja.SNLDesign.createPrimitive(lib, "PY_AND2")
    a = naja.SNLScalarTerm.create(and2, INPUT, "A")
    b = naja.SNLScalarTerm.create(and2, INPUT, "B")
    y = naja.SNLScalarTerm.create(and2, OUTPUT, "Y")
    and2.setTruthTable(0x8)
    naja.SNLDesign.addCombinatorialArcs([a, b], [y])

    dff = naja.SNLDesign.createPrimitive(lib, "PY_DFF")
    d = naja.SNLScalarTerm.create(dff, INPUT, "D")
    c = naja.SNLScalarTerm.create(dff, INPUT, "C")
    q = naja.SNLScalarTerm.create(dff, OUTPUT, "Q")
    naja.SNLDesign.addClockToOutputsArcs(c, [q])
    naja.SNLDesign.addInputsToClockArcs([d], c)

    # A flop whose clock, reset and enable pins naja knows by their term
    # role (SNLBitTerm.setRole()), not by their names: group_by "role".
    ffre = naja.SNLDesign.createPrimitive(lib, "PY_FFRE")
    fd = naja.SNLScalarTerm.create(ffre, INPUT, "D")
    fc = naja.SNLScalarTerm.create(ffre, INPUT, "CK")
    fr = naja.SNLScalarTerm.create(ffre, INPUT, "R")
    fl = naja.SNLScalarTerm.create(ffre, INPUT, "L")
    fq = naja.SNLScalarTerm.create(ffre, OUTPUT, "Q")
    naja.SNLDesign.addClockToOutputsArcs(fc, [fq])
    naja.SNLDesign.addInputsToClockArcs([fd, fr, fl], fc)
    fc.setRole(naja.SNLTermRole.Clock)
    fd.setRole(naja.SNLTermRole.DataInput)
    fr.setRole(naja.SNLTermRole.AsyncReset, naja.SNLActiveLevel.Low)
    fl.setRole(naja.SNLTermRole.Enable)
    fq.setRole(naja.SNLTermRole.DataOutput)

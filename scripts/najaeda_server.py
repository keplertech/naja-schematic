from glob import glob
from najaeda import netlist, naja
import argparse
import asyncio
import websockets
import json

PORT = 8081
MIN_NAJAEDA_VERSION = (0, 7, 24)


def check_najaeda_version():
    version_str = naja.getVersion()
    try:
        version = tuple(int(p) for p in version_str.split("."))
    except ValueError:
        print(f"⚠️ Could not parse najaeda version {version_str!r}; skipping version check")
        return
    if version < MIN_NAJAEDA_VERSION:
        min_str = ".".join(str(p) for p in MIN_NAJAEDA_VERSION)
        raise SystemExit(
            f"najaeda {version_str} is too old (need >= {min_str}, for "
            f"SNLEquipotential.Mode support used by load_equipotential). "
            f"Upgrade with: pip install -U najaeda"
        )

def get_design_ref(ref_msg):
    if not ref_msg:
        return None
    return (
        ref_msg.get("db_id"),
        ref_msg.get("library_id"),
        ref_msg.get("design_id"),
    )

def get_path(top, instance_ids):
    current = top
    path = []
    naja_path = naja.SNLPath()
    for inst_id in instance_ids:
        instance = current.getInstanceByID(inst_id)
        if not instance:
            return None
        current = instance.getModel()
        naja_path = naja.SNLPath(naja_path, instance)
    return naja_path

def has_visible_primitive_instances(design):
    return any(not instance.getModel().isAssign()
               for instance in design.getPrimitiveInstances())

def is_anonymous_constant_net(net):
    # Anonymous scalar constant nets (1'b0/1'b1 tie-offs, e.g. an unconnected
    # input najaeda ties off implicitly) are structural noise, not
    # user-authored signals -- filtered out of both has_nets and the Nets
    # tree listing, same spirit as has_visible_primitive_instances() filtering
    # isAssign() primitives. Named or bus constants are left alone: a name
    # means someone authored it, and a bus is shown as a whole even if every
    # bit happens to be tied.
    return (not net.getName()
            and not isinstance(net, naja.SNLBusNet)
            and (net.isConstant0() or net.isConstant1()))

def has_nets(design):
    # No hasNets() convenience binding (unlike hasTerms()/hasNonPrimitiveInstances());
    # short-circuit on the first visible net rather than materializing the whole list.
    return any(not is_anonymous_constant_net(net) for net in design.getNets())

def get_source_loc(obj):
    # RTL source location for an elaborated object (SNLRTLInfos), populated
    # today only by the SystemVerilog/slang frontend. None means "no link
    # available" -- not an error.
    if not obj.hasSourceLoc():
        return None
    file, line, column, end_line, end_column = obj.getSourceLoc()
    return {
        "file": file,
        "line": line,
        "end_line": end_line,
        "column": column,
        "end_column": end_column,
    }

def serialize_model(model, child_id, name, source_loc=None):
    result = {
        "name": name,
        "child_id": child_id,
        "model_name": model.getName(),
        "design_ref": {
            "db_id": model.getDB().getID(),
            "library_id": model.getLibrary().getID(),
            "design_id": model.getID(),
        },
        "has_terms": model.hasTerms(),
        "has_primitives": has_visible_primitive_instances(model),
        "has_instances": model.hasNonPrimitiveInstances(),
        "has_nets": has_nets(model),
    }
    if source_loc is not None:
        result["source_loc"] = source_loc
    return result

def direction_to_int(direction):
    if direction == naja.SNLTerm.Direction.Input:
        return 0
    elif direction == naja.SNLTerm.Direction.Output:
        return 1
    else:
        return 2

def direction_to_string(direction):
    # Matches Types.h's toString(Direction) on the C++ side (LocalSNLProvider
    # uses the same enum ordering via snlDirToInt/Direction).
    return ["Input", "Output", "Inout"][direction_to_int(direction)]


def resolve_instance_path(top, path):
    # Walk an instance-name path (root excluded) down from the top design,
    # the same convention DiagnosisItem/get_properties use elsewhere (see
    # CLAUDE.md's "Path matching convention") rather than provider-specific
    # numeric ids. Returns (design, instance): the design that owns any
    # terminal lookup at this point (top if path is empty, else the last
    # instance's model), and that last instance itself (None if path is
    # empty). Returns (None, None) if any segment doesn't resolve.
    design = top
    instance = None
    for name in path:
        if design is None:
            return None, None
        instance = design.getInstance(name)
        if not instance:
            return None, None
        design = instance.getModel()
    return design, instance


# Upper bound on nets returned by one trace_driver request (mirrors
# LocalSNLProvider's kMaxTraceNets): every net is a schematic wire plus its
# instance boxes, so an unbounded cone through a big design would bury the view.
MAX_TRACE_NETS = 500


def resolve_start_point(top, path_ids, term_id, bit):
    # The net-component a load_equipotential/trace_driver request starts from:
    # a top-level bit term (empty path), or the SNLOccurrence of the tail
    # instance's inst term. Returns None if it can't be resolved.
    path = get_path(top, path_ids)
    if path is None:
        return None
    if path.empty():
        term = top.getTermByID(term_id)
        if term is None:
            return None
        if bit is not None:
            if not isinstance(term, naja.SNLBusTerm):
                return None
            return term.getBusTermBit(bit)
        return term
    design = path.getModel()
    term = design.getTermByID(term_id)
    if term is None:
        return None
    if bit is not None:
        if not isinstance(term, naja.SNLBusTerm):
            return None
        term = term.getBusTermBit(bit)
    inst_term = path.getTailInstance().getInstTerm(term)
    if inst_term is None:
        return None
    return naja.SNLOccurrence(path.getHeadPath(), inst_term)


def term_key(term):
    return ("T", term.getID(),
            term.getBit() if isinstance(term, naja.SNLBusTermBit) else None)


def occurrence_key(occ):
    inst_term = occ.getInstTerm()
    return (tuple(inst.getID() for inst in occ.getPath().getInstances()),
            inst_term.getInstance().getID(),
            term_key(inst_term.getBitTerm()))


def equipotential_to_json(equipotential, sinks=None):
    # Wire-format body of an equipotential (no "response" key): its top-level
    # terms plus every leaf inst-term occurrence on the net.
    # With `sinks` (a set of term_key/occurrence_key values) only the net's
    # drivers and those listed receivers are emitted -- a driver trace shows
    # the path it followed, not every reader on the net.
    occurrences = []
    terms = []
    for occ in equipotential.getInstTermOccurrences():
        instTerm = occ.getInstTerm()
        if (sinks is not None
                and instTerm.getDirection() == naja.SNLTerm.Direction.Input
                and occurrence_key(occ) not in sinks):
            continue
        # Each path entry is [name, child_id, model_name]: the model name lets
        # the schematic label the hierarchical module boxes it draws around a
        # driver trace (see EquipotentialView's hierarchy grouping).
        path = [[inst.getName(), inst.getID(), inst.getModel().getName()]
                for inst in occ.getPath().getInstances()]
        path.append([instTerm.getInstance().getName(), instTerm.getInstance().getID(),
                     instTerm.getInstance().getModel().getName()])
        term = instTerm.getBitTerm()
        inst_model = instTerm.getInstance().getModel()
        has_instances = (inst_model.hasNonPrimitiveInstances() or
                         has_visible_primitive_instances(inst_model))
        occurrences.append({
            "path": path,
            "term_id": term.getID(),
            "name": term.getName(),
            "direction": direction_to_int(term.getDirection()),
            "bit": term.getBit() if isinstance(term, naja.SNLBusTermBit) else None,
            "design_ref": {
                "db_id": inst_model.getDB().getID(),
                "library_id": inst_model.getLibrary().getID(),
                "design_id": inst_model.getID(),
            },
            "has_instances": has_instances,
            # Lets the view tell whether every pin of this instance is already
            # on screen (solid box) or only a subset (dashed, expandable).
            "bit_term_count": sum(1 for _ in inst_model.getBitTerms()),
            "source_loc": get_source_loc(instTerm.getInstance())
        })
    for term in equipotential.getTerms():
        # A top-level output is a receiver of the net; an input/inout drives it.
        if (sinks is not None
                and term.getDirection() == naja.SNLTerm.Direction.Output
                and term_key(term) not in sinks):
            continue
        terms.append({
            "name": term.getName(),
            "child_id": term.getID(),
            "direction": direction_to_int(term.getDirection()),
            "bit": term.getBit() if isinstance(term, naja.SNLBusTermBit) else None
        })
    return {"occurrences": occurrences, "terms": terms}


def equipotential_key(equipotential):
    # Identity of a net for de-duplicating the cone: the set of things on it.
    return (frozenset(occurrence_key(occ) for occ in equipotential.getInstTermOccurrences()),
            frozenset(term_key(term) for term in equipotential.getTerms()))


def sink_key(sink):
    # `sink` is an SNLOccurrence of an inst term, or a top-level bit term.
    if isinstance(sink, naja.SNLOccurrence):
        return occurrence_key(sink)
    return term_key(sink)


def trace_driver_cone(starts):
    # Breadth-first from the start nets toward the drivers, so each net in the
    # result shares an instance with an earlier one (the layout relies on that
    # to chain nets left-to-right). The cone ends at sequential cells and at
    # cells with no timing model (blackboxes): no combinational arc to cross.
    # Each net carries the receiver pins the trace entered it through (the
    # start pin, or the input pin of the cell being crossed); a net reached
    # through several of them accumulates all of them.
    # Returns ([(SNLEquipotential, sinks)], truncated).
    mode = naja.SNLEquipotential.Mode.TraverseAssigns
    cone = []
    index_of = {}

    def enqueue(sink):
        equipotential = naja.SNLEquipotential(sink, mode=mode)
        key = equipotential_key(equipotential)
        if key not in index_of:
            index_of[key] = len(cone)
            cone.append((equipotential, set()))
        cone[index_of[key]][1].add(sink_key(sink))
        return key

    for start in starts:
        if start is not None:
            enqueue(start)

    i = 0
    while i < len(cone):
        for occ in list(cone[i][0].getInstTermOccurrences()):
            driver = occ.getInstTerm()
            if driver.getDirection() != naja.SNLTerm.Direction.Output:
                continue
            model = driver.getInstance().getModel()
            if model.isSequential() or not model.hasModeling():
                continue
            for inp in naja.SNLInstance.getCombinatorialInputs(driver):
                sink = naja.SNLOccurrence(occ.getPath(), inp)
                # Already-known nets don't grow the cone, so only cap new ones.
                if (len(cone) >= MAX_TRACE_NETS and
                        equipotential_key(naja.SNLEquipotential(sink, mode=mode)) not in index_of):
                    return cone, True
                enqueue(sink)
        i += 1
    return cone, False


async def send_error(websocket, response_type, gui_id=0):
    await websocket.send(json.dumps({
        "response": response_type,
        "gui_id": gui_id
    }))

async def handle_connection(websocket):
    print("🟢 Client connected")
    try:
        async for message in websocket:
            print(f"📩 Received: {message}")
            request = json.loads(message)
            req_type = request.get("request")
            design_ref_message = request.get("design_ref")
            gui_id = request.get("gui_id", 0)

            u = naja.NLUniverse.get()

            if req_type == "load_root":
                print("📦 LoadRoot request received")
                top = u.getTopDesign()

                if not top:
                    send_error(websocket, "root_response")
                else:
                    await websocket.send(json.dumps({
                        "response": "root_response",
                        "root": serialize_model(top, 0, top.getName())
                    }))

            elif req_type in {"load_instance", "load_primitives", "load_instances", "load_terms", "load_nets"}:
                if not design_ref_message:
                    print("⚠️ Missing design_ref in request")
                    continue

                design_ref = get_design_ref(design_ref_message)
                print(f"🔍 {req_type} for: {design_ref}")
                design = u.getSNLDesign(design_ref)

                if not design:
                    await send_error(websocket, f"{req_type}_response", gui_id)
                    continue

                if req_type == "load_instance":
                    await websocket.send(json.dumps({
                        "response": "instance_response",
                        "gui_id": gui_id,
                        "instance": {
                            "design_ref": {
                                "db_id": design.getDB().getID(),
                                "library_id": design.getLibrary().getID(),
                                "design_id": design.getID(),
                            },
                            "has_terms": design.hasTerms(),
                            "has_primitives": has_visible_primitive_instances(design),
                            "has_instances": design.hasNonPrimitiveInstances()
                        }
                    }))

                elif req_type in {"load_primitives", "load_instances"}:
                    children = []
                    instances = (design.getPrimitiveInstances()
                                 if req_type == "load_primitives"
                                 else design.getNonPrimitiveInstances())

                    for instance in instances:
                        model = instance.getModel()
                        if model.isAssign():
                            continue
                        children.append(serialize_model(
                            model, instance.getID(), instance.getName(),
                            get_source_loc(instance)))

                    response_type = req_type.replace("load_", "") + "_response"
                    await websocket.send(json.dumps({
                        "response": response_type,
                        "gui_id": gui_id,
                        "children": children
                    }))
                elif req_type == "load_terms":
                    terms = [
                        { "name": term.getName(),
                          "child_id": term.getID(),
                          "direction": direction_to_int(term.getDirection()),
                          "msb": term.getMSB() if isinstance(term, naja.SNLBusTerm) else None,
                          "lsb": term.getLSB() if isinstance(term, naja.SNLBusTerm) else None,
                        } for term in design.getTerms()
                    ]
                    await websocket.send(json.dumps({
                        "response": "terms_response",
                        "gui_id": gui_id,
                        "children": terms
                    }))
                elif req_type == "load_nets":
                    nets = []
                    for net in design.getNets():
                        if is_anonymous_constant_net(net):
                            continue
                        entry = {"name": net.getName()}
                        if isinstance(net, naja.SNLBusNet):
                            entry["msb"] = net.getMSB()
                            entry["lsb"] = net.getLSB()
                        nets.append(entry)
                    await websocket.send(json.dumps({
                        "response": "nets_response",
                        "gui_id": gui_id,
                        "children": nets
                    }))

            elif req_type == "load_equipotential":
                path_ids = request.get("path", [])
                term_id = request.get("term_id", {})
                bit = request.get("bit", None)
                print(f"⚡ LoadEquipotential request for path: {path_ids} and term_id: {term_id} bit: {bit}")
                start_point = resolve_start_point(u.getTopDesign(), path_ids, term_id, bit)
                print(f"🔍 Start point: {start_point}")
                if start_point is None:
                    await send_error(websocket, "equipotential_response")
                    continue
                equipotential = naja.SNLEquipotential(
                    start_point, mode=naja.SNLEquipotential.Mode.TraverseAssigns)
                response = equipotential_to_json(equipotential)
                response["response"] = "equipotential_response"
                await websocket.send(json.dumps(response))

            elif req_type == "trace_driver":
                # Full combinational fan-in cone of a term's net, back to the
                # drivers: one message holding every net in the cone. "bits"
                # (a list) traces several bits of one bus term at once.
                path_ids = request.get("path", [])
                term_id = request.get("term_id", {})
                bits = request.get("bits", None)
                print(f"⚡ TraceDriver request for path: {path_ids} and term_id: {term_id} bit: {request.get('bit')} bits: {bits}")
                top = u.getTopDesign()
                if bits is not None:
                    starts = [resolve_start_point(top, path_ids, term_id, b) for b in bits]
                else:
                    starts = [resolve_start_point(top, path_ids, term_id, request.get("bit", None))]
                cone, truncated = trace_driver_cone(starts)
                if truncated:
                    print(f"⚠️ trace_driver: cone truncated at {MAX_TRACE_NETS} nets")
                await websocket.send(json.dumps({
                    "response": "trace_driver_response",
                    "equipotentials": [equipotential_to_json(e, sinks) for e, sinks in cone],
                    "truncated": truncated
                }))

            elif req_type == "expand_instance_terms":
                path_key = request.get("path_key", "")
                design_ref = get_design_ref(design_ref_message)
                print(f"🔎 expand_instance_terms for path_key={path_key!r} design_ref={design_ref}")
                design = u.getSNLDesign(design_ref) if design_ref else None
                if not design:
                    print(f"⚠️ expand_instance_terms: design not found for {design_ref}")
                terms = []

                if design:
                    for term in design.getTerms():
                        if isinstance(term, naja.SNLBusTerm):
                            lo, hi = sorted((term.getLSB(), term.getMSB()))
                            for b in range(lo, hi + 1):
                                bit_term = term.getBusTermBit(b)
                                if bit_term:
                                    terms.append({
                                        "name": f"{term.getName()}[{b}]",
                                        "child_id": bit_term.getID(),
                                        "direction": direction_to_int(bit_term.getDirection()),
                                        "bit": b,
                                    })
                        else:
                            terms.append({
                                "name": term.getName(),
                                "child_id": term.getID(),
                                "direction": direction_to_int(term.getDirection()),
                            })

                print(f"📤 Sending expanded_instance_terms for {path_key!r}: {len(terms)} terms")
                await websocket.send(json.dumps({
                    "response": "expanded_instance_terms",
                    "path_key": path_key,
                    "terms": terms
                }))

            elif req_type == "load_instance_internals":
                path_key = request.get("path_key", "")
                design_ref = get_design_ref(design_ref_message)
                model = u.getSNLDesign(design_ref) if design_ref else None
                children = []
                nets = []

                if model:
                    for instance in model.getNonPrimitiveInstances():
                        sub = instance.getModel()
                        children.append(serialize_model(
                            sub, instance.getID(), instance.getName(),
                            get_source_loc(instance)))
                    for instance in model.getPrimitiveInstances():
                        sub = instance.getModel()
                        if sub.isAssign():
                            continue
                        children.append(serialize_model(
                            sub, instance.getID(), instance.getName(),
                            get_source_loc(instance)))

                    def emit_bit_net(bit_net, name, bit):
                        pins = []
                        for comp in bit_net.getComponents():
                            if isinstance(comp, naja.SNLInstTerm):
                                bt = comp.getBitTerm()
                                pins.append({
                                    "name": bt.getName(),
                                    "child_id": bt.getID(),
                                    "direction": direction_to_int(bt.getDirection()),
                                    "bit": bt.getBit() if isinstance(bt, naja.SNLBusTermBit) else None,
                                    "inst_id": comp.getInstance().getID(),
                                })
                            elif isinstance(comp, naja.SNLBitTerm):
                                pins.append({
                                    "name": comp.getName(),
                                    "child_id": comp.getID(),
                                    "direction": direction_to_int(comp.getDirection()),
                                    "bit": comp.getBit() if isinstance(comp, naja.SNLBusTermBit) else None,
                                })
                        if len(pins) < 2:
                            return
                        entry = {"name": name, "pins": pins}
                        if bit is not None:
                            entry["bit"] = bit
                        nets.append(entry)

                    for net in model.getNets():
                        if isinstance(net, naja.SNLBusNet):
                            lo, hi = sorted((net.getLSB(), net.getMSB()))
                            for b in range(lo, hi + 1):
                                bit_net = net.getBit(b)
                                if bit_net:
                                    emit_bit_net(bit_net, net.getName(), b)
                        else:
                            emit_bit_net(net, net.getName(), None)

                await websocket.send(json.dumps({
                    "response": "instance_internals_response",
                    "path_key": path_key,
                    "children": children,
                    "nets": nets
                }))

            elif req_type == "load_source":
                file = request.get("file", "")
                line = request.get("line", 0)
                text = ""
                found = False
                try:
                    with open(file, "r") as f:
                        text = f.read()
                    found = True
                except OSError as e:
                    print(f"⚠️ Failed to read source file {file}: {e}")

                await websocket.send(json.dumps({
                    "response": "source_response",
                    "file": file,
                    "line": line,
                    "found": found,
                    "text": text
                }))

            elif req_type == "get_properties":
                kind = request.get("kind", "instance")
                path = request.get("path", [])
                properties = []
                subject = ""

                top = u.getTopDesign()
                if top is None:
                    print("⚠️ get_properties: no design loaded")
                else:
                    design, instance = resolve_instance_path(top, path)
                    if path and design is None:
                        print(f"⚠️ get_properties: could not resolve instance path {path}")
                    elif kind == "term":
                        terminal = request.get("terminal", "")
                        subject = "/".join(path + [terminal]) if path else terminal
                        if design is not None and terminal:
                            term = design.getTerm(terminal)
                            if term is not None:
                                bit_arg = request.get("bit")
                                if bit_arg is not None:
                                    bit = (term.getBusTermBit(bit_arg)
                                           if isinstance(term, naja.SNLBusTerm) else None)
                                    if bit is not None:
                                        properties = [
                                            {"name": "Name", "value": bit.getName()},
                                            {"name": "Direction", "value": direction_to_string(bit.getDirection())},
                                            {"name": "Bit", "value": str(bit.getBit())},
                                        ]
                                else:
                                    properties = [
                                        {"name": "Name", "value": term.getName()},
                                        {"name": "Direction", "value": direction_to_string(term.getDirection())},
                                    ]
                                    if isinstance(term, naja.SNLBusTerm):
                                        properties.append({"name": "MSB", "value": str(term.getMSB())})
                                        properties.append({"name": "LSB", "value": str(term.getLSB())})
                    elif kind == "net":
                        net_name = request.get("net", "")
                        subject = "/".join(path + [net_name]) if path else net_name
                        if design is not None and net_name:
                            net = design.getNet(net_name)
                            if net is not None:
                                bit_arg = request.get("bit")
                                if bit_arg is not None:
                                    bit = (net.getBit(bit_arg)
                                           if isinstance(net, naja.SNLBusNet) else None)
                                    if bit is not None:
                                        properties = [
                                            {"name": "Name", "value": bit.getName()},
                                            {"name": "Bit", "value": str(bit.getBit())},
                                        ]
                                else:
                                    properties = [
                                        {"name": "Name", "value": net.getName()},
                                    ]
                                    if isinstance(net, naja.SNLBusNet):
                                        properties.append({"name": "MSB", "value": str(net.getMSB())})
                                        properties.append({"name": "LSB", "value": str(net.getLSB())})
                    else:  # "instance"
                        if not path:
                            subject = top.getName()
                            properties = [
                                {"name": "Name", "value": subject},
                                {"name": "Type", "value": "Top Design"},
                            ]
                        elif instance is not None:
                            subject = instance.getName()
                            model = instance.getModel()
                            properties = [
                                {"name": "Name", "value": subject},
                                {"name": "Model", "value": model.getName() if model else ""},
                                {"name": "Type", "value": "Primitive" if model and model.isPrimitive() else "Hierarchical"},
                            ]

                await websocket.send(json.dumps({
                    "response": "properties_response",
                    "subject": subject,
                    "properties": properties
                }))

            else:
                print(f"⚠️ Unknown request type: {req_type}")

    except websockets.exceptions.ConnectionClosed as e:
        print(f"🔴 Client disconnected: {e}")


async def main():
    print(f"🚀 WebSocket server starting on ws://localhost:{PORT}")
    async with websockets.serve(handle_connection, "localhost", PORT):
        await asyncio.Future()  # Run forever


if __name__ == "__main__":
    check_najaeda_version()

    parser = argparse.ArgumentParser(description="najaeda WebSocket Server")
    parser.add_argument("--port", type=int, default=8081,
                        help="Port to run the websocket server on (default: 8081)")
    parser.add_argument("--xilinx", action="store_true",
                        help="Load Xilinx primitives")
    parser.add_argument("--allow_unknown_designs", action="store_true",
                        help="Allow unknown designs when loading the design.")
    parser.add_argument("--liberty", nargs="*", help="List of liberty files to load")
    parser.add_argument("--verilog", type=str,
                        help="Verilog netlist to load")
    args = parser.parse_args()

    PORT = args.port

    if args.xilinx:
        print("📦 Loading Xilinx primitives")
        netlist.load_primitives('xilinx')

    # Load the liberty libraries and the Verilog design
    if args.liberty:
        #if arg contains *, expand to list of files
        expanded_liberty_files = []
        for lib in args.liberty:
            if '*' in lib:
                expanded_liberty_files.extend(glob.glob(lib))
            else:
                expanded_liberty_files.append(lib)
        for lib in expanded_liberty_files:
            print(f"📚 Loading liberty file: {lib}")
            netlist.load_liberty(lib)

    if not args.verilog:
        print("❌ No Verilog file specified. Use --verilog to provide a netlist.")
        exit(1)
    else:
        print(f"📄 Loading Verilog netlist: {args.verilog}")
        config = netlist.VerilogConfig()
        config.allow_unknown_designs = args.allow_unknown_designs
        top = netlist.load_verilog(args.verilog, config=config)
        print(f"✅ Design loaded: {top.get_name()}")

    asyncio.run(main())

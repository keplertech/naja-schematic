"""naja-schematic wire protocol, answered from the live najaeda netlist.

Python side of the request/response protocol the viewer speaks (see
CLAUDE.md, "Wire protocol"); LocalSNLProvider.cpp implements the same
protocol in C++ for the native build -- when changing one side, check the
other. Nothing here knows about transports: see server.py (WebSocket and
stdio) and widget.py (Jupyter/Colab comm channel).
"""
import json
import logging

from najaeda import naja

log = logging.getLogger("naja_schematic")

MIN_NAJAEDA_VERSION = (0, 7, 25)  # keep in step with pyproject.toml


def check_najaeda_version():
    version_str = naja.getVersion()
    try:
        version = tuple(int(p) for p in version_str.split("."))
    except ValueError:
        log.warning("Could not parse najaeda version %r; skipping version check", version_str)
        return
    if version < MIN_NAJAEDA_VERSION:
        min_str = ".".join(str(p) for p in MIN_NAJAEDA_VERSION)
        raise RuntimeError(
            f"najaeda {version_str} is too old (need >= {min_str}). "
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

# Gate/cell function classification, from naja's own modeling of the cell
# (the truth table a Liberty `function` or an NLDB0 gate defines, and its
# sequential model) -- never from its name. A cell naja has no model for is
# "unknown" and draws as the generic box. LocalSNLProvider.cpp's
# getPrimitiveType() mirrors this, same as the rest of the wire protocol.
_PRIMITIVE_TYPE_CHECKS = (
    ("isSequential", "dff"),
    ("isConst0", "tie0"),
    ("isConst1", "tie1"),
    ("isInv", "inv"),
    ("isBuf", "buf"),
    ("isAnd", "and"),
    ("isNand", "nand"),
    ("isOr", "or"),
    ("isNor", "nor"),
    ("isXor", "xor"),
    ("isXnor", "xnor"),
)

def get_primitive_type(model):
    if model is None:
        return "unknown"
    if model.isAssign():
        return "assign"
    for check, ptype in _PRIMITIVE_TYPE_CHECKS:
        if getattr(model, check)():
            return ptype
    return "unknown"

def with_clock(entry, term):
    # Only sent when set: marks a flip-flop's clock pin (see drawDffInstance).
    if term.is_clock():
        entry["clock"] = True
    return entry

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
        "primitive_type": get_primitive_type(model),
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


def _is_instance_id(value):
    # bool is an int subclass: True must not pass for instance 1.
    return isinstance(value, int) and not isinstance(value, bool) and value >= 0


def resolve_request_path(top, request):
    """The instance a request names, root excluded: by "id_path" (instance
    ids, preferred: the only way to name an anonymous instance) or else by
    "path" (instance names) -- see CLAUDE.md's "Path matching convention".

    Returns (design, chain): `chain` lists the instances from the top down,
    `design` owns any terminal/net lookup there (top for an empty path, else
    the last instance's model). (None, None) if the path doesn't resolve.
    Mirrors LocalSNLProvider.cpp's resolveRequestPath().
    """
    design = top
    chain = []
    id_path = request.get("id_path")
    if isinstance(id_path, list):
        for inst_id in id_path:
            if not _is_instance_id(inst_id):
                return None, None
            instance = design.getInstanceByID(inst_id) if design else None
            if instance is None:
                return None, None
            chain.append(instance)
            design = instance.getModel()
        return design, chain
    for name in request.get("path") or []:
        # "" never names an instance: anonymous ones need id_path.
        if not isinstance(name, str) or not name:
            return None, None
        instance = design.getInstance(name) if design else None
        if instance is None:
            return None, None
        chain.append(instance)
        design = instance.getModel()
    return design, chain


def display_name(instance):
    # An instance's name for a human to read, "<#id>" when it's anonymous;
    # same as displayName() in Types.h. Display only, never parsed back.
    return instance.getName() or f"<#{instance.getID()}>"


def display_path(chain):
    return "/".join(display_name(inst) for inst in chain)


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


def net_display_name(net):
    # "name" for a scalar net, "name[bit]" for a bus-net bit; None when the
    # pin is unconnected.
    if net is None:
        return None
    if isinstance(net, naja.SNLBusNetBit):
        return f"{net.getName()}[{net.getBit()}]"
    return net.getName()


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
        occurrences.append(with_clock({
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
            "primitive_type": get_primitive_type(inst_model),
            "has_instances": has_instances,
            # Lets the view tell whether every pin of this instance is already
            # on screen (solid box) or only a subset (dashed, expandable).
            "bit_term_count": sum(1 for _ in inst_model.getBitTerms()),
            "source_loc": get_source_loc(instTerm.getInstance()),
            # Net on this inst term inside the instance's parent design --
            # shown in the schematic's pin hover tooltip.
            "net": net_display_name(instTerm.getNet())
        }, term))
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
            "bit": term.getBit() if isinstance(term, naja.SNLBusTermBit) else None,
            "net": net_display_name(term.getNet())
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


# ---------------------------------------------------------------------------
# Request dispatch
# ---------------------------------------------------------------------------
# Every handler takes the decoded request and returns the list of response
# messages to send back (usually one; [] when the request is ignored). They
# all read the live NLUniverse, so a design edited in the same process (e.g.
# a notebook cell) is what the next request sees.

def _error(response_type, gui_id=0):
    return [{"response": response_type, "gui_id": gui_id}]


def _handle_load_root(u, request):
    log.debug("load_root request")
    top = u.getTopDesign()
    if not top:
        return _error("root_response")
    return [{
        "response": "root_response",
        "root": serialize_model(top, 0, top.getName())
    }]


def _handle_design_request(u, request):
    # load_instance / load_primitives / load_instances / load_terms / load_nets:
    # all address a design by its design_ref.
    req_type = request.get("request")
    gui_id = request.get("gui_id", 0)
    design_ref_message = request.get("design_ref")
    if not design_ref_message:
        log.warning("Missing design_ref in %s request", req_type)
        return []

    design_ref = get_design_ref(design_ref_message)
    log.debug("%s for: %s", req_type, design_ref)
    design = u.getSNLDesign(design_ref)
    if not design:
        return _error(f"{req_type}_response", gui_id)

    if req_type == "load_instance":
        return [{
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
        }]

    if req_type in {"load_primitives", "load_instances"}:
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
        return [{
            "response": req_type.replace("load_", "") + "_response",
            "gui_id": gui_id,
            "children": children
        }]

    if req_type == "load_terms":
        terms = [
            {"name": term.getName(),
             "child_id": term.getID(),
             "direction": direction_to_int(term.getDirection()),
             "msb": term.getMSB() if isinstance(term, naja.SNLBusTerm) else None,
             "lsb": term.getLSB() if isinstance(term, naja.SNLBusTerm) else None,
             } for term in design.getTerms()
        ]
        return [{"response": "terms_response", "gui_id": gui_id, "children": terms}]

    # load_nets
    nets = []
    for net in design.getNets():
        if is_anonymous_constant_net(net):
            continue
        entry = {"name": net.getName()}
        if isinstance(net, naja.SNLBusNet):
            entry["msb"] = net.getMSB()
            entry["lsb"] = net.getLSB()
        nets.append(entry)
    return [{"response": "nets_response", "gui_id": gui_id, "children": nets}]


def _handle_load_equipotential(u, request):
    path_ids = request.get("path", [])
    term_id = request.get("term_id", {})
    bit = request.get("bit", None)
    log.debug("load_equipotential for path: %s term_id: %s bit: %s", path_ids, term_id, bit)
    start_point = resolve_start_point(u.getTopDesign(), path_ids, term_id, bit)
    if start_point is None:
        return _error("equipotential_response")
    equipotential = naja.SNLEquipotential(
        start_point, mode=naja.SNLEquipotential.Mode.TraverseAssigns)
    response = equipotential_to_json(equipotential)
    response["response"] = "equipotential_response"
    return [response]


def _handle_trace_driver(u, request):
    # Full combinational fan-in cone of a term's net, back to the drivers:
    # one message holding every net in the cone. "bits" (a list) traces
    # several bits of one bus term at once.
    path_ids = request.get("path", [])
    term_id = request.get("term_id", {})
    bits = request.get("bits", None)
    log.debug("trace_driver for path: %s term_id: %s bit: %s bits: %s",
              path_ids, term_id, request.get("bit"), bits)
    top = u.getTopDesign()
    if bits is not None:
        starts = [resolve_start_point(top, path_ids, term_id, b) for b in bits]
    else:
        starts = [resolve_start_point(top, path_ids, term_id, request.get("bit", None))]
    cone, truncated = trace_driver_cone(starts)
    if truncated:
        log.warning("trace_driver: cone truncated at %d nets", MAX_TRACE_NETS)
    reply = {
        "response": "trace_driver_response",
        "equipotentials": [equipotential_to_json(e, sinks) for e, sinks in cone],
        "truncated": truncated
    }
    # Echoed so the viewer knows which of its traces the nets are for.
    if "trace_id" in request:
        reply["trace_id"] = request["trace_id"]
    return [reply]


def bit_terms_json(design):
    # Every pin of a design, bus terms expanded bit by bit ("data[3]", with
    # "bit" set) so each gets its own schematic pin.
    terms = []
    for term in design.getTerms():
        if isinstance(term, naja.SNLBusTerm):
            lo, hi = sorted((term.getLSB(), term.getMSB()))
            for b in range(lo, hi + 1):
                bit_term = term.getBusTermBit(b)
                if bit_term:
                    terms.append(with_clock({
                        "name": f"{term.getName()}[{b}]",
                        "child_id": bit_term.getID(),
                        "direction": direction_to_int(bit_term.getDirection()),
                        "bit": b,
                    }, bit_term))
        else:
            terms.append(with_clock({
                "name": term.getName(),
                "child_id": term.getID(),
                "direction": direction_to_int(term.getDirection()),
            }, term))
    return terms


def _handle_expand_instance_terms(u, request):
    # The requesting box's instance path (names and ids), echoed back as is.
    instance_path = request.get("instance_path", [])
    instance_id_path = request.get("instance_id_path", [])
    design_ref = get_design_ref(request.get("design_ref"))
    log.debug("expand_instance_terms for instance_path=%r design_ref=%s", instance_path, design_ref)
    design = u.getSNLDesign(design_ref) if design_ref else None
    if not design:
        log.warning("expand_instance_terms: design not found for %s", design_ref)
    return [{
        "response": "expanded_instance_terms",
        "instance_path": instance_path,
        "instance_id_path": instance_id_path,
        "terms": bit_terms_json(design) if design else []
    }]


def _handle_resolve_instance(u, request):
    # Everything the viewer needs to show one instance, given only its path
    # -- "id_path" (preferred) or "path" (names), as get_properties and
    # diagnosis items: the child_id/model of each path level, to reveal it
    # in the tree and address its pins, plus its full pin list, to draw it
    # alone in the schematic as a starting point. The reply's path/id_path
    # are the resolved instance's when found, the request's otherwise.
    top = u.getTopDesign()
    reply = {"response": "instance_resolved",
             "path": list(request.get("path") or []), "found": False}
    if isinstance(request.get("id_path"), list):
        reply["id_path"] = list(request["id_path"])
    if top is None:
        return [reply]
    design, chain = resolve_request_path(top, request)
    if design is None:
        log.warning("resolve_instance: could not resolve instance path %s",
                    request.get("id_path", request.get("path")))
        return [reply]
    reply["path"] = [inst.getName() for inst in chain]
    reply["id_path"] = [inst.getID() for inst in chain]
    reply["found"] = True
    if not chain:
        return [reply]  # the top design itself
    reply["instance"] = {
        "path": [[inst.getName(), inst.getID(), inst.getModel().getName()] for inst in chain],
        "primitive_type": get_primitive_type(design),
        "design_ref": {
            "db_id": design.getDB().getID(),
            "library_id": design.getLibrary().getID(),
            "design_id": design.getID(),
        },
        "has_instances": (design.hasNonPrimitiveInstances() or
                          has_visible_primitive_instances(design)),
        "source_loc": get_source_loc(chain[-1]),
        "terms": bit_terms_json(design),
    }
    return [reply]


def _handle_instance_selected(u, request):
    # A notification, not a request: the viewer reports the instance the
    # user selected, by "path" (names) and "id_path". The notebook widget
    # intercepts it (Schematic.selected); elsewhere it's only logged.
    top = u.getTopDesign()
    _, chain = resolve_request_path(top, request) if top else (None, None)
    log.info("Viewer selected instance: %s",
             (display_path(chain) or "<top>") if chain is not None
             else request.get("id_path", request.get("path")))
    return []


def _handle_load_instance_internals(u, request):
    instance_path = request.get("instance_path", [])
    instance_id_path = request.get("instance_id_path", [])
    design_ref = get_design_ref(request.get("design_ref"))
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

    return [{
        "response": "instance_internals_response",
        "instance_path": instance_path,
        "instance_id_path": instance_id_path,
        "children": children,
        "nets": nets
    }]


def _handle_load_source(u, request):
    file = request.get("file", "")
    line = request.get("line", 0)
    text = ""
    found = False
    try:
        with open(file, "r") as f:
            text = f.read()
        found = True
    except OSError as e:
        log.warning("Failed to read source file %s: %s", file, e)
    return [{
        "response": "source_response",
        "file": file,
        "line": line,
        "found": found,
        "text": text
    }]


def _handle_get_properties(u, request):
    kind = request.get("kind", "instance")
    properties = []
    subject = ""

    top = u.getTopDesign()
    if top is None:
        log.warning("get_properties: no design loaded")
    else:
        design, chain = resolve_request_path(top, request)
        at = display_path(chain) if chain else ""
        if design is None:
            log.warning("get_properties: could not resolve instance path %s",
                        request.get("id_path", request.get("path")))
        elif kind == "term":
            terminal = request.get("terminal", "")
            subject = f"{at}/{terminal}" if at else terminal
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
            subject = f"{at}/{net_name}" if at else net_name
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
            if not chain:
                subject = top.getName()
                properties = [
                    {"name": "Name", "value": subject},
                    {"name": "Type", "value": "Top Design"},
                ]
            else:
                instance = chain[-1]
                subject = display_name(instance)
                model = instance.getModel()
                properties = [
                    {"name": "Name", "value": instance.getName()},  # "" when anonymous
                    {"name": "ID", "value": str(instance.getID())},
                    {"name": "Model", "value": model.getName() if model else ""},
                    {"name": "Type", "value": "Primitive" if model and model.isPrimitive() else "Hierarchical"},
                ]

    return [{
        "response": "properties_response",
        "subject": subject,
        "properties": properties
    }]


_HANDLERS = {
    "load_root": _handle_load_root,
    "load_instance": _handle_design_request,
    "load_primitives": _handle_design_request,
    "load_instances": _handle_design_request,
    "load_terms": _handle_design_request,
    "load_nets": _handle_design_request,
    "load_equipotential": _handle_load_equipotential,
    "trace_driver": _handle_trace_driver,
    "expand_instance_terms": _handle_expand_instance_terms,
    "load_instance_internals": _handle_load_instance_internals,
    "load_source": _handle_load_source,
    "get_properties": _handle_get_properties,
    "resolve_instance": _handle_resolve_instance,
    "instance_selected": _handle_instance_selected,
}


def handle_request(request):
    """Answer one decoded viewer request.

    Returns the list of response messages (dicts) to send back, in order.
    Transport-agnostic: the WebSocket server, the stdio server and the
    notebook widget all wrap this.
    """
    req_type = request.get("request")
    handler = _HANDLERS.get(req_type)
    if handler is None:
        log.warning("Unknown request type: %s", req_type)
        return []
    u = naja.NLUniverse.get()
    if u is None:
        # Nothing loaded yet (e.g. a widget shown before any netlist cell ran).
        return _error("root_response") if req_type == "load_root" else []
    try:
        return handler(u, request)
    except Exception:
        # One bad request must not take the connection/widget down with it.
        log.exception("Error while handling %s request", req_type)
        return []


def handle_message(message):
    """Same as handle_request(), on a JSON string; returns JSON strings."""
    return [json.dumps(r) for r in handle_request(json.loads(message))]


def _instance_id_path(instance):
    # A najaeda netlist.Instance's id path (top excluded); None for anything else.
    ids = getattr(instance, "pathIDs", None)
    return list(ids) if ids is not None else None


def _instance_name_path(ids):
    # Instance names down an id path, "" for anonymous levels.
    universe = naja.NLUniverse.get()
    design = universe.getTopDesign() if universe else None
    names = []
    for inst_id in ids:
        inst = design.getInstanceByID(inst_id) if design else None
        if inst is None:
            raise ValueError(f"instance id path {ids} does not resolve in the loaded design")
        names.append(inst.getName())
        design = inst.getModel()
    return names


def _diagnosis_item(item):
    # An item whose "path" is a najaeda netlist.Instance gets its id_path
    # (what the viewer matches first) and the matching name path.
    ids = _instance_id_path(item.get("path"))
    if ids is None:
        return item
    return {**item, "path": _instance_name_path(ids), "id_path": ids}


def diagnosis_response(items):
    """Build a diagnosis_response push message from a list of diagnosis
    items (dicts with kind/path/id_path/terminal/severity/message/source),
    or from a {"items": [...]} document as File > Load Diagnosis JSON...
    accepts. An item names its instance by "id_path" (instance ids,
    preferred) and/or "path" (instance names); "path" may also be a najaeda
    netlist.Instance, turned into both here."""
    if isinstance(items, dict):
        items = items.get("items", [])
    return {"response": "diagnosis_response", "items": [_diagnosis_item(i) for i in items]}


def focus_instance(path=None, id_path=None):
    """Build a focus_instance push message: the viewer resolves the instance
    -- by `id_path` (a list of instance ids, top excluded) when given, else
    by `path` (a list of instance names); [] = the top design -- reveals and
    selects it in the tree, and draws it alone in the schematic."""
    message = {"response": "focus_instance",
               "path": [str(name) for name in (path or [])]}
    if id_path is not None:
        message["id_path"] = [int(i) for i in id_path]
    return message


def design_changed():
    """Build a design_changed push message: the design behind the viewer was
    replaced or edited, so the viewer drops its tree, schematic, diagnoses
    and selection and loads the root again. The sender stamps it with the
    new design generation (see session.ViewerSession): the viewer then
    ignores messages for any other generation, and stamps its own requests
    with it."""
    return {"response": "design_changed"}

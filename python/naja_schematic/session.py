"""One viewer's state on the Python side, whatever carries its messages.

A ViewerSession answers the viewer's requests (protocol.handle_request()
against the live NLUniverse), keeps the host's pushes -- diagnoses, the
focused instance and the nets shown with show_net() -- and re-sends them
after every root_response (a viewer that reloads has lost them), and
records the instance the viewer selects. The notebook widget, the CLI servers and the embeddable
ViewerServer all wrap one; they only differ in how messages travel.

Universe access is split by who starts it:
- what the viewer starts (its requests, and resolving a selection sent by
  names only) runs through `run(fn)`, which the host can hook to serialize
  it with its own design operations (a lock, or a hop to its own thread);
- what the host starts (annotate(), show_instance(), show_net(), selected)
  touches the universe directly, on the host's thread: call those where the host's own
  design operations are allowed. They never go through `run`, so a host
  holding its lock -- or on the very thread `run` hops to -- can call them
  without deadlocking.

A host that replaces the design calls design_changed(): the session moves to
the next design generation, and requests the viewer made for an older one
are dropped (see ViewerSession).
"""
import dataclasses
import json
import logging
import threading
import time

from najaeda import naja

from . import protocol
from .review import ReviewError

log = logging.getLogger("naja_schematic")


def names_to_ids(names):
    """Instance-name path (top excluded) -> instance-id path, walking down
    from the top design; None if a name doesn't resolve. An anonymous
    instance has no name to walk by ("" never resolves): use its ids."""
    universe = naja.NLUniverse.get()
    design = universe.getTopDesign() if universe else None
    ids = []
    for name in names:
        inst = design.getInstance(name) if design and name else None
        if inst is None:
            return None
        ids.append(inst.getID())
        design = inst.getModel()
    return ids


def ids_to_names(ids):
    """Instance-id path -> instance names ("" for an anonymous level); None
    if an id doesn't resolve."""
    universe = naja.NLUniverse.get()
    design = universe.getTopDesign() if universe else None
    names = []
    for inst_id in ids:
        inst = design.getInstanceByID(inst_id) if design else None
        if inst is None:
            return None
        names.append(inst.getName())
        design = inst.getModel()
    return names


def is_id(value):
    return isinstance(value, int) and not isinstance(value, bool)


def instance_paths(target):
    """Normalize what show_instance() accepts to (names, ids): the instance's
    name path and id path, top excluded ([] = the top design). `target` is a
    naja SNLPath (or a najaeda netlist.Instance), a list/tuple of instance
    ids (ints, as SNLPath.getInstanceIDs()), a list/tuple of names, or a
    single name for a child of the top design. A string is one name, never split on "/": escaped names
    can contain it. Ids reach anonymous instances; names can't. Raises
    ValueError if the path doesn't resolve in the loaded design."""
    ids = protocol.instance_id_path(target)
    if ids is not None:
        names = None
    elif isinstance(target, str):
        names = [target]
    elif isinstance(target, (list, tuple)):
        if target and all(is_id(seg) for seg in target):
            ids = list(target)
        elif all(isinstance(seg, str) for seg in target):
            names = list(target)
        else:
            raise TypeError("expected a path of instance names (str) or of "
                            f"instance ids (int), got {list(target)!r}")
    else:
        raise TypeError(f"expected an SNLPath, a najaeda Instance, a list of names or ids, "
                        f"or a name, got {type(target).__name__}")
    if ids is None:
        ids = names_to_ids(names)
        if ids is None:
            raise ValueError(f"no instance {names!r} in the loaded design")
    else:
        names = ids_to_names(ids)
        if names is None:
            raise ValueError(f"instance id path {ids} does not resolve in the loaded design")
    return names, ids


def instance_path(target):
    """The name path of what show_instance() accepts (see instance_paths())."""
    return instance_paths(target)[0]


def _top_design():
    universe = naja.NLUniverse.get()
    design = universe.getTopDesign() if universe else None
    if design is None:
        raise ValueError("no design is loaded")
    return design


def _model_at(ids):
    # The model of the instance at `ids` (the top design for []).
    design = _top_design()
    for inst_id in ids:
        inst = design.getInstanceByID(inst_id)
        if inst is None:
            raise ValueError(f"instance id path {list(ids)} does not resolve in the loaded design")
        design = inst.getModel()
    return design


def _same_design(a, b):
    return a is not None and b is not None and a.getDB().getID() == b.getDB().getID() \
        and a.getLibrary().getID() == b.getLibrary().getID() and a.getID() == b.getID()


def _term_display_name(term):
    # Same as displayTermName() in the viewer: "<term #id>" when unnamed.
    return term.getName() or f"<term #{term.getID()}>"


def _display_prefix(ids, names):
    # "u1/u2/" for a trace label, "<#id>" for an anonymous level, "" at the
    # top: same as traceLabel() in the viewer. Display only.
    prefix = "/".join(name or f"<#{i}>" for name, i in zip(names, ids))
    return prefix + "/" if prefix else ""


def _pin_of(term, bit, what):
    # (term_id, bit, display name) of the pin an SNL term (scalar, bus, or
    # bus bit) and an optional bit name, as load_equipotential takes it.
    if isinstance(term, naja.SNLBusTermBit):
        if bit is not None and bit != term.getBit():
            raise ValueError(f"{what} is bit {term.getBit()}, not bit {bit}")
        bus = term.getBus()
        return bus.getID(), term.getBit(), f"{_term_display_name(bus)}[{term.getBit()}]"
    if isinstance(term, naja.SNLBusTerm):
        if bit is None:
            raise ValueError(f"{what} is a bus ({_term_display_name(term)}"
                             f"[{term.getMSB()}:{term.getLSB()}]): give one bit")
        if not is_id(bit) or term.getBusTermBit(bit) is None:
            raise ValueError(f"{what} has no bit {bit!r}")
        return term.getID(), bit, f"{_term_display_name(term)}[{bit}]"
    if bit is not None:
        raise ValueError(f"{what} is not a bus: it has no bit {bit!r}")
    return term.getID(), None, _term_display_name(term)


def _pin_on_equipotential(equipotential):
    # A pin on an SNLEquipotential to draw it from: its first top-level
    # port, else its first leaf pin.
    for term in equipotential.getTerms():
        return [], term
    for occ in equipotential.getInstTermOccurrences():
        return list(occ.getPath().getInstanceIDs()) + [occ.getInstTerm().getInstance().getID()], \
            occ.getInstTerm().getBitTerm()
    raise ValueError("the equipotential has no pin to draw it from")


def net_pin(target):
    """Normalize what show_net() accepts to the pin its net is drawn from:
    a dict with `id_path` and `path` (the pin's instance, top excluded; []
    for a top-level port), `term_id`, `bit` (None for a scalar) and
    `label`, a default trace label. Raises ValueError (TypeError for an
    unsupported kind) with the reason when it doesn't resolve.

    `target` is one bit, never a whole bus:
    - a najaeda netlist.Term (one bit), netlist.Net (one bit: the net is
      drawn from a port of its design, else from its first instance pin),
      or netlist.Equipotential;
    - a naja SNLOccurrence of an inst term, an SNLInstTerm of a child of
      the top design, a top-level SNLScalarTerm/SNLBusTermBit, or an
      SNLEquipotential (drawn from its first top-level port, else its first
      leaf pin);
    - a tuple (instance, terminal) or (instance, terminal, bit): `instance`
      as show_instance() takes it ([] = a top-level port), `terminal` a
      name or a term id.
    """
    label = None
    if isinstance(target, tuple) and len(target) in (2, 3):
        instance, terminal = target[0], target[1]
        bit = target[2] if len(target) == 3 else None
        names, ids = instance_paths(instance)
        design = _model_at(ids)
        if isinstance(terminal, str):
            term = design.getTerm(terminal)
        elif is_id(terminal):
            term = design.getTermByID(terminal)
        else:
            raise TypeError(f"expected a terminal name or id, got {terminal!r}")
        if term is None:
            raise ValueError(f"no terminal {terminal!r} on {'/'.join(names) or 'the top design'}"
                             f" ({design.getName()})")
    elif hasattr(target, "termID") and hasattr(target, "pathIDs"):  # najaeda Term
        ids, bit = list(target.pathIDs), target.bit
        term = _model_at(ids).getTermByID(target.termID)
        if term is None:
            raise ValueError(f"terminal {target.termID} does not resolve in the loaded design")
    elif hasattr(target, "pathIDs") and (hasattr(target, "net") or hasattr(target, "net_concat")):
        # najaeda Net: the net of the design at pathIDs.
        net = target.net if hasattr(target, "net") else None
        if net is None:
            concat = list(target.net_concat)
            if len(concat) != 1:
                raise ValueError("a concatenation of several nets: show one bit")
            net = concat[0]
        if isinstance(net, naja.SNLBusNet):
            raise ValueError(f"net {net.getName()} is a bus: show one bit (net.get_bit(i))")
        head = list(target.pathIDs)
        _model_at(head)  # validates the path
        bit = None
        bit_terms = list(net.getBitTerms())
        if bit_terms:
            ids, term = head, bit_terms[0]
        else:
            # A cell's pin rather than an assign's (an anonymous connector
            # the schematic doesn't draw as a gate), when there is one.
            inst_terms = sorted(net.getInstTerms(),
                                key=lambda it: bool(it.getInstance().getModel().isAssign()))
            if not inst_terms:
                raise ValueError(f"net {net.getName() or '#' + str(net.getID())} connects "
                                 "no pin: there is nothing to draw it from")
            ids = head + [inst_terms[0].getInstance().getID()]
            term = inst_terms[0].getBitTerm()
        # Named after the net, in the instance owning it.
        net_name = net.getName() or f"<net #{net.getID()}>"
        if isinstance(net, naja.SNLBusNetBit):
            net_name += f"[{net.getBit()}]"
        label = _display_prefix(head, ids_to_names(head)) + net_name + " (net)"
    elif hasattr(target, "equi") or isinstance(target, naja.SNLEquipotential):
        ids, term = _pin_on_equipotential(getattr(target, "equi", target))
        bit = None
    elif isinstance(target, naja.SNLOccurrence):
        inst_term = target.getInstTerm()
        if inst_term is None:
            raise ValueError("an SNLOccurrence of an instance, not of a pin: "
                             "use show_instance(), or name one of its pins")
        ids = list(target.getPath().getInstanceIDs()) + [inst_term.getInstance().getID()]
        term, bit = inst_term.getBitTerm(), None
    elif isinstance(target, naja.SNLInstTerm):
        inst = target.getInstance()
        if not _same_design(inst.getDesign(), _top_design()):
            raise ValueError("an SNLInstTerm only names a pin of a child of the top design: "
                             "pass an SNLOccurrence (or a najaeda Term) for a deeper one")
        ids, term, bit = [inst.getID()], target.getBitTerm(), None
    elif isinstance(target, (naja.SNLBitTerm, naja.SNLBusTerm)):
        if not _same_design(target.getDesign(), _top_design()):
            raise ValueError("an SNL term alone only names a top-level port: pass an "
                             "SNLOccurrence (or a najaeda Term) for an instance pin")
        ids, term, bit = [], target, None
    else:
        raise TypeError("expected a najaeda Term, Net or Equipotential, a naja SNLOccurrence, "
                        "SNLInstTerm, top-level term or SNLEquipotential, or an "
                        f"(instance, terminal[, bit]) tuple, got {type(target).__name__}")
    names = ids_to_names(ids)
    if names is None:
        raise ValueError(f"instance id path {ids} does not resolve in the loaded design")
    term_id, bit, pin = _pin_of(term, bit, "the terminal")
    if label is None:
        label = _display_prefix(ids, names) + pin + " (net)"
    return {"id_path": ids, "path": names, "term_id": term_id, "bit": bit, "label": label}


def trace_color(color):
    """Normalize a trace color -- "#rrggbb" or an (r, g, b) tuple of 0-255
    ints -- to "#rrggbb". The viewer refuses near-black colors (the
    convergence color of shared gates) and near-white ones (invisible on its
    white canvas); so does this, with a ValueError."""
    if isinstance(color, str):
        hexa = color[1:] if color.startswith("#") else color
        if len(hexa) != 6 or any(c not in "0123456789abcdefABCDEF" for c in hexa):
            raise ValueError(f"expected a color as '#rrggbb', got {color!r}")
        rgb = tuple(int(hexa[i:i + 2], 16) for i in (0, 2, 4))
    elif isinstance(color, (tuple, list)) and len(color) == 3 and all(
            is_id(c) and 0 <= c <= 255 for c in color):
        rgb = tuple(color)
    else:
        raise TypeError(f"expected a color as '#rrggbb' or (r, g, b), got {color!r}")
    if all(c <= 80 for c in rgb):
        raise ValueError(f"color {color!r} is too dark: near-black marks gates traces share")
    if all(c >= 190 for c in rgb):
        raise ValueError(f"color {color!r} is too light to show on the white canvas")
    return "#%02x%02x%02x" % rgb


def selected_instance(id_path, path):
    """A selection (id path and/or name path) as a naja SNLPath (empty for
    the top design), or None if there is none or it no longer exists in the
    netlist."""
    if id_path is not None:
        ids = id_path
        if ids_to_names(ids) is None:
            return None
    elif path is not None:
        ids = names_to_ids(path)
        if ids is None:
            return None
    else:
        return None
    return protocol.get_path(naja.NLUniverse.get().getTopDesign(), ids)


def lock_runner(lock):
    """A `run` for ViewerSession that holds `lock` (any context manager)
    around each call."""
    def run(fn):
        with lock:
            return fn()
    return run


_REVIEW_WRITES = ("review_add_thread", "review_add_event")


class ViewerSession:
    """The Python side of one viewer (or of several showing the same view).

    `push(message)` sends a JSON string to the viewer(s) unasked; it may be
    None until a transport is attached. `run(fn)` returns fn(), called with
    exclusive access to the universe (default: fn() under a private RLock).
    `dispatch(fn)` calls fn() -- a selection notification -- eventually, in
    the order given (default: right away, on the calling thread).
    `diagnosis` pre-loads diagnosis items, sent after the first root load.
    `source_roots` lists the directories relative source file references
    are looked up in, `source_maps` (FROM, TO) path prefixes to rewrite for
    files recorded on another machine (see protocol.resolve_source_file()).
    `review`, a review.Review, is sent to the viewer after every root load
    and written by its review_* requests (see docs/review_format.md).

    Design generations: the session numbers the designs it has shown,
    starting at 0; design_changed() moves to the next one. Every message to
    the viewer carries the "generation" it was produced for, and the viewer
    stamps its requests with the generation it is showing. A request stamped
    with any other generation was made for a design that is gone: it is
    dropped unanswered and without effect -- in particular a selection never
    reaches the host -- even if the ids it names exist (reused) in the new
    design. The check runs inside `run`, so it is atomic with a design swap
    the host makes under the same lock, followed by design_changed(), before
    releasing it. Unstamped requests (a viewer's first load_root, or a viewer
    predating generations) are answered for the current design.
    """

    def __init__(self, push=None, run=None, diagnosis=None, dispatch=None, source_roots=(),
                 source_maps=(), review=None):
        self._push = push
        self.review = review
        # Stamped now and at each design_changed(): the design the viewer
        # shows was loaded by then, so a source file modified later is stale.
        self._source_paths = protocol.SourcePaths(
            tuple(source_roots), tuple((str(a), str(b)) for a, b in source_maps), time.time())
        self._run = run or lock_runner(threading.RLock())
        self._dispatch = dispatch or (lambda fn: fn())
        self._state_lock = threading.Lock()  # generation, pushes, selection; not the universe
        self._generation = 0
        self._diagnosis_push = (protocol.diagnosis_response(diagnosis)
                                if diagnosis is not None else None)
        self._focus_push = None
        # show_net() pushes since the view was last cleared, in order: they
        # are re-sent after the focus, which clears the view when drawn.
        self._net_pushes = []
        self._select_callbacks = []
        self._svg_callbacks = []  # one-shot, for the next svg_exported
        self.svg = None
        self.selected_id_path = None
        self.selected_path = None

    @property
    def generation(self):
        """The current design generation (0 until design_changed())."""
        with self._state_lock:
            return self._generation

    @staticmethod
    def _stamp(message, generation):
        return json.dumps({**message, "generation": generation})

    # -- viewer -> host ----------------------------------------------------

    def answer(self, message):
        """Answer one JSON message from the viewer; returns the JSON
        strings to send back to that viewer, in order."""
        try:
            request = json.loads(message)
        except json.JSONDecodeError as e:
            log.error("Ignoring malformed viewer request %r: %s", message, e)
            return []
        if not isinstance(request, dict):
            log.error("Ignoring viewer message that is not an object: %r", message)
            return []
        stamp = request.get("generation")
        if stamp is not None and not is_id(stamp):
            log.error("Ignoring viewer request with a bad generation: %r", message)
            return []
        kind = request.get("request")

        def handle():
            # Under `run`: a host that swaps the design and calls
            # design_changed() under the same lock can't slip in between the
            # check and the answer.
            with self._state_lock:
                generation = self._generation
                pushes = [p for p in (self._diagnosis_push, self._focus_push) if p]
                pushes += self._net_pushes
                if self._svg_callbacks:
                    # An export asked before the view was up got lost with
                    # it: ask again once the view is (re)loaded.
                    pushes.append(protocol.export_svg())
            if stamp is not None and stamp != generation:
                return generation, None, pushes
            if kind == "instance_selected":
                return generation, self._resolve_selection(request), pushes
            if kind == "svg_exported":
                svg = request.get("svg")
                return generation, svg if isinstance(svg, str) else "", pushes
            if kind in _REVIEW_WRITES:
                return generation, self._review_write(kind, request), pushes
            replies = protocol.handle_request(request, self._source_paths)
            if self.review is not None and any(r.get("response") == "root_response"
                                               for r in replies):
                # Under `run`: anchors resolve against the universe. A root
                # load is where an edited design shows: identify it afresh.
                self.review.design_changed()
                pushes = pushes + [self._review_response()]
            return generation, replies, pushes

        generation, result, pushes = self._run(handle)
        if result is None:
            log.info("Dropping %s made for design generation %s (now %s)", kind, stamp, generation)
            return []
        if kind == "instance_selected":
            self._commit_selection(generation, *result)
            return []  # a notification: no reply
        if kind == "svg_exported":
            self._commit_svg(generation, result)
            return []
        if kind in _REVIEW_WRITES and self._push is not None and result[0].get(
                "response") == "review_response":
            # Every viewer shows the review, not just the one that wrote.
            self._send(self._stamp(result[0], generation))
            return []
        replies = []
        for reply in result:
            replies.append(self._stamp(reply, generation))
            # The viewer clears its diagnoses and selection on every
            # root_response, so re-apply them after the root is (re)loaded.
            if reply.get("response") == "root_response":
                replies.extend(self._stamp(p, generation) for p in pushes)
        return replies

    def _review_response(self):
        return self.review.response(naja.NLUniverse.get().getTopDesign()
                                    if naja.NLUniverse.get() else None)

    def _review_write(self, kind, request):
        # Under `run`. A refused write is an error reply to that viewer only.
        if self.review is None:
            return [{"response": "error", "request": kind,
                     "message": "no review directory: start with --review DIR"}]
        try:
            if kind == "review_add_thread":
                u = naja.NLUniverse.get()
                self.review.add_thread(
                    u.getTopDesign() if u else None, request.get("anchor"), request.get("text"),
                    severity=request.get("severity", "info"), labels=request.get("labels"),
                    title=request.get("title", ""), view=request.get("view"))
            else:
                self.review.add_event(request.get("thread"), text=request.get("text"),
                                      status=request.get("status"),
                                      severity=request.get("severity"),
                                      labels=request.get("labels"))
        except (ReviewError, OSError) as e:
            log.warning("%s refused: %s", kind, e)
            return [{"response": "error", "request": kind, "message": str(e)}]
        return [self._review_response()]

    @staticmethod
    def _resolve_selection(request):
        path, ids = request.get("path"), request.get("id_path")
        if not (isinstance(ids, list) and all(is_id(i) for i in ids)):
            # A viewer that only sends names: find the ids by them.
            ids = names_to_ids(path) if isinstance(path, list) else None
        return ids, (list(path) if isinstance(path, list) else None)

    def _commit_selection(self, generation, ids, path):
        with self._state_lock:
            if generation != self._generation:
                # design_changed() came between the answer and here.
                return
            self.selected_path, self.selected_id_path = path, ids
        self._dispatch(lambda: self._notify(generation, ids, path))

    def _notify(self, generation, ids, path):
        # Checked again at delivery: a notification queued before a
        # design_changed() must not reach the host after it.
        with self._state_lock:
            if generation != self._generation:
                return
            callbacks = list(self._select_callbacks)
        # Outside every lock: a callback may take the host's lock itself.
        for callback in callbacks:
            try:
                callback(ids, path)
            except Exception:
                log.exception("Error in a selection callback")

    def _commit_svg(self, generation, svg):
        with self._state_lock:
            if generation != self._generation:
                return
            self.svg = svg
            callbacks, self._svg_callbacks = self._svg_callbacks, []
        for callback in callbacks:
            self._dispatch(lambda cb=callback: self._deliver_svg(cb, svg))

    @staticmethod
    def _deliver_svg(callback, svg):
        try:
            callback(svg)
        except Exception:
            log.exception("Error in an SVG export callback")

    def on_select(self, callback):
        """Call `callback(id_path, path)` on each selection the viewer
        reports for the current design, and with (None, None) when
        design_changed() clears it; through `dispatch`, in order. Returns
        `callback`, for remove_select_callback()."""
        with self._state_lock:
            self._select_callbacks.append(callback)
        return callback

    def remove_select_callback(self, callback):
        with self._state_lock:
            self._select_callbacks.remove(callback)

    # -- host -> viewer ----------------------------------------------------

    def _send(self, message):
        if self._push is not None:
            self._push(message)

    def annotate(self, items):
        """Overlay diagnosis items on the view, now and after every reload
        (see protocol.diagnosis_response() for the item shape). Pass [] to
        clear."""
        message = protocol.diagnosis_response(items)
        with self._state_lock:
            self._diagnosis_push = message
            generation = self._generation
        self._send(self._stamp(message, generation))

    def show_instance(self, target):
        """Reveal, select and draw one instance, now and after every
        reload (see instance_paths() for what `target` can be)."""
        names, ids = instance_paths(target)
        message = protocol.focus_instance(names, ids)
        with self._state_lock:
            self._focus_push = message
            self._net_pushes = []  # the focus clears the view
            generation = self._generation
        self._send(self._stamp(message, generation))

    def show_net(self, target, *, label=None, color=None, add=True, trace=False):
        """Draw the net on a pin as a colored one-net trace, as the tree's
        Show Equipotential does: its own color and a row in the Traces tab.
        Now, and after every reload (until show_instance(), show_net(...,
        add=False) or design_changed()). See net_pin() for what `target`
        can be: one bit, resolved here, so a bad target raises ValueError
        rather than reaching the viewer. `label` names the trace (default:
        the pin, e.g. "u1/A[0] (net)"); `color` ("#rrggbb" or (r, g, b),
        see trace_color()) replaces its palette color. With add=False the
        schematic is cleared first. A net the view already shows as a
        host net is left as it is. With trace=True the pin's whole fan-in
        cone is drawn, as the tree's Trace to Driver does."""
        pin = net_pin(target)
        if label is None:
            label = pin["label"].removesuffix(" (net)") if trace else pin["label"]
        message = protocol.show_net(
            pin["id_path"], pin["term_id"], pin["bit"], path=pin["path"],
            label=str(label), color=None if color is None else trace_color(color), add=add,
            trace=trace)
        with self._state_lock:
            if add:
                self._net_pushes.append(message)
            else:
                # The view is cleared: the focus goes with it.
                self._focus_push, self._net_pushes = None, [message]
            generation = self._generation
        self._send(self._stamp(message, generation))

    def export_svg(self, callback=None):
        """Ask the viewer(s) for the schematic as SVG (the whole sheet, as
        File > Export Schematic as SVG... saves it). The answer comes
        asynchronously, once a viewer's schematic shows something: it is
        kept in `svg`, and `callback(svg)`, if given, is called once,
        through `dispatch`."""
        with self._state_lock:
            if callback is not None:
                self._svg_callbacks.append(callback)
            generation = self._generation
        self._send(self._stamp(protocol.export_svg(), generation))

    def design_changed(self, diagnosis=None, instance=None):
        """Start the next design generation: the viewer(s) reload the design
        from the root, and whatever they asked about or selected in the
        previous one is dropped (see the class docstring). Call it right
        after replacing or editing the design, before releasing the lock (or
        leaving the `run` thread) the swap was made under.

        The diagnoses and focused instance kept so far name the old design,
        so they are replaced by `diagnosis` and `instance` (resolved against
        the new design; None: none), sent after the new root loads. The
        selection is cleared: selection callbacks get (None, None).
        Returns the new generation."""
        diagnosis_push = (protocol.diagnosis_response(diagnosis)
                          if diagnosis is not None else None)
        focus_push = None
        if instance is not None:
            names, ids = instance_paths(instance)
            focus_push = protocol.focus_instance(names, ids)
        if self.review is not None:
            self.review.design_changed()
        with self._state_lock:
            self._generation += 1
            generation = self._generation
            self._source_paths = dataclasses.replace(self._source_paths, loaded_at=time.time())
            self._diagnosis_push, self._focus_push = diagnosis_push, focus_push
            self._net_pushes = []  # they name the old design
            had_selection = self.selected_id_path is not None or self.selected_path is not None
            self.selected_path = self.selected_id_path = None
        self._send(self._stamp(protocol.design_changed(), generation))
        if had_selection:
            self._dispatch(lambda: self._notify(generation, None, None))
        return generation

    @property
    def selected(self):
        """The viewer's selected instance as a naja SNLPath, or None (see
        selected_instance())."""
        with self._state_lock:
            ids, path = self.selected_id_path, self.selected_path
        return selected_instance(ids, path)

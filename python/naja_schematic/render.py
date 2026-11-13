"""SVG export with no browser and no display: export_svg(), export_svgs()
and the `naja-schematic render` command.

The drawing is made by naja-schematic-render, the viewer built with no
window (no SDL, GL or naja; shipped in the platform wheels in bin/). It is
driven over its stdin/stdout like a browser viewer over its WebSocket: its
requests are answered here, by a session.ViewerSession on the design in the
current NLUniverse -- the same answers the notebook widget gets -- so the
SVG is what File > Export Schematic as SVG... saves from a viewer showing
the same things, and designs whose primitives were built in Python draw
with their full modeling (gate symbols, clock pins, traces crossing them).

What to draw is pushed as the host pushes it to a viewer: focus_instance
(an instance alone, every pin open; with "add" for the next ones), show_net
(a net as a colored one-net trace, or with "trace" its fan-in cone), then
export_svg. Targets are resolved here first, so one that doesn't resolve
raises ValueError and nothing is drawn.

The universe is read on the calling thread, while export_svg() runs: in an
application serving it to viewers too, call it under that server's lock
(ViewerServer.lock).
"""
import argparse
import collections
import json
import logging
import os
import subprocess
import sys
import threading
import types
from pathlib import Path

from najaeda import naja

from . import protocol, session

log = logging.getLogger("naja_schematic")

BIN_DIR = Path(__file__).parent / "bin"
RENDERER_NAME = "naja-schematic-render"
# Seconds one drawing may take before the renderer is stopped.
DEFAULT_TIMEOUT = 120.0


class RenderError(RuntimeError):
    """The renderer is missing, failed, or had nothing to draw."""


def renderer_path():
    """Path of the naja-schematic-render executable: $NAJA_SCHEMATIC_RENDER
    (a locally built one, for a development checkout), else the one in this
    package's bin/ (platform wheels). Raises RenderError when there is none."""
    override = os.environ.get("NAJA_SCHEMATIC_RENDER")
    path = Path(override) if override else BIN_DIR / RENDERER_NAME
    if path.is_file() and not override and not os.access(path, os.X_OK):
        # An install that dropped the executable bit (some unpackers do).
        try:
            path.chmod(path.stat().st_mode | 0o111)
        except OSError:
            pass
    if path.is_file() and os.access(path, os.X_OK):
        return path
    if override:
        raise RenderError(f"NAJA_SCHEMATIC_RENDER={override}: not an executable file")
    raise RenderError(
        "this naja-schematic install has no SVG renderer (bin/naja-schematic-render): "
        "it ships in the platform wheels (Linux x86_64, macOS); on other platforms build "
        "it from a source checkout (cmake -DNAJA_SCHEMATIC_RENDER_ONLY=ON) and set "
        "NAJA_SCHEMATIC_RENDER to its path")


# -- what to draw -------------------------------------------------------------

def _is_net_target(target):
    # What session.net_pin() takes, as opposed to an instance
    # (session.instance_paths()). A tuple is (instance, terminal[, bit]).
    if isinstance(target, tuple):
        return True
    if hasattr(target, "termID") or hasattr(target, "equi"):  # najaeda Term, Equipotential
        return True
    if hasattr(target, "pathIDs") and (hasattr(target, "net") or hasattr(target, "net_concat")):
        return True  # najaeda Net
    return isinstance(target, (naja.SNLOccurrence, naja.SNLInstTerm, naja.SNLBitTerm,
                               naja.SNLBusTerm, naja.SNLEquipotential))


def _as_list(targets):
    # One target or a list of them. A tuple is one pin, (instance,
    # terminal[, bit]); a list (or set, or generator) holds targets.
    if targets is None or targets == ():
        return []
    if isinstance(targets, (list, set, frozenset, types.GeneratorType)):
        return list(targets)
    return [targets]


def drawing_pushes(targets=None, *, nets=None, instances=None, traces=None):
    """The pushes drawing these targets on a fresh view, ending with
    export_svg: the instances first (the first one alone, the next ones
    added), then the nets, then the traces. Raises ValueError (TypeError
    for an unsupported kind) for one that doesn't resolve, RenderError
    when there is nothing to draw. See export_svg() for the targets."""
    inst_targets, net_targets = [], []
    for target in _as_list(targets):
        (net_targets if _is_net_target(target) else inst_targets).append(target)
    inst_targets += _as_list(instances)
    net_targets += _as_list(nets)
    trace_targets = _as_list(traces)
    if not (inst_targets or net_targets or trace_targets):
        raise RenderError("nothing to draw: give instances, nets or traces")

    pushes = [protocol.design_changed()]  # a clean view: no tree, schematic or selection
    for i, target in enumerate(inst_targets):
        names, ids = session.instance_paths(target)
        if not ids:
            raise ValueError("the top design has no box to draw: draw nets or traces "
                             "on its ports instead")
        pushes.append(protocol.focus_instance(names, ids, add=i > 0))
    for trace, pin_targets in ((False, net_targets), (True, trace_targets)):
        for target in pin_targets:
            pin = session.net_pin(target)
            label = pin["label"].removesuffix(" (net)") if trace else pin["label"]
            pushes.append(protocol.show_net(pin["id_path"], pin["term_id"], pin["bit"],
                                            path=pin["path"], label=label, trace=trace))
    pushes.append(protocol.export_svg())
    return pushes


# -- the renderer process -----------------------------------------------------

class Renderer:
    """One naja-schematic-render process, drawing one SVG per render()
    call on the design in the current NLUniverse. A context manager;
    close() ends the process."""

    def __init__(self, source_roots=(), source_maps=()):
        self._session = session.ViewerSession(source_roots=source_roots,
                                              source_maps=source_maps)
        self._proc = subprocess.Popen(
            [str(renderer_path())], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, encoding="utf-8", bufsize=1)
        # True while the renderer waits for the answer to its render_idle:
        # the next drawing's pushes.
        self._idle = False
        self._timeout_hit = False
        self._stderr = collections.deque(maxlen=40)
        self._stderr_thread = threading.Thread(target=self._drain_stderr, daemon=True,
                                               name="naja-schematic-render-stderr")
        self._stderr_thread.start()

    def _drain_stderr(self):
        for line in self._proc.stderr:
            self._stderr.append(line.rstrip("\n"))
            log.debug("renderer: %s", line.rstrip("\n"))

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def close(self):
        if self._proc.poll() is None:
            try:
                self._proc.stdin.close()  # the renderer exits at end of input
            except OSError:
                pass
            try:
                self._proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self._proc.kill()
                self._proc.wait()
        self._stderr_thread.join(timeout=5)

    def _failure(self, what):
        self.close()
        tail = "\n".join(self._stderr)
        return RenderError(f"naja-schematic-render {what} (exit code {self._proc.returncode})"
                           + (f":\n{tail}" if tail else ""))

    def _write(self, messages):
        try:
            self._proc.stdin.write(json.dumps(messages) + "\n")
            self._proc.stdin.flush()
        except (BrokenPipeError, OSError):
            raise self._failure("stopped") from None

    def render(self, pushes, timeout=DEFAULT_TIMEOUT):
        """Draw on a fresh view what `pushes` (drawing_pushes()) ask for;
        returns the SVG. Raises RenderError when nothing got drawn, or when
        it takes more than `timeout` seconds (None: no limit; the renderer
        is then stopped)."""
        timer = None
        if timeout is not None:
            timer = threading.Timer(timeout, self._timed_out)
            timer.daemon = True
            timer.start()
        try:
            return self._render(pushes)
        except RenderError:
            if self._timeout_hit:
                raise RenderError(f"naja-schematic-render took more than {timeout} s: "
                                  "stopped") from None
            raise
        finally:
            if timer is not None:
                timer.cancel()

    def _timed_out(self):
        self._timeout_hit = True
        self._proc.kill()

    def _render(self, pushes):
        pending, svg, errors = list(pushes), None, []
        if self._idle:
            self._idle = False
            self._write(pending)
            pending = None
        while True:
            line = self._proc.stdout.readline()
            if not line:
                raise self._failure("stopped")
            message = json.loads(line)
            kind = message.get("request")
            if kind == "render_idle":
                if pending is None:
                    self._idle = True  # drawn; waits for the next render()
                    break
                self._write(pending)
                pending = None
                continue
            if kind == "svg_exported":
                svg = message.get("svg") or ""
                self._write([])
                continue
            try:
                replies = [json.loads(r) for r in self._session.answer(line)]
            except Exception as e:  # answered, so the renderer doesn't wait forever
                log.exception("Failed to answer the renderer's %s", kind)
                errors.append(f"{kind}: {e}")
                replies = [{"response": "error", "request": kind, "message": str(e)}]
            self._write(replies)
        if errors:
            raise RenderError("; ".join(errors))
        if not svg:
            raise RenderError("nothing was drawn")
        return svg


def _write_svg(path, svg):
    Path(path).write_text(svg, encoding="utf-8")


def export_svg(targets=None, path=None, *, nets=None, instances=None, traces=None,
               timeout=DEFAULT_TIMEOUT, source_roots=(), source_maps=()):
    """Draw part of the design in the current NLUniverse as an SVG
    schematic, with no browser or display; returns the SVG, also written to
    `path` when given.

    `targets` is one target or a list of them, each sorted by kind: an
    instance (a najaeda Instance, a naja SNLPath, a list of instance ids or
    of names -- a path, top excluded -- or a str, one name, a child of the
    top) or a pin (anything show_net() takes: a najaeda Term, Net or
    Equipotential, a naja SNLOccurrence, SNLInstTerm, top-level term or
    SNLEquipotential, or a tuple (instance, terminal[, bit])). So
    ["u1", "u2"] is two instances, [["u1", "u2"]] one (u2 inside u1).
    `instances`, `nets` and `traces` name targets of one kind each:

    - an instance is drawn as a box with every pin open, as the viewer's
      show_instance() draws it; the first one selected (highlighted);
    - a net (one bit) is drawn as a colored one-net trace, as the tree's
      Show Equipotential;
    - a trace is the pin's combinational fan-in cone, back to flops, ports
      and blackboxes, as the tree's Trace to Driver.

    Raises ValueError (TypeError for an unsupported kind) when a target
    doesn't resolve -- nothing is drawn then -- and RenderError when there
    is no renderer, nothing to draw, or the drawing takes more than
    `timeout` seconds. `source_roots`/`source_maps` are as for
    ViewerSession (they don't change the drawing)."""
    pushes = drawing_pushes(targets, nets=nets, instances=instances, traces=traces)
    with Renderer(source_roots, source_maps) as renderer:
        svg = renderer.render(pushes, timeout)
    if path is not None:
        _write_svg(path, svg)
    return svg


def export_svgs(jobs, *, timeout=DEFAULT_TIMEOUT, source_roots=(), source_maps=()):
    """Several export_svg() drawings with one renderer process; returns
    their SVGs, in order. Each job is a dict of export_svg()'s arguments:
    "targets", "path", "nets", "instances", "traces". Every job is
    resolved before anything is drawn, and files are written only once
    all are drawn: a bad target raises without writing any. `timeout`
    applies to each drawing."""
    keys = {"targets", "path", "nets", "instances", "traces"}
    plans = []
    for i, job in enumerate(jobs):
        if not isinstance(job, dict):
            raise TypeError(f"job {i}: expected a dict of export_svg() arguments, "
                            f"got {type(job).__name__}")
        unknown = set(job) - keys
        if unknown:
            raise TypeError(f"job {i}: unknown keys {sorted(unknown)}")
        try:
            pushes = drawing_pushes(job.get("targets"), nets=job.get("nets"),
                                    instances=job.get("instances"),
                                    traces=job.get("traces"))
        except (ValueError, TypeError, RenderError) as e:
            raise type(e)(f"job {i}: {e}") from e
        plans.append((job.get("path"), pushes))
    if not plans:
        return []
    svgs = []
    with Renderer(source_roots, source_maps) as renderer:
        for i, (_, pushes) in enumerate(plans):
            try:
                svgs.append(renderer.render(pushes, timeout))
            except RenderError as e:
                raise RenderError(f"job {i}: {e}") from e
    for (path, _), svg in zip(plans, svgs):
        if path is not None:
            _write_svg(path, svg)
    return svgs


# -- `naja-schematic render` --------------------------------------------------
#
# Instance paths are lists everywhere in naja-schematic; on a command line
# they're also accepted as text, "u1/u2", matched against the design rather
# than split: "/" ends a name only where an instance of that name exists, so
# an escaped name containing "/" (or ":", "[") still resolves -- and an
# ambiguous one is refused, asking for the JSON form.

def _instances_matching(text):
    """Every instance-id path whose names joined with "/" spell `text`."""
    universe = naja.NLUniverse.get()
    top = universe.getTopDesign() if universe else None
    if top is None:
        raise ValueError("no design is loaded")
    if text == "":
        return [[]]
    found = []

    def walk(design, rest, ids):
        for end in [i for i, c in enumerate(rest) if c == "/"] + [len(rest)]:
            name = rest[:end]
            inst = design.getInstance(name) if name else None
            if inst is None:
                continue
            if end == len(rest):
                found.append(ids + [inst.getID()])
            else:
                walk(inst.getModel(), rest[end + 1:], ids + [inst.getID()])

    walk(top, text, [])
    return found


def _json_arg(text):
    stripped = text.lstrip()
    if stripped[:1] in ("[", "{"):
        try:
            return json.loads(stripped)
        except json.JSONDecodeError as e:
            raise ValueError(f"not valid JSON: {e}") from None
    return None


def parse_instance_arg(text):
    """--instance: a path as text ("u1/u2", matched against the design), or
    JSON: a list of ids ([3, 7]) or names (["u1", "u2"]), or an object with
    "id_path"/"path". Returns the instance id path."""
    value = _json_arg(text)
    if value is not None:
        if isinstance(value, dict):
            value = value.get("id_path", value.get("path"))
        if not isinstance(value, list):
            raise ValueError("expected a JSON list of instance ids or names, or an object "
                             "with \"id_path\" or \"path\"")
        return session.instance_paths(value)[1]
    matches = _instances_matching(text)
    if not matches:
        raise ValueError(f"no instance {text} in the design")
    if len(matches) > 1:
        raise ValueError(f"{text} names several instances ({matches}): give the JSON id "
                         "path, e.g. --instance '[3,7]'")
    return matches[0]


def parse_pin_arg(text):
    """--net/--trace: INST:TERM, INST:TERM[BIT], or TERM / TERM[BIT] for a
    top-level port (INST matched as for --instance); or JSON, {"id_path"
    or "path": the instance ([] or absent = a top-level port), "terminal":
    "A", "bit": 0}. Returns (instance id path, term id, bit or None), a
    pin session.net_pin() takes."""
    value = _json_arg(text)
    if value is not None:
        if not isinstance(value, dict) or not isinstance(value.get("terminal"), str):
            raise ValueError("expected a JSON object like "
                             "{\"id_path\": [3, 7], \"terminal\": \"A\", \"bit\": 0}")
        path = value.get("id_path", value.get("path", []))
        ids = session.instance_paths(path)[1] if path else []
        return _pin_on(ids, value["terminal"], value.get("bit"), text)

    pins = set()
    splits = [i for i, c in enumerate(text) if c == ":"]
    for i in [-1] + splits:
        inst_text, term_text = text[:max(i, 0)], text[i + 1:]
        for ids in _instances_matching(inst_text):
            for name, bit in _term_spellings(term_text):
                try:
                    pins.add(_pin_on(ids, name, bit, text))
                except ValueError:
                    pass
    if not pins:
        raise ValueError(f"no pin {text} in the design (INST:TERM, INST:TERM[BIT], or TERM "
                         "for a top-level port; a bus needs a bit)")
    if len(pins) > 1:
        raise ValueError(f"{text} names several pins: give it as JSON, e.g. "
                         "--net '{\"id_path\": [3, 7], \"terminal\": \"A\", \"bit\": 0}'")
    return pins.pop()


def _term_spellings(text):
    # The terminal as written, else TERM[BIT] read as bit BIT of bus TERM.
    yield text, None
    if text.endswith("]") and "[" in text:
        base, _, bit = text[:-1].rpartition("[")
        if base and bit.isdigit():
            yield base, int(bit)


def _pin_on(ids, terminal, bit, text):
    design = session._model_at(ids)
    term = design.getTerm(terminal)
    if term is None:
        raise ValueError(f"{text}: no terminal {terminal!r} on {design.getName()}")
    if isinstance(term, naja.SNLBusTerm):
        if bit is None or term.getBusTermBit(bit) is None:
            raise ValueError(f"{text}: {terminal} is a bus "
                             f"[{term.getMSB()}:{term.getLSB()}]: give one bit")
    elif bit is not None:
        raise ValueError(f"{text}: {terminal} is not a bus")
    return (tuple(ids), term.getID(), bit)


def build_arg_parser():
    from .server import add_design_arguments

    parser = argparse.ArgumentParser(
        prog="naja-schematic render",
        description="Draw part of a netlist as an SVG schematic, with no browser or "
                    "display. Exits non-zero, writing nothing, if a target doesn't "
                    "resolve.")
    parser.add_argument("--svg", required=True, metavar="FILE", help="SVG file to write")
    parser.add_argument("--instance", action="append", default=[], metavar="PATH",
                        dest="instances",
                        help="Instance drawn as a box with every pin open (repeatable): "
                             "u1/u2, or JSON ids [3,7] / names [\"u1\",\"u2\"]")
    parser.add_argument("--net", action="append", default=[], metavar="INST:TERM[BIT]",
                        dest="nets",
                        help="Net on a pin, drawn as a colored trace (repeatable): "
                             "u1/u2:A, u1:D[3], or y for a top-level port; or JSON "
                             "{\"id_path\":[3,7],\"terminal\":\"A\",\"bit\":0}")
    parser.add_argument("--trace", action="append", default=[], metavar="INST:TERM[BIT]",
                        dest="traces",
                        help="Fan-in cone of a pin, back to flops, ports and blackboxes "
                             "(repeatable); same forms as --net")
    parser.add_argument("--verbose", "-v", action="store_true", help="Log to stderr")
    add_design_arguments(parser)
    return parser


def main(argv=None):
    from .server import check_design_arguments, load_design

    parser = build_arg_parser()
    args = parser.parse_args(argv)
    check_design_arguments(parser, args)
    if not (args.instances or args.nets or args.traces):
        parser.error("nothing to draw: give --instance, --net and/or --trace")
    logging.basicConfig(stream=sys.stderr, format="%(message)s",
                        level=logging.DEBUG if args.verbose else logging.INFO)
    try:
        protocol.check_najaeda_version()
        renderer_path()
    except (RuntimeError, RenderError) as e:
        raise SystemExit(str(e))

    if load_design(args) is None:
        raise SystemExit("the design did not load")

    errors, instances, nets, traces = [], [], [], []
    for flag, values, parse, out in (("--instance", args.instances, parse_instance_arg, instances),
                                     ("--net", args.nets, parse_pin_arg, nets),
                                     ("--trace", args.traces, parse_pin_arg, traces)):
        for text in values:
            try:
                out.append(parse(text))
            except (ValueError, TypeError) as e:
                errors.append(f"{flag} {text}: {e}")
    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        raise SystemExit(1)

    try:
        export_svg(None, args.svg, nets=nets, instances=instances, traces=traces)
    except (ValueError, TypeError, RenderError) as e:
        raise SystemExit(f"render: {e}")
    log.info("Schematic written to %s", args.svg)
    return 0

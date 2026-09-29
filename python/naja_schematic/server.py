"""Transports for the naja-schematic protocol, plus the `naja-schematic` CLI.

- WebSocket (`serve()`): what the browser build connects to. The same port
  also serves the viewer page itself (index.html + the bundled wasm module),
  so `naja-schematic design.v --open` is a one-command launch.
- stdio (`serve_stdio()`): one JSON message per line on stdin/stdout, for a
  host that spawns the server as a child process (e.g. an editor extension)
  and relays messages itself.

Both answer requests through protocol.handle_request(), against whatever
design is loaded in this process's NLUniverse.
"""
import argparse
import asyncio
import http
import json
import logging
import os
import sys
import webbrowser
from glob import glob

from . import protocol
from ._bundle import static_file

log = logging.getLogger("naja_schematic")

DEFAULT_PORT = 8081

_CONTENT_TYPES = {
    ".html": "text/html; charset=utf-8",
    ".js": "text/javascript; charset=utf-8",
}


def _diagnosis_push(diagnosis):
    return json.dumps(protocol.diagnosis_response(diagnosis)) if diagnosis else None


def _answer(message, diagnosis_push):
    # Responses to one request, plus the diagnosis push right after the root
    # is (re)loaded: the viewer clears its DiagnosisStore on every
    # root_response, so the annotations have to follow it.
    replies = protocol.handle_message(message)
    if diagnosis_push and any('"root_response"' in r for r in replies):
        replies.append(diagnosis_push)
    return replies


# ---------------------------------------------------------------------------
# WebSocket (+ static viewer page) server
# ---------------------------------------------------------------------------

def _static_response(connection, request):
    # Any non-upgrade HTTP request is a request for the viewer page.
    from websockets.datastructures import Headers
    from websockets.http11 import Response

    if request.headers.get("Upgrade", "").lower() == "websocket":
        return None  # continue with the WebSocket handshake

    name = request.path.split("?", 1)[0].lstrip("/") or "index.html"
    path = static_file(name)
    if path is None:
        hint = ""
        if name == "naja-schematic.js":
            hint = (" -- the viewer bundle is not built into this install; "
                    "set NAJA_SCHEMATIC_BUNDLE to a naja-schematic.js built "
                    "with -DNAJA_SCHEMATIC_WASM_MODULE=ON")
        return connection.respond(http.HTTPStatus.NOT_FOUND, f"Not found: {name}{hint}\n")
    body = path.read_bytes()
    headers = Headers([
        ("Content-Type", _CONTENT_TYPES.get(path.suffix, "application/octet-stream")),
        ("Content-Length", str(len(body))),
        ("Cache-Control", "no-cache"),
    ])
    return Response(http.HTTPStatus.OK, http.HTTPStatus.OK.phrase, headers, body)


async def serve(host="localhost", port=DEFAULT_PORT, diagnosis=None, open_browser=False):
    """Serve the viewer page and its WebSocket on host:port, forever."""
    from websockets.asyncio.server import serve as ws_serve
    from websockets.exceptions import ConnectionClosed

    diagnosis_push = _diagnosis_push(diagnosis)

    async def handle_connection(websocket):
        log.info("Client connected")
        try:
            async for message in websocket:
                log.debug("Received: %s", message)
                for reply in _answer(message, diagnosis_push):
                    await websocket.send(reply)
        except ConnectionClosed as e:
            log.info("Client disconnected: %s", e)

    async with ws_serve(handle_connection, host, port, process_request=_static_response,
                        max_size=None):
        url = f"http://{host}:{port}/"
        log.info("Serving naja-schematic on %s (WebSocket ws://%s:%d/ws)", url, host, port)
        if open_browser:
            webbrowser.open(url)
        await asyncio.Future()  # run forever


# ---------------------------------------------------------------------------
# stdio server
# ---------------------------------------------------------------------------

def serve_stdio(diagnosis=None, stdin=None, stdout=None):
    """Answer one JSON request per stdin line with JSON lines on stdout,
    until stdin closes. Logging must not go to stdout in this mode."""
    stdin = stdin or sys.stdin
    stdout = stdout or sys.stdout
    diagnosis_push = _diagnosis_push(diagnosis)
    for line in stdin:
        line = line.strip()
        if not line:
            continue
        try:
            replies = _answer(line, diagnosis_push)
        except json.JSONDecodeError as e:
            log.error("Ignoring malformed request %r: %s", line, e)
            continue
        for reply in replies:
            stdout.write(reply + "\n")
        stdout.flush()


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def build_arg_parser():
    parser = argparse.ArgumentParser(
        prog="naja-schematic",
        description="Serve a netlist to the naja-schematic viewer (browser page "
                    "+ WebSocket, or stdio).")
    parser.add_argument("--host", default="localhost",
                        help="Interface to listen on (default: localhost)")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT,
                        help=f"Port to serve on (default: {DEFAULT_PORT})")
    parser.add_argument("--open", action="store_true",
                        help="Open the viewer in a web browser once the server is up")
    parser.add_argument("--stdio", action="store_true",
                        help="Speak the protocol as JSON lines on stdin/stdout instead "
                             "of serving WebSocket/HTTP")
    parser.add_argument("--diagnosis", metavar="FILE",
                        help="Diagnosis JSON ({\"items\": [...]} or a bare array) to "
                             "push to the viewer after the design loads")
    parser.add_argument("--verbose", "-v", action="store_true",
                        help="Log every request (to stderr)")
    parser.add_argument("--xilinx", action="store_true",
                        help="Load Xilinx primitives")
    parser.add_argument("--allow_unknown_designs", action="store_true",
                        help="Allow unknown designs when loading the design.")
    parser.add_argument("--liberty", nargs="*", help="List of liberty files to load")
    parser.add_argument("--verilog", type=str,
                        help="Verilog netlist to load")
    parser.add_argument("--systemverilog", "--sv", nargs="+", metavar="FILE",
                        help="SystemVerilog file(s) to load (elaborated with slang)")
    parser.add_argument("--flist", "-f", type=str, metavar="FILE",
                        help="SystemVerilog command file (slang -f syntax: sources, "
                             "+incdir+, +define+, ...); may be combined with --systemverilog")
    parser.add_argument("--top", type=str,
                        help="SystemVerilog only: top module to elaborate")
    parser.add_argument("--define", "-D", action="append", metavar="NAME[=VALUE]",
                        help="SystemVerilog only: preprocessor define (repeatable)")
    return parser


def load_design(args):
    """Load the liberty files and the Verilog/SystemVerilog design named by
    the parsed CLI arguments into the NLUniverse; returns the top."""
    from najaeda import netlist

    is_sv = bool(args.systemverilog or args.flist)
    if args.xilinx:
        log.info("Loading Xilinx primitives")
        netlist.load_primitives('xilinx')

    if args.liberty:
        # Expand wildcards ourselves, for shells that pass them through.
        expanded_liberty_files = []
        for lib in args.liberty:
            if '*' in lib:
                expanded_liberty_files.extend(glob(lib))
            else:
                expanded_liberty_files.append(lib)
        for lib in expanded_liberty_files:
            log.info("Loading liberty file: %s", lib)
            netlist.load_liberty(lib)

    if is_sv:
        sv_files = args.systemverilog or []
        sources = sv_files + ([f"-f {args.flist}"] if args.flist else [])
        log.info("Loading SystemVerilog: %s", ", ".join(sources))
        config = netlist.SystemVerilogConfig()
        config.flist = args.flist
        config.top = args.top
        config.defines = args.define
        config.blackbox_unknown_modules = args.allow_unknown_designs
        top = netlist.load_system_verilog(sv_files, config=config)
    else:
        log.info("Loading Verilog netlist: %s", args.verilog)
        config = netlist.VerilogConfig()
        config.allow_unknown_designs = args.allow_unknown_designs
        top = netlist.load_verilog(args.verilog, config=config)
    log.info("Design loaded: %s", top.get_name())
    return top


def load_diagnosis_file(path):
    with open(path) as f:
        return json.load(f)


def main(argv=None):
    parser = build_arg_parser()
    args = parser.parse_args(argv)

    is_sv = bool(args.systemverilog or args.flist)
    if not args.verilog and not is_sv:
        parser.error("provide a design: --verilog, or --systemverilog and/or --flist")
    if args.verilog and is_sv:
        parser.error("--verilog cannot be combined with --systemverilog/--flist")
    if not is_sv and (args.top or args.define):
        parser.error("--top/--define only apply to --systemverilog/--flist")
    if is_sv and args.liberty:
        # Same restriction as the native standalone: the SystemVerilog loader
        # has no liberty hook, so fail loudly rather than silently ignore it.
        parser.error("--liberty is not supported with --systemverilog/--flist")
    if args.stdio and args.open:
        parser.error("--open does not apply to --stdio")

    # stderr only: stdout is the protocol channel in --stdio mode.
    logging.basicConfig(stream=sys.stderr, format="%(message)s",
                        level=logging.DEBUG if args.verbose else logging.INFO)
    if not args.verbose:
        logging.getLogger("websockets").setLevel(logging.WARNING)

    try:
        protocol.check_najaeda_version()
    except RuntimeError as e:
        raise SystemExit(str(e))

    protocol_out = None
    if args.stdio:
        # naja's C++ logger writes to fd 1: keep a private copy of the real
        # stdout for protocol messages and point fd 1 at stderr, so nothing
        # else can land on the protocol channel.
        sys.stdout.flush()
        protocol_out = os.fdopen(os.dup(1), "w")
        os.dup2(2, 1)

    diagnosis = load_diagnosis_file(args.diagnosis) if args.diagnosis else None
    load_design(args)

    if args.stdio:
        serve_stdio(diagnosis, stdout=protocol_out)
        return
    try:
        asyncio.run(serve(args.host, args.port, diagnosis, args.open))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()

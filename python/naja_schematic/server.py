"""Transports for the naja-schematic protocol, plus the `naja-schematic` CLI.

- WebSocket (`serve()`): what the browser build connects to. The same port
  also serves the viewer page itself (index.html + the bundled wasm module),
  so `naja-schematic design.v --open` is a one-command launch.
- stdio (`serve_stdio()`): one JSON message per line on stdin/stdout, for a
  host that spawns the server as a child process (e.g. an editor extension)
  and relays messages itself.
- `ViewerServer`: the WebSocket server as an object a host application
  embeds -- started and stopped on demand, on a background thread, with
  the host's lock around the design, pushes and selection callbacks.

All answer requests through a session.ViewerSession, against whatever
design is loaded in this process's NLUniverse.
"""
import argparse
import asyncio
import contextlib
import hmac
import http
import json
import logging
import os
import secrets
import socket
import sys
import threading
import urllib.parse
import webbrowser
from concurrent.futures import ThreadPoolExecutor
from glob import glob

from . import protocol
from ._bundle import static_file
from .session import ViewerSession, lock_runner

log = logging.getLogger("naja_schematic")

DEFAULT_PORT = 8081

_CONTENT_TYPES = {
    ".html": "text/html; charset=utf-8",
    ".js": "text/javascript; charset=utf-8",
}


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

    session = ViewerSession(diagnosis=diagnosis)

    async def handle_connection(websocket):
        log.info("Client connected")
        try:
            async for message in websocket:
                log.debug("Received: %s", message)
                for reply in session.answer(message):
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
# Embeddable server, for a host that already owns the design
# ---------------------------------------------------------------------------

_WILDCARD_HOSTS = {"", "0.0.0.0", "::"}


def _url_host(host):
    if host in _WILDCARD_HOSTS:
        return "127.0.0.1"
    return f"[{host}]" if ":" in host else host


class ViewerServer:
    """The viewer page and its WebSocket, served from a background thread
    for a host application that already holds the design in NLUniverse.

    Nothing is loaded: requests are answered from whatever design the
    universe holds when they arrive. Typical use::

        server = ViewerServer(lock=design_lock)   # port 0: any free port
        server.start()
        webbrowser.open(server.url)               # or server.open_browser()
        server.annotate(items); server.show_instance(instance)
        server.on_select(lambda id_path, path: ...)
        with server.replacing_design():            # viewers reload the new design,
            netlist.load_verilog(other)            # stale requests are dropped
        server.stop()

    It is also a context manager (start on enter, stop on exit).

    Serializing with the host's design operations -- give at most one of:
    - `lock`: a lock (any context manager) held around every universe
      access the viewer starts; the host holds the same lock while it
      edits the design. Must be reentrant (e.g. threading.RLock) if the
      host calls this server's methods while holding it. Without `lock` or
      `run`, the server makes its own RLock, available as `server.lock`.
    - `run`: `run(fn) -> fn()`, called for every universe access the viewer
      starts, e.g. to hand `fn` to the host's own thread and wait for it.
    Viewer requests are answered one at a time, on a worker thread, and
    `run`/`lock` see only those. The host's own calls (annotate(),
    show_instance(), selected) touch the universe directly on the calling
    thread: make them where the host's design operations are allowed.

    Security: the WebSocket only accepts connections that present
    `token` (a random one by default, part of `url`) and, for browser
    connections, come from this server's own page or one of
    `allowed_origins` -- so another web page open in the same browser
    can't read the design. Pass token=None to turn the token off.
    """

    def __init__(self, host="127.0.0.1", port=0, *, lock=None, run=None,
                 token=True, allowed_origins=(), diagnosis=None):
        if lock is not None and run is not None:
            raise ValueError("pass lock or run, not both")
        if run is None:
            lock = lock if lock is not None else threading.RLock()
            run = lock_runner(lock)
        self.lock = lock
        self._host = host
        self._port = port
        self._token = secrets.token_urlsafe(16) if token is True else (token or None)
        self._allowed_origins = list(allowed_origins)
        # Selection callbacks run in order on one thread of their own: the
        # viewer's selections come from the request worker, design_changed()'s
        # (None, None) from the host, and neither may overtake the other.
        self._callbacks = ThreadPoolExecutor(max_workers=1,
                                             thread_name_prefix="naja-schematic-callbacks")
        self._session = ViewerSession(push=self._broadcast, run=run, diagnosis=diagnosis,
                                      dispatch=self._dispatch_callback)
        self._thread = None
        self._loop = None
        self._stopping = None
        self._executor = None
        self._start_error = None
        self._clients = set()
        self._pending = set()  # requests handed to the executor

    # -- lifecycle -----------------------------------------------------------

    def start(self):
        """Start serving; returns once the port is bound (so `url` and
        `port` are final). Returns self."""
        if self._thread is not None:
            raise RuntimeError("ViewerServer already started")
        ready = threading.Event()
        self._thread = threading.Thread(target=self._thread_main, args=(ready,),
                                        name="naja-schematic-server", daemon=True)
        self._thread.start()
        ready.wait()
        if self._start_error is not None:
            self._thread.join()
            self._thread = None
            raise self._start_error
        log.info("Serving naja-schematic on %s", self.url)
        return self

    def stop(self, timeout=5.0):
        """Close every viewer connection and the listening socket. Doesn't
        wait for a request stuck in `run` (it is abandoned). Safe to call
        more than once."""
        thread, loop = self._thread, self._loop
        if thread is None:
            return
        if loop is not None:
            try:
                loop.call_soon_threadsafe(self._stopping.set)
            except RuntimeError:  # loop already closed
                pass
        thread.join(timeout)
        self._thread = None

    def __enter__(self):
        return self.start()

    def __exit__(self, *exc):
        self.stop()

    @property
    def running(self):
        return self._thread is not None and self._thread.is_alive()

    @property
    def host(self):
        return self._host

    @property
    def port(self):
        """The bound port (the one picked by the OS for port 0, once
        started)."""
        return self._port

    @property
    def token(self):
        return self._token

    @property
    def url(self):
        """The viewer page's URL, token included."""
        query = f"?token={self._token}" if self._token else ""
        return f"http://{_url_host(self._host)}:{self._port}/{query}"

    def open_browser(self):
        webbrowser.open(self.url)

    # -- host API ------------------------------------------------------------

    def annotate(self, items):
        """Overlay diagnosis items on every connected viewer, and on any
        that (re)loads later (see naja_schematic.diagnosis_response() for
        the item shape; an item's path may be a najaeda Instance). Pass []
        to clear."""
        self._session.annotate(items)

    def show_instance(self, target):
        """Reveal, select and draw one instance in every viewer, and in any
        that (re)loads later. `target`: a najaeda netlist.Instance, a list
        of instance ids or of instance names (top excluded; [] = top), or
        one name for a child of the top."""
        self._session.show_instance(target)

    @property
    def generation(self):
        """The current design generation: 0, plus one per design_changed()."""
        return self._session.generation

    def design_changed(self, diagnosis=None, instance=None):
        """Start the next design generation, after replacing or editing the
        design: every viewer reloads it from the root, and every request a
        viewer made for the previous design -- a query or a selection, even
        one already queued behind the host's lock, even naming ids the new
        design reuses -- is dropped unanswered. Kept diagnoses and focus are
        replaced by `diagnosis` and `instance` (resolved against the new
        design; None: none), and the selection is cleared. Returns the new
        generation.

        Call it in the same critical section as the swap: before releasing
        `lock` (see replacing_design()), or, with `run=`, on the thread
        `run` hands requests to, before it runs another one. Otherwise a
        queued request could still be answered for the old generation
        against the new design in between."""
        return self._session.design_changed(diagnosis, instance)

    @contextlib.contextmanager
    def replacing_design(self, diagnosis=None, instance=None):
        """Replace or edit the design inside the block: `lock` is held for
        it, and design_changed(diagnosis, instance) runs at its end, still
        under the lock (not if the block raises)::

            with server.replacing_design(diagnosis=items):
                netlist.load_verilog(new_design)

        Only with `lock` (or the default one): with `run=`, swap on the
        thread `run` hops to and call design_changed() there."""
        if self.lock is None:
            raise RuntimeError("replacing_design() needs a lock; with run=, call "
                               "design_changed() on the thread run hops to")
        with self.lock:
            yield
            self.design_changed(diagnosis, instance)

    def on_select(self, callback):
        """Call `callback(id_path, path)` each time a viewer selects an
        instance of the current design: its instance ids and names, top
        excluded ([] = the top design; a name is "" for an anonymous
        instance), or (None, None) when design_changed() clears the
        selection. Called in order, on a thread of the server's own, with no
        lock held; a selection is not delivered once design_changed() has
        started a newer generation. Returns `callback`, for
        remove_select_callback()."""
        return self._session.on_select(callback)

    def remove_select_callback(self, callback):
        self._session.remove_select_callback(callback)

    @property
    def selected_id_path(self):
        return self._session.selected_id_path

    @property
    def selected_path(self):
        return self._session.selected_path

    @property
    def selected(self):
        """The last selected instance as a najaeda netlist.Instance, or
        None if nothing is selected or it no longer exists."""
        return self._session.selected

    # -- server thread -------------------------------------------------------

    def _thread_main(self, ready):
        loop = asyncio.new_event_loop()
        self._loop = loop
        try:
            loop.run_until_complete(self._serve(ready))
        except BaseException as e:
            if not ready.is_set():
                self._start_error = e
            else:
                log.exception("naja-schematic server stopped on an error")
        finally:
            ready.set()
            self._loop = None
            loop.close()

    async def _serve(self, ready):
        from websockets.asyncio.server import serve as ws_serve

        # Bound here, not by websockets: with port 0 and a name like
        # "localhost", each address family would get a different port.
        sock = socket.create_server(
            (self._host, self._port),
            family=socket.AF_INET6 if ":" in self._host else socket.AF_INET)
        self._port = sock.getsockname()[1]
        self._stopping = asyncio.Event()
        # One worker: viewer requests are answered one at a time. Not the
        # loop's default executor, whose shutdown would wait for a request
        # stuck in the host's `run`.
        self._executor = ThreadPoolExecutor(max_workers=1,
                                            thread_name_prefix="naja-schematic-request")
        try:
            async with ws_serve(self._handle, sock=sock, origins=self._origins(),
                                process_request=self._process_request, max_size=None):
                ready.set()
                await self._stopping.wait()
                # Abandon requests still waiting for the host; their
                # handlers return, and closing the server closes the rest.
                for pending in list(self._pending):
                    pending.cancel()
        finally:
            self._executor.shutdown(wait=False, cancel_futures=True)

    def _origins(self):
        if self._host in _WILDCARD_HOSTS:
            # Reachable under names we can't know: rely on the token.
            return None
        hosts = {_url_host(self._host)}
        if self._host in ("127.0.0.1", "localhost"):
            hosts |= {"127.0.0.1", "localhost"}
        # None: a non-browser client, which sends no Origin.
        return ([f"http://{h}:{self._port}" for h in sorted(hosts)]
                + self._allowed_origins + [None])

    def _process_request(self, connection, request):
        if request.headers.get("Upgrade", "").lower() != "websocket":
            return _static_response(connection, request)
        if self._token:
            query = urllib.parse.urlsplit(request.path).query
            given = urllib.parse.parse_qs(query).get("token", [""])[0]
            if not hmac.compare_digest(given, self._token):
                return connection.respond(http.HTTPStatus.FORBIDDEN, "Bad or missing token\n")
        return None

    async def _handle(self, websocket):
        from websockets.exceptions import ConnectionClosed

        loop = asyncio.get_running_loop()
        self._clients.add(websocket)
        log.info("Viewer connected")
        try:
            async for message in websocket:
                log.debug("Received: %s", message)
                pending = loop.run_in_executor(self._executor, self._session.answer, message)
                self._pending.add(pending)
                try:
                    replies = await pending
                except asyncio.CancelledError:
                    if not pending.cancelled():
                        raise  # this task itself is being cancelled
                    return  # stop() abandoned the request
                finally:
                    self._pending.discard(pending)
                for reply in replies:
                    await websocket.send(reply)
        except ConnectionClosed as e:
            log.info("Viewer disconnected: %s", e)
        finally:
            self._clients.discard(websocket)

    def _dispatch_callback(self, fn):
        try:
            self._callbacks.submit(fn)
        except RuntimeError:  # interpreter shutting down
            pass

    def _broadcast(self, message):
        # Called on the host's thread: hand the message to the server loop.
        loop = self._loop
        if loop is None:
            return
        try:
            loop.call_soon_threadsafe(self._send_all, message)
        except RuntimeError:  # loop closed under us: nobody to send to
            pass

    def _send_all(self, message):
        from websockets.asyncio.server import broadcast
        broadcast(self._clients, message)


# ---------------------------------------------------------------------------
# stdio server
# ---------------------------------------------------------------------------

def serve_stdio(diagnosis=None, stdin=None, stdout=None):
    """Answer one JSON request per stdin line with JSON lines on stdout,
    until stdin closes. Logging must not go to stdout in this mode."""
    stdin = stdin or sys.stdin
    stdout = stdout or sys.stdout
    session = ViewerSession(diagnosis=diagnosis)
    for line in stdin:
        line = line.strip()
        if not line:
            continue
        for reply in session.answer(line):
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

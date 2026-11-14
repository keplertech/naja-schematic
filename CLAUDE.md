# CLAUDE.md

This file provides guidance to AI coding agents (Claude Code, Codex, and others)
when working with code in this repository.

## Project overview

naja-schematic is a C++20 ImGui-based netlist viewer/schematic browser for the
[naja](https://github.com/najaeda/naja) SNL netlist data model. It builds to two
distinct targets from the same core sources (see Architecture below): a native
desktop app and a WASM app that runs in a browser or a VSCode webview.

## Vision: part of an AI RTL-diagnosis loop

naja-schematic exists to make **AI-generated diagnosis reports on RTL/netlists
visually inspectable by a human**, not to be a general-purpose EDA schematic
viewer. The target workflow it serves:

1. An AI agent (e.g. Claude) edits/manipulates RTL, optionally using
   **naja-scope** (`https://github.com/najaeda/naja-scope`) — an MCP server,
   sibling to this repo on the same naja/SNL base, that lets an agent query a
   netlist (connectivity, hierarchy, fan-in/fan-out) without pasting RTL into
   context. It has no GUI and produces no files — it's a query layer for
   agents, not a diagnosis/report source.
2. Changes are checked with **kepler-formal**
   (`https://github.com/keplertech/kepler-formal`) — an LEC/SEC equivalence
   checker that operates on Verilog/SystemVerilog and, notably, **speaks the
   Naja interchange format directly**. Its output today is exit codes plus
   text/log reports (miter logs, `boundary_terms.txt`, `skipped_*.txt`); there
   is **no structured/JSON diagnosis format published yet** anywhere in this
   ecosystem, and internal instance/net names aren't guaranteed stable across
   designs in its SEC diagnostics (only top-level terminal names are).
3. **naja-schematic** is meant to be the visualization endpoint, but the
   report hand-off format from kepler-formal doesn't exist yet — building it
   is part of closing this loop, not something to assume is already there.

This is why ImGui was picked (portability: native + WASM/browser/VSCode
webview, so the viewer can be embedded wherever the loop runs) and why naja/SNL
was picked for netlist representation (kepler-formal already speaks it, so no
netlist-level translation is needed — only a report-level one).

**Integration surface (implemented):** the `diagnosis_response` message type
and its rendering — see "Diagnosis overlay" below — plus, for the native
standalone build, a `--diagnosis <path>` CLI flag (`main_native.cpp`) that
loads a design and a diagnosis JSON in one command, so an external caller
(a script, or naja-agent's skill) can open a fully annotated view without a
human clicking through File > Open .../Load Diagnosis JSON... by hand. This
closes the "opening a view" half of the loop for native builds. The browser
build has the same one-shot launch through the `naja_schematic` Python
package's CLI (`naja-schematic --verilog d.v --diagnosis diag.json --open`,
see "Python package" below), and notebooks get it through
`naja_schematic.show(diagnosis=...)`. What's still missing upstream either way: neither kepler-formal
nor naja-scope emits `diagnosis_response` JSON today, so an adapter that
turns kepler-formal's log/text output into `diagnosis_response` items is the
remaining piece to actually produce the file this flag consumes.

## Building

Clone with submodules (`thirdparty/imgui`, `thirdparty/naja`,
`thirdparty/ImGuiColorTextEdit`) — if already cloned
without them: `git submodule update --init --recursive`.

### Native standalone target (`naja-schematic-standalone`)

Uses CMake presets (`CMakePresets.json`), which pin the compiler to system Apple
Clang so Emscripten on `PATH` doesn't get auto-detected. Build directory lives
*outside* the repo, at `../naja-schematic-build/<preset>`.

```bash
cmake --preset native-debug      # or native-release
cmake --build --preset native-debug
```

Requires SDL2 and OpenGL (e.g. `brew install sdl2`). Pulls in `thirdparty/naja`
as a CMake subdirectory (`naja_nl`, `naja_snl_verilog`, `naja_snl_liberty`,
`naja_snl_systemverilog`, `naja_nl_dump`).

The native target also builds on Linux (see `.github/workflows/native-linux.yml`
for the exact apt package list — boost/capnproto/tbb/SDL2/OpenGL dev headers
plus `zenity`), but not via `CMakePresets.json`: those presets pin
`/usr/bin/cc`/`/usr/bin/c++` specifically to dodge Emscripten-on-PATH
auto-detection on macOS (see WASM section below) and aren't meant for other
platforms — invoke `cmake` directly instead
(`cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug`). File-picker
backend is chosen per-platform in `CMakeLists.txt`: `NativeFileDialog.mm`
(Cocoa/`NSOpenPanel`) on `APPLE`, `NativeFileDialogLinux.cpp` (shells out to
`zenity`) otherwise — both implement the same `NativeFileDialog.h` interface.
Windows has no backend and isn't a supported target.

Run directly, optionally with a netlist to load (dispatched by extension —
`.v` → Verilog, `.sv` → SystemVerilog via slang, `.vhd`/`.vhdl` → VHDL via
naja's experimental frontend (`LocalSNLProvider::loadVHDL()`,
`VHDLConstructor::constructFiles()`; the only kind that takes several
positional files, in any order -- naja resolves the units across them),
otherwise treated as an SNL directory), `--top <name>` (`.sv`/VHDL only:
forces the top module/entity instead of inferring it), `--liberty <path>`
(repeatable, once per file — defines the
primitive cell library for a `.v` design; not supported for `.sv` or VHDL,
since `LocalSNLProvider::loadSystemVerilog()`/`SNLSVConstructor` and
`loadVHDL()` have no liberty hook, and the CLI fails loudly rather than
silently ignoring it), a
diagnosis JSON to pre-load (`--diagnosis <path>` — see
`loadDiagnosisFile()` in `AppLogic.cpp`), a style JSON to pre-load
(`--style <path>`, `loadStyleFile()`; see `style_response`), and/or directories to look up
relative source file references in (`--source-root <dir>`, repeatable; see
"Source references" under Wire protocol), path prefixes to rewrite for
files recorded on another machine (`--source-map FROM=TO`, repeatable),
and `--yosys-src` (`.v` only:
decode Yosys `(* src *)` attributes into source origins; also a checkbox in
File > Open Verilog...) — any order, all optional:

```bash
../naja-schematic-build/native-debug/naja-schematic-standalone [path/to/netlist] [--liberty path/to/cells.lib]... [--diagnosis path/to/diagnosis.json] [--source-root dir]... [--source-map FROM=TO]... [--yosys-src]

# gate-level Verilog + Liberty example:
../naja-schematic-build/native-debug/naja-schematic-standalone design.v --liberty cells.lib
```

**Headless SVG export.** `--export <out.svg>` draws the schematic to a
file and exits, with no window (`HeadlessExport.cpp`, run before any SDL
init). It uses the same requests and `AppLogic` response handling as the
GUI through `HeadlessView` (a backend-less ImGui context with the app's
font, so names are measured the same way, and the schematic panel alone),
then `SchematicView::exportSvg()`. The Python package has the same export
for designs it holds, including primitives defined in Python (see
`render.py` under Python package); both give byte-identical SVG for the
same design and targets. It needs at least
one of these:
- `--focus <json>`: one instance drawn alone with every pin open. Takes a
  list of ids (`[3,7]`, an `id_path`), a list of names (`["u1","u2"]`), or
  an object with `id_path`/`path`. It clears the view, so it's applied
  first.
- `--trace <json>` (repeatable): a fan-in cone (`trace_driver`).
- `--equipotential <json>` (repeatable): one net (`load_equipotential`),
  colored as a one-net trace like the tree's "Show Equipotential".

A `--trace`/`--equipotential` argument names a terminal,
`{"id_path"|"path": <containing instance>, "terminal": "A", "bit": 0}`,
where no path or `[]` means a top-level port. It's resolved to term ids by
`LocalSNLProvider::termRequest()` (native only, no protocol change). The
exit code is non-zero, and nothing is written, if the design didn't load,
anything didn't resolve, or there's nothing to draw. `--diagnosis` is
accepted, but tints nothing while `kDiagnosisUIHidden` is set.

```bash
naja-schematic-standalone design.v --liberty cells.lib --export cone.svg \
  --trace '{"terminal":"y"}' --focus '["u1","u2"]'
```

### Headless renderer (`naja-schematic-render`)

The viewer with no window, no SDL/GL and **no naja** (`src/main_render.cpp`
+ `HEADLESS_SOURCES` in `CMakeLists.txt`): what the Python package's
`export_svg()`/`naja-schematic render` run, shipped in the platform wheels.
Built with the native targets, or alone (only imgui and a C++ compiler):

```bash
cmake -S . -B ../naja-schematic-build/render -DCMAKE_BUILD_TYPE=Release -DNAJA_SCHEMATIC_RENDER_ONLY=ON
cmake --build ../naja-schematic-build/render
export NAJA_SCHEMATIC_RENDER=$PWD/../naja-schematic-build/render/naja-schematic-render
```

It's a viewer like the browser one, with the Python host answering its
requests from the live `NLUniverse` -- which is why it exists rather than
dumping the design to naja-if for `--export`: a naja-if dump drops the
modeling primitives built in Python carry (sequential models, clock roles,
LUT truth tables from parameters, so traces stop at the first LUT), and
the native app refuses a snapshot from any other naja build. Transport:
JSON lines on stdin/stdout, strictly alternating -- each line it writes (a
request or notification) gets exactly one line back, a JSON array of
messages (replies, pushes, maybe none). Idle, it sends
`{"request":"render_idle"}`; the answer holds the next drawing's pushes
(`design_changed` to reset, `focus_instance`, `show_net`, `export_svg`),
and closing stdin ends it. An `export_svg` is answered, after the frames
settle, by `svg_exported` (`""` if nothing is drawn). stdout is the
protocol only (fd 1 points at stderr; `Console::Log` is off unless
`NAJA_SCHEMATIC_RENDER_VERBOSE=1`).

### WASM target (`naja-schematic`)

Requires Emscripten (`emcmake`/`emrun` on `PATH`). Detected automatically in
`CMakeLists.txt` by matching the compiler name (`em++`), not by a build flag.

```bash
mkdir build-wasm && cd build-wasm
emcmake cmake ..
cmake --build .
emrun --port 8080 naja-schematic.html
```

The WASM app doesn't link naja directly — it talks to a netlist server over a
WebSocket instead. Start the server first, optionally with `--verilog <path>`
(a single Verilog netlist) and `--liberty <path> [<path>...]` (one flag,
listing the Liberty files defining its cell library — see
`build_arg_parser()`/`add_design_arguments()` in `python/naja_schematic/server.py`;
`scripts/najaeda_server.py` is just a wrapper that runs that CLI from the
source tree without installing the package):

```bash
python3 scripts/najaeda_server.py   # serves ws://localhost:8081/ws

# gate-level Verilog + Liberty example:
python3 scripts/najaeda_server.py --verilog design.v --liberty cells.lib

# SystemVerilog (elaborated with slang) example:
python3 scripts/najaeda_server.py --systemverilog a.sv b.sv --top top -D SYNTHESIS

# SystemVerilog from a slang command file (sources, +incdir+, +define+, ...):
python3 scripts/najaeda_server.py --flist design.f --top top
```

A design is required, exactly one of: `--verilog`; SystemVerilog via
`--systemverilog`/`--sv <path>...` and/or `--flist`/`-f <file>` (the two SV
inputs can be combined; `--flist` is the raw loader's `flist`); or
VHDL via `--vhdl <path>...` (`NLDB.loadVHDL()`, files in any order).
`load_design()` uses najaeda's raw `naja` API only, like `protocol.py`
(`NLDB.loadGateLevelVerilog()`/`loadSystemVerilog()`/`loadVHDL()`/
`loadLibertyPrimitives()`); `--top` reaches slang as
`frontend_options={"slang": ["--top", name]}` (najaeda >= 0.7.34).
`--py-primitives <file.py|module>` (repeatable; `load_python_primitives()`)
calls the module's `load(db)` on the top NLDB before the design is loaded,
after `--xilinx` (najaeda's own `primitives.xilinx`) and before Liberty:
primitives built in Python, like kepler-formal's `py_tech_files`. Allowed with every design kind.
`--top` applies to SystemVerilog and VHDL, `--define`/`-D` only to
SystemVerilog, and `--liberty`/`--yosys-src` are rejected with SV and VHDL —
same restriction as the native CLI, since those loaders have no liberty
hook.
`--allow_unknown_designs` maps to the loaders' `blackbox_unknown_modules`.

Note the flag shapes are *not* symmetric with the native CLI above: the
server takes `--verilog <path>` (singular) + `--liberty <path>...` (one flag,
many files after it), while the native standalone takes a positional
`<design>` + a repeatable `--liberty <path>` (one flag per file) — don't
assume a reader moving between modes can reuse the same invocation shape.

(`scripts/test_server.py` is a minimal canned-response stub for protocol
testing without a real netlist backend.)

### Python package (`python/`, PyPI `naja-schematic`)

`python/naja_schematic` packages the Python side of the protocol with the
WASM viewer, so najaeda users get a viewer from `pip install` alone:

- `protocol.py` — the Python protocol implementation: `handle_request(dict)
  -> [dict]`, transport-agnostic, answering from the live `NLUniverse`. This
  is the file to keep in step with `LocalSNLProvider.cpp`.
- `session.py` — `ViewerSession`, one viewer's state above the protocol,
  shared by every transport below: it answers a message
  (`answer(json) -> [json]`), keeps the host's pushes (`annotate()`,
  `show_instance()`) and re-sends them after every `root_response` (the
  viewer clears diagnoses and selection on each root load), and records
  `instance_selected` notifications (`on_select(cb(id_path, path))`). Add
  host-facing behavior here, not in one transport. Universe access is split
  by who starts it: the viewer's requests go through the session's
  `run(fn)` hook (default: a private `RLock`), the host's own calls touch
  the universe directly on the host's thread -- so a host holding its lock,
  or running on the thread `run` hops to, never deadlocks on itself.
- `server.py` — the `naja-schematic` CLI (same flags as above, plus `--host`,
  `--open`, `--diagnosis <json>`, `--stdio`, `--source-root <dir>`
  (repeatable; `source_roots=` on `ViewerSession`/`ViewerServer`/`show()`),
  `--source-map FROM=TO` (repeatable; `source_maps=` there too),
  `--style <json>` (`style=`, pushed after every `root_response`),
  `--yosys-src` (with `--verilog`: `loadGateLevelVerilog(yosys_source_attributes=)`))
  and its two transports: a
  WebSocket server that also serves the viewer page on the same port
  (`static/index.html` sets `Module.najaWsUrl` from `location`, query
  string included), and JSON
  lines on stdin/stdout for a host that relays messages itself. In `--stdio`
  mode fd 1 is pointed at stderr (naja's C++ logger writes to stdout) and
  protocol output goes to a private dup of the original stdout. With
  `--diagnosis`, a `diagnosis_response` is pushed after every
  `root_response`.
  Also `ViewerServer`, the same page + WebSocket as an object for a host
  application that already holds the design (nothing is loaded):
  `start()`/`stop()`/context manager, on a background thread with its own
  event loop; `port=0` (the default) binds a free port, final once
  `start()` returns (`.port`, `.url`). Viewer requests are answered one at
  a time on a single worker thread, through `lock=` (held around each,
  exposed as `.lock`; a private `RLock` by default) or `run=` (e.g. hop to
  the host's thread). `stop()` abandons a request stuck in the host rather
  than waiting for it. The WebSocket requires a random `token` (in `.url`'s
  query, forwarded by `index.html`) and rejects browser `Origin`s other
  than its own page and `allowed_origins`, so other web pages can't read
  the design; `token=None` turns the token off. Pushes go to every
  connected viewer (`websockets` `broadcast`); selection callbacks get
  `(id_path, path)` in order on a callbacks thread with no lock held.
  `export_svg(path=None, callback=None, timeout=30.0)` returns the SVG
  (blocking; `TimeoutError` past `timeout`), or with `callback` returns at
  once; waiting refuses to run on the callbacks thread, and must not hold
  `lock` (the answer is a viewer request). The
  host swaps or edits the design inside `with server.replacing_design():`
  (or calls `design_changed()` itself, in the same critical section):
  every viewer reloads, and requests made for the old design are dropped
  (see `design_changed` under Wire protocol).
- `render.py` — SVG export with no browser or display:
  `export_svg(targets=None, path=None, *, nets=, instances=, traces=)`
  and `export_svgs([{...}, ...])` (one renderer process, each drawing on
  a clean view; all jobs resolved before anything is drawn, files written
  once all are drawn), on the design in the current `NLUniverse`, read on
  the calling thread. Drives `naja-schematic-render` (`bin/` in the
  platform wheels, or `$NAJA_SCHEMATIC_RENDER`; see "Headless renderer"
  above) as a viewer, answering through a `ViewerSession`, so the SVG is
  what a viewer showing the same things exports. Targets are resolved
  first (`session.instance_paths()`/`net_pin()`, `ValueError` on a bad
  one) into pushes (`drawing_pushes()`): `design_changed` (a clean view),
  the instances (`focus_instance`, then with `"add": true`), the nets and
  traces (`show_net`, traces with `"trace": true`), `export_svg`. In
  `targets` a tuple is one pin and a list holds several targets, so an
  instance path there is a list inside the list. Also the `naja-schematic
  render` subcommand (`render.main()`, dispatched from `server.main()`):
  the design flags plus repeatable `--instance PATH`, `--net INST:TERM[BIT]`,
  `--trace INST:TERM[BIT]` (or JSON, as the native `--focus`/`--trace`),
  `--svg FILE`; exits non-zero, writing nothing, on any unresolved
  target. Text paths are matched against the design (`/` ends a name
  only where such an instance exists, `_instances_matching()`), never
  split blindly, and ambiguity is an error. Tests:
  `tests/test_render.py` (drawing tests skip without a renderer unless
  `NAJA_SCHEMATIC_REQUIRE_RENDER=1`, as in CI).
- `widget.py` — `naja_schematic.show()`: an anywidget for Jupyter/Colab/
  VSCode notebooks. Its ES module is the bundle + `static/widget.js`; the
  viewer's requests come back over the widget comm channel as
  `{"json": "<message>"}` and are answered in the kernel (through a
  `ViewerSession`), so the view shows
  the netlist as edited by earlier cells. `Schematic.annotate(items)`
  pushes diagnoses; `show_instance()`/`selected`/`on_select()` exchange
  instances with najaeda (see "Getting to one hierarchical instance"
  under Wire protocol); `show_net()` draws a net as a colored trace (see
  `show_net` under Wire protocol). `ViewerSession`, `ViewerServer` and
  `Schematic` all have `show_net()` (`trace=True`: the pin's fan-in cone).

Wheels: the `build` job makes the sdist and a pure `py3-none-any` wheel
(no renderer: everything but `export_svg()`), the `platform-wheels` job
one wheel per platform with `bin/naja-schematic-render`, each on its own
runner in parallel: manylinux_2_28 x86_64 and aarch64 (in the manylinux
containers, `ubuntu-24.04-arm` for aarch64; libstdc++ linked statically),
macOS universal2, and Windows x64 (`win_amd64`, MSVC with the static CRT,
`naja-schematic-render.exe`; "Open in Editor" is compiled out there, its
shell quoting being POSIX). Tagged `py3-none-<platform>` by
`python/hatch_build.py` from `$NAJA_SCHEMATIC_WHEEL_PLATFORM` (the
executable is gitignored, like the bundle, and only included when present).
`test` runs on one runner per platform wheel (ubuntu, ubuntu arm, macOS,
Windows) and installs the wheel pip picks there from the built ones; its
`renderer_path()` check fails if that's the pure wheel.

The viewer bundle `static/naja-schematic.js` is **not** checked in: it's the
WASM target configured with `-DNAJA_SCHEMATIC_WASM_MODULE=ON` (single file,
wasm inlined, `createNajaSchematic({canvas, ...})` factory, one instance per
canvas), built by `.github/workflows/python-package.yml`, which also tests
the wheel and publishes it to PyPI on a `v<version>` tag (version in
`naja_schematic/__init__.py`, the project's single version: `CMakeLists.txt`
also reads it into `NAJA_SCHEMATIC_VERSION_STRING` for the C++ app's About
dialog, so a release is one edit plus a tag). For local work set
`NAJA_SCHEMATIC_BUNDLE` to a locally built bundle (see `_bundle.py`). Tests:
`pytest python/tests`. `python/notebooks/getting_started.ipynb` is the
Open-in-Colab notebook (badge in the top `README.md`): a walkthrough of the
widget against the released PyPI package. `python/notebooks/tinyrocket.ipynb` (a
gate-level RISC-V core with Liberty) is its sequel, kept apart because
najaeda holds one design at a time: in one notebook, *Run all* would leave
every view showing the last design. `tests/test_notebook.py` runs both
in a real kernel (nbclient) and, in the walkthrough, splices in a cell that plays the viewer
against its `view`, so a `Schematic`/protocol change that breaks it fails
CI; its install cell skips `pip install` when `naja_schematic` is already
importable, so CI tests the fresh wheel, not PyPI's. The workflow's
`colab` job runs the same test the other way (`NAJA_SCHEMATIC_COLAB_CHECK`):
the branch's notebook installing from PyPI as Colab does -- the current
release on PRs/main/weekly, the just-published one after a tag -- so a
notebook edit needing an unreleased API, or a release that breaks it, fails CI.

## Architecture

### Dual-mode, shared core

The app has two entry points that share almost everything except how netlist
data is sourced:

- **`src/main_native.cpp`** — native desktop: SDL2 + OpenGL 3.3 core, a plain
  `while` loop calling `appFrame()`. Backed by `LocalSNLProvider`, which loads
  netlists directly through the naja SNL C++ API in-process (Verilog,
  SystemVerilog, or pre-built SNL directories).
- **`src/main_wasm.cpp`** — browser/VSCode webview/notebook: SDL2 + OpenGL
  ES via Emscripten's `emscripten_set_main_loop()`. Backed by
  `JsBridgeProvider` when the host page sets `Module.najaSend` (embedded
  mode: the host owns the transport, requests go out through
  `Module.najaSend(json)` and replies come back through the embind-exported
  `Module.deliverMessage(json)` — what the notebook widget uses), otherwise
  by `WebSocketProvider`, a thin wrapper around `WebSocketClient` that
  connects to `Module.najaWsUrl` or `ws://localhost:8081/ws` (served by the
  `naja_schematic` Python package, see above). `main()` maps SDL's
  hard-coded `"#canvas"` selector to the instance's own `Module.canvas`
  (`specialHTMLTargets`), so several viewers can share a page, and with
  `Module.najaEmbedded` limits keyboard capture to the focused canvas.

Both providers implement **`INetlistProvider`** (`src/INetlistProvider.h`):
`send()`, `on_open()`/`on_message()`/`on_close()`/`on_error()` callback
registration, and `start()`. `LocalSNLProvider::start()` fires `on_open`
synchronously and answers requests in-process, as does `JsBridgeProvider`'s
(the host channel is already up); `WebSocketProvider` is a passive
wrapper since the underlying socket connects in its constructor.

`src/AppLogic.h` + `AppMessages.cpp`/`AppLogic.cpp` hold the logic shared
by the entry points. `AppMessages.cpp` has no SDL/GL (the headless
renderer links it without them; `AppLogic.h` only forward-declares
`SDL_Window`), `AppLogic.cpp` the windowed frame loop:
- `setupProvider(AppState&)` (`AppMessages.cpp`) wires all provider
  callbacks and calls `provider->start()`. All response-message dispatch (`root_response`,
  `instances_response`/`primitives_response`/`children_loaded`,
  `terms_response`, `nets_response`, `equipotential_response`,
  `expanded_instance_terms`, `error`, ...) lives here, driven by the
  `"response"` field of incoming JSON —
  this is the one place to look when tracing how a server/provider reply turns
  into UI state.
- `appFrame(AppState&)` runs one ImGui frame (poll events, build UI, render)
  and returns `false` on quit.

### Wire protocol

**Instance paths are lists: of ids, and of names.** Everywhere a
hierarchical instance is named, it's a list, root excluded (`[]` = top),
never a `"u1/u2"` string: escaped Verilog names (`\a/b `) can contain
`/`, so splitting on it corrupts the path. A bare string passed to the
Python API is one name, never split. Don't add `/`-parsing conveniences.

A path comes in two parallel forms:
- **`id_path`** — naja instance ids (`SNLInstance::getID()`, unique among
  the instances of the parent design; najaeda's `Instance.pathIDs`), e.g.
  `[3, 7]`. This is the identity, and the preferred form: it's the only
  one that reaches **anonymous instances**, which all have the name `""`
  (so two anonymous siblings are indistinguishable by name). Netlist
  instances are never renamed to make names unique.
- **`path`** — instance names, e.g. `["u1", "u2"]` (`""` for an anonymous
  level). Kept for display and for backward compatibility: every message
  that accepted a name path still does.

Requests that name an instance (`resolve_instance`, `get_properties`,
`instance_selected`), the `focus_instance` push and diagnosis items
carry `id_path` and/or `path`; when `id_path` is present it wins. A name
path never resolves through `""` (anonymous instances need `id_path`).
Both backends resolve with the same rule (`resolveRequestPath()` in
`LocalSNLProvider.cpp`, `resolve_request_path()` in `protocol.py`), and
both send an anonymous instance's name as `""` (not naja's `getString()`
placeholder). The shared cases in `tests/data/anonymous_protocol_cases.json`
are run against both (`python/tests/test_anonymous_instances.py`,
`tests/provider/LocalSNLProviderProtocolTest.cpp`) -- add a case there
when changing how either resolves or answers.

**Unnamed terms and nets** get the same treatment. The gates naja builds
for SystemVerilog operators (`and_N`, `or_N`, `not_1`, ...) have unnamed
terms (output id 0, input bus id 1), and elaboration leaves many nets
unnamed. Both backends send their name as `""` (never a made-up one), and
the viewer shows `<term #id>` (`displayTermName()`, `displayPinLabel()`
for a label already ending in `[bit]`) and `<net #id>` (`displayNetName()`)
-- distinct from an instance's `<#id>`. Display only, never parsed: terms
and nets are addressed by id. A gate symbol's bus pin draws only its range
(`[1:0]`); the full name is in its tooltip.

The C++ viewer follows the same rule internally: an instance's identity is
an `InstancePath` (`std::vector<InstanceRef>`, `Types.h`), each segment an
`{id, name}` pair that compares **by id only**, used directly as map/set
key; composite keys are `std::tuple`s, never strings joined with `/`, `|`
or any other separator. `writePath()`/`readPath()` put it on the wire as
the `path` + `id_path` pair (`instance_path` + `instance_id_path` for the
echoed tags). Joined strings exist only for display (`displayPath()`,
`properties_response`'s `subject`), where an anonymous level shows as
`<#id>` (`displayName()`, `display_name()` in `protocol.py`), and are
never parsed back.

Requests/responses are JSON with a `"request"`/`"response"` type field (e.g.
`load_root`, `load_instance`, `load_primitives`, `load_terms`, `load_nets`,
`load_equipotential` → `*_response`). Both `LocalSNLProvider` (native,
`buildRootResponse()`/`buildInstancesResponse()`/etc.) and
`python/naja_schematic/protocol.py` (WASM/browser/notebook) must independently implement this same
protocol — when changing one side, check the other.

`root_response` also carries `"server"`, what answers the viewer:
`{"name", "version", "naja", "naja_git"}` -- `server_info()` in
`protocol.py` (this package's `__version__`, `naja.getVersion()`,
`najaeda.git_hash()`; also logged at server start), and in
`LocalSNLProvider::buildRootResponse()` the app itself
(`NAJA_SCHEMATIC_VERSION_STRING`, `naja::NAJA_VERSION`/`NAJA_GIT_HASH` from
the linked naja). Help > About shows it under "Server" (`AppState::serverInfo`);
the viewer's own section has only its version and commit, since a browser
viewer contains no naja.

`load_nets`/`nets_response` mirrors `load_terms`/`terms_response` exactly
(same `gui_id`/`design_ref` request shape, same bus-vs-scalar `msb`/`lsb`
response shape, lazily populating a `NetlistTreeGroupNode::Type::Nets` group
next to `Terms` under an instance node) with one deliberate asymmetry: a net
entry carries no `child_id` and offers no "Show Equipotential" action — a
net has no direction, and get_properties identifies it by name (`"net"`
field) rather than by the provider-specific numeric id a term's
`load_equipotential` request needs. It does carry its `net_id`
(`SNLNet::getID()`), only to anchor review threads (`getNetID()`; absent
from older hosts). See `NetlistTreeNetNode`/
`NetlistTreeBusNetBitNode` in `NetlistTree.h/.cpp`.

Both `nets_response` and the `has_nets` flag filter out anonymous scalar
constant nets (unnamed 1'b0/1'b1 tie-offs, e.g. naja's implicit tie-off of
an unconnected input) — structural noise, not user-authored signals — via
`isAnonymousConstantNet()`/`hasVisibleNets()` (`LocalSNLProvider.cpp`) and
`is_anonymous_constant_net()`/`has_nets()` (`protocol.py`), the same
spirit as `hasVisiblePrimitiveInstances()` filtering `isAssign()`
primitives. A *named* or *bus* constant is still shown — the filter is
deliberately narrow (unnamed **and** scalar **and** constant 0/1) so it
only hides the implicit tie-offs, not anything a designer wrote by hand.

`trace_driver`/`trace_driver_response` is "Show Equipotential" extended to the
whole combinational fan-in cone: same `path`/`term_id`/optional `bit` request
shape as `load_equipotential` (plus an optional `bits` list to trace several
bits of one bus term in a single request), but the reply is *one* message
holding every net in the cone rather than one net:

```json
{ "response": "trace_driver_response",
  "equipotentials": [ {"terms": [...], "occurrences": [...]}, ... ],  // each = an equipotential_response body
  "truncated": false }
```
Starting from the requested net, each output pin on it is a driver; if its
cell has a combinational timing model (`SNLDesignModeling`) the cone crosses
it and continues with the net on every input pin that arc depends on. It stops
at sequential cells (flop/latch outputs), cells with no timing model
(blackboxes) and top-level input terms -- those are the "drivers" the cone
ends at. Each net lists its drivers plus only the receiver pins the trace
entered it through (the start pin, or the input pin of the cell being crossed;
several if the net is reached more than once) -- *not* every reader on the net,
unlike `equipotential_response`. Nets are returned breadth-first from the
requested one and de-duplicated, capped at `kMaxTraceNets` (`MAX_TRACE_NETS` in
`protocol.py`, 500) with `truncated: true` when hit. `AppLogic.cpp` adds
each to `GUIData` in order; the layered placement breaks ordering ties by
first appearance, so the cone keeps a stable shape as it grows. Reachable from the tree
(right-click a term/bus-bit row -> "Trace to Driver (replace view)", which
clears the view first, or "Trace to Driver (add to view)", which overlays the
trace) and from the schematic (right-click a pin -> "Trace to Driver (add to
view)"; the schematic never clears). The tree's "Show Equipotential" comes
in the same two variants (`clearView` on
`NetlistTree::sendLoadEquipotential()`/`sendTraceDriver()`). Both work one
bit at a time: a bus row offers neither for now (only its bit rows do). A
whole-bus trace (the `bits` list) is still supported by both providers but
has no menu entry.

Traces overlay: the viewer sends an optional `trace_id` (an int) with every
`trace_driver` request, and both providers echo it back unchanged in the
`trace_driver_response`. Each net the reply carries is tagged with it
(`Equipotential::traceIds`; `direct` marks a net also shown on its own), and
a net the view already shows is not added twice but gets the new trace
recorded on it (`GUIData::addEquipotential(eq, traceId)`). The schematic draws
each visible trace in its own `TraceStore` color. A wire tree two visible
traces go through is striped in their colors (`NetRoute::stripeColors`,
dashes anchored to world coordinates; plain convergence color when zoomed
too far out to read them), and a gate they share is outlined in the
near-black convergence color -- only a gate each trace's own reply listed
on the net (`Equipotential::traceInstances`): a trace's partial net folded
into a fuller one already shown doesn't reach that net's other readers. A
diagnosis color
always wins over a trace color. A hidden trace keeps its nets in the layout,
drawn faint, so nothing moves. `GUIData::removeTrace()` drops only the nets
nothing else still shows. A reply with no `trace_id` (an older server) still
becomes a trace of its own.

The tree's "Show Equipotential" is a **one-net trace**: `load_equipotential`
takes the same optional `trace_id` (registered under the pin's label plus
" (net)"), both providers echo it in the `equipotential_response` (even an
empty one), and the net gets a color, a Traces tab row and stripes where it
meets other traces. A schematic pin click sends no `trace_id`: those nets
are exploration and stay the plain wire color (a `direct` net). An empty
reply to a tagged request drops its trace, so no empty row is left.

`equipotential_response` also echoes the pin it was asked about,
`"start": {"path", "term_id", "bit"}`, and `"found"`: whether that pin
resolved. A found pin with nothing else on its net -- no net at all (an
empty reply), or a net holding only itself -- is **not connected**:
`EquipotentialView::noteUnconnectedReply()` marks it (`markUnconnected()`,
`Port::unconnected`), and it's drawn with a no-connect cross (on a
top-level port, past its flag's tip), says "Not connected" in its tooltip,
and is never requested again; the console logs it. A pin that didn't
resolve (`found: false`) is never marked. Shared cases in
`tests/data/anonymous_protocol_cases.json` cover both kinds.

Extending the schematic by hand works pin by pin. A box drawn with a dashed
border shows only the pins on nets already in view; double-clicking its body
sends `expand_instance_terms` and reveals the rest. Pins whose net isn't in
the view yet are "open" (`Port::open`, drawn with a hollow-circle stub), and a
**single click** on one sends `load_equipotential` for its net (the stub fills
while it loads; `SchematicInteraction::PendingRequests` stops repeat sends).
A single click on a merged bus pin shows its bits instead. Hovering a pin
highlights it and shows a tooltip naming its click action. When the requested
net arrives the view pans so the clicked box stays put on screen, even though
the whole schematic is laid out again; then, if what the net added (boxes and
port flags not shown before the click) lands off the canvas, the view pans
just enough to show it next to the clicked box, zooming out only when it
can't fit at the current zoom (`SchematicInteraction::revealRect`, extents
from `shapeWorldExtent()`). Pins are hit-tested nearest-first by
`SchematicInteraction::pickPin`. `GUIData::addEquipotential()` rejects a net
whose endpoints are all already shown (`equipotentialCovers()`), so neither a
re-click nor a trace re-listing a shown net draws its wires twice.

Occurrence `path` entries (in `equipotential_response` and each
`trace_driver_response` net) are `[name, child_id, model_name]`; the third
element is optional on parse (`InstTermOccurrence::pathModels`, `""` when
absent). It lets the schematic keep the design hierarchy of whatever it
shows (**View > Show Hierarchy**, on by default, also in the canvas context
menu): `layoutHierarchyGroups()` in `EquipotentialView.cpp` draws every
module enclosing a displayed leaf as a nested translucent frame labelled
`instance (Model)`, re-laying the leaves out inside it — each leaf keeps the
logic level the layered placement gave it (inside a frame its leaves and
sub-frames are bucketed into columns by level) and, where the column allows,
the height that aligned its pins. Frames are
`InstanceShape`s with `isHierGroup` set, inserted at the front of
`SchematicView::instances` and drawn before the nets
(`SchematicView::render()`); the leaves stay top-level shapes, so wiring and
hit-testing are unchanged. Right-clicking a frame offers Show Properties and
"Zoom to Module". The bottom "Equipotential" table lists every net of the
last trace (not just the last net) with a "Hierarchy" column giving each
occurrence's enclosing modules.

Instance entries (`instances_response`/`primitives_response` children,
equipotential occurrences, `instance_resolved`'s `instance`) carry a
`primitive_type` (`"and"`, `"nand"`, `"or"`, `"nor"`, `"xor"`, `"xnor"`,
`"inv"`, `"buf"`, `"dff"`, `"assign"`, `"tie0"`, `"tie1"`, `"mux"`,
`"add"`, `"sub"`, `"mul"`, `"shl"`, `"shr"`, `"ashr"`, `"divmod"`,
`"unknown"`) that picks the gate
symbol `SchematicView::drawInstance()` draws, and term entries carry
`"clock": true` on a sequential cell's clock pin (the DFF clock notch).
Both come from naja's modeling of the cell -- `SNLDesignModeling`'s
truth-table checks (`isAnd`, ..., `isConst0`/`isConst1` for tie cells, whose
Liberty `function` is `"0"`/`"1"`; `isMux`, at any width, since a bus mux
is one NLDB0 mux2), `isSequential` and `isClock` -- and
**never from a cell or pin name**: a cell naja has no model for (e.g.
gate-level Verilog without Liberty) is `"unknown"` and draws as a box. A
mux is a trapezoid (`SchematicLayout::muxSlant()`), the word-level
operators SystemVerilog lowers to (`NLDB0::isAddSub()`/`isSubtract()`,
`isMultiply()`, `isShift()` + `getShiftSignature()`, `isDivMod()`) are
boxes with a circled operator glyph (`isOperatorSymbol()`,
`kOperatorGlyphW`). `protocol.py` uses najaeda's bindings of the same
checks (`SNLDesign.isAddSub()`, `isSubtract()`, `isMultiply()`,
`isShift()`, `isShiftRight()`, `isShiftArithmetic()`, `isDivMod()`).

`diagnosis_response` is different: it's a **server push**, not a reply to a
request (a diagnosis run finishes on its own schedule), and it *annotates*
the already-loaded netlist rather than loading anything:

```json
{
  "response": "diagnosis_response",
  "items": [
    {
      "kind": "instance",          // "instance" | "net"
      "id_path": [3, 7],           // instance ids, root excluded; [] = top level (preferred)
      "path": ["u1", "u2"],        // instance names; used when id_path is absent
      "terminal": "Q",             // pin/port base name, no bus-bit suffix; "net" only
      "severity": "error",         // "info" | "warning" | "error"
      "message": "...",
      "source": "kepler-formal"    // free-form, shown in the UI
    }
  ]
}
```
Nothing upstream produces it yet (see Vision above), so the Python side
only relays a file or list it's given: the `naja-schematic --diagnosis
<json>` CLI and `Schematic.annotate()`/`show(diagnosis=...)` in notebooks
push it after each `root_response`; `scripts/test_server.py` sends a canned
example after `load_root` as a demo/test fixture. Native/standalone mode has no server at
all, so it gets diagnosis data via **File > Load Diagnosis JSON...**
(reads a `{"items": [...]}` file or a bare array through the same
`DiagnosisItem` parser) instead. An item with `id_path` matches that
instance only; one with only `path` matches by names, and never through
an anonymous (`""`) level. In Python, an item's `path` may also be a
naja `SNLPath` or a najaeda `netlist.Instance` (read through `pathIDs`,
without calling `netlist`), turned into both by `diagnosis_response()`.

Getting to one hierarchical instance works in both directions, keyed by
the same `id_path`/`path` pair as diagnosis items:

- **Host → viewer.** `focus_instance` is a server push,
  `{"response":"focus_instance","path":["u1","u2"],"id_path":[3,7]}`, from
  `Schematic.show_instance()` / `show(instance=...)` (which take a naja
  `SNLPath` or najaeda `Instance`, a list of ids, or a list of names), re-pushed after each
  `root_response` like diagnoses. With `"add": true` (`render.py`
  drawing several instances; needs `id_path`) the box is added to the
  view instead (`EquipotentialView::addOnResolve()`, as a review "go to"
  adds one) and the selection stays. The viewer answers it with a
  `resolve_instance` request carrying the same `path`/`id_path`,
  which both providers implement (`protocol.py`
  `_handle_resolve_instance`, `LocalSNLProvider::buildResolveInstanceResponse`).
  The reply looks like this:
  ```json
  { "response": "instance_resolved", "path": ["u1","u2"], "id_path": [3,7], "found": true,
    // path/id_path: the resolved instance's names and ids (the request's, as given, if not found)
    "instance": {                        // absent for the top design ([])
      "path": [["u1", 3, "Mod"], ["u2", 7, "AND2"]],   // [name, child_id, model] per level
      "design_ref": {...}, "primitive_type": "and", "has_instances": false, "has_sources": true,
      "terms": [ {"name": "A", "child_id": 0, "direction": 0}, ... ] } }  // expanded_instance_terms shape
  ```
  The viewer then:
  - clears the schematic and draws the instance alone with every pin open
    (`EquipotentialView::showInstance()`, parsed by
    `startInstanceFromResolved()`);
  - selects it (`SelectionStore`);
  - reveals it in the tree. `NetlistTree::reveal()` walks down the path,
    requesting each level's Instances/Primitives group as needed; it's
    asynchronous and `advanceReveal()` runs each frame.
- **Viewer → host.** There is one selected instance: click an instance row
  in the tree or a box/frame body in the schematic. Each change sends a
  notification with no reply,
  `{"request":"instance_selected","path":[...],"id_path":[...]}`, plus a
  `get_properties` for it. The notebook widget intercepts
  `instance_selected` into `Schematic.selected_id_path` (and
  `selected_path`, the names); `Schematic.selected` returns it as a
  naja `SNLPath` (raw API, like the whole package; empty for the top),
  and `on_select()` gives a callback (it
  observes the ids, so moving between anonymous siblings fires it). The other hosts only log it (`protocol.py`) or ignore it
  (`LocalSNLProvider`).

`export_svg` is a server push too, `{"response":"export_svg"}`, from
`Schematic.export_svg()` (`ViewerSession.export_svg()`): the viewer sets
`AppState::svgExportRequested` and, at the first frame its schematic shows
something, answers with a notification,
`{"request":"svg_exported","svg":"<svg ...>"}` -- what File > Export
Schematic as SVG... saves. The session intercepts it (like
`instance_selected`), keeps it as `svg` and calls the request's one-shot
callbacks; the widget also writes it to the `path` given. A notebook reads
it in a later cell or through the callback (e.g. updating a
`display_id` output), since comm messages reach the kernel between cells.

`show_net` is a server push too: a net drawn as a one-net trace (own
color, Traces tab row), as the tree's "Show Equipotential" does, from
`show_net(target, *, label=None, color=None, add=True)` (`ViewerSession`,
`ViewerServer`, `Schematic`):
```json
{ "response": "show_net", "id_path": [0, 0], "path": ["u_sub", "u_and"],
  "term_id": 2, "bit": 1,            // bit: bus bits only
  "label": "u_sub/u_and/O (net)",    // the Traces tab row
  "color": "#ff8800",                // optional: replaces the palette color
  "add": true,                       // false: clear the schematic first
  "trace": true }                    // optional: the pin's fan-in cone (trace_driver)
```
With `"trace": true` (`show_net(..., trace=True)`) the viewer sends a
tagged `trace_driver` instead, as the tree's "Trace to Driver" does, and
the `trace_driver_response` goes through `ShowNetStore::takeReply()` like
an `equipotential_response`; a net and a trace on the same pin are two
host nets. A viewer predating it ignores the key and draws just the net.
The pin is named by ids only (`id_path` + `term_id` + `bit`, what
`load_equipotential` takes; `path` is for display): the host resolves the
target -- a najaeda `Term`/`Net`/`Equipotential`, a naja `SNLOccurrence`,
`SNLInstTerm` (child of the top), top-level term or `SNLEquipotential`, or
an `(instance, terminal[, bit])` tuple; always one bit -- with
`session.net_pin()`, and validates the color (`trace_color()`: `#rrggbb`
or `(r, g, b)`, not near-black/near-white, like `TraceStore::setColor()`),
raising `ValueError` rather than pushing something that can't resolve. A
`Net` is drawn from a port of its design, else a cell (not assign) pin on
it; an equipotential from its first top-level port, else its first leaf
pin. The viewer (`ShowNetStore`, `AppLogic.cpp`) registers the trace and
sends a tagged `load_equipotential`; the reply is an ordinary
`equipotential_response`. The session keeps the pushes and re-sends them
after every `root_response`, after the focus: `show_instance()` and
`add=False` reset the kept list (both clear the view; `add=False` drops
the focus too), `design_changed()` empties it. Because of that re-send,
`ShowNetStore` skips a push for a pin it already shows as a host net,
gives a reply whose trace a focus cleared in flight its trace back (same
label and color), and `forget()`s its nets when the view is cleared (a
`focus_instance` push, an `add: false` push, the user's clears, a reload)
so their late replies aren't drawn. A tagged reply with `found: false`
logs the label. Native has no host, so `LocalSNLProvider` has nothing to
add (headless export has `--equipotential`).

`style_response` is a server push too: host styling, kept by the viewer
in `StyleStore` until the next one and cleared on every fresh
`root_response`/`root_loaded` (the host re-sends it after each, like
diagnoses):

```json
{ "response": "style_response",
  "items": [
    { "kind": "net",               // "net" | "instance" | "term"
      "id_path": [3], "path": ["drv_0"],   // as diagnosis items; [] = top-level port
      "terminal": "O",             // or "term_id"; "net"/"term" only
      "bit": 0,                    // optional: absent = every bit
      "color": "#1f77b4",          // "#rrggbb" or "#rrggbbaa"
      "width": 2.5,                // world units (SVG px at 1x)
      "fill": "#1f77b455",         // instances; translucent = tints the default fill
      "label": "copy 0" } ] }
```
A `net` item colors (and widens, and labels) every wire of each shown
equipotential with that pin among its ends (`StyleStore::netStyle()`), so
it must name a pin the viewer draws -- a leaf pin or a top-level port.
Python resolves every item (`session.style_response()`, `ValueError` on
a bad one) and sends a `net` item once per bit on its net's driver (a
top-level port, else an output leaf pin: the pin a trace's partial net
also lists), whatever pin or `target` (an instance or pin as
`show_instance()`/`show_net()` take) named it. An `instance` item fills
the box (also gate symbols, port flags; module frames keep their
translucency), outlines it in `color`/`width`, and `label` replaces the
drawn name (`InstanceShape::label`, so the layout makes room for it). A
`term` item colors/widens one pin's tick and `label` replaces its drawn
name. Items on one object merge in order, field by field.

**Color precedence**, the same on screen, in File > Export and in both
headless exports (all draw `SchematicView` through the painter):
diagnosis colors > style colors > trace colors (a trace's own, the
convergence color and stripes) > the default ink; a style's width,
fill and label apply whatever the color; the selection outline/halo is
drawn on top. In code: `EquipotentialView`'s `applyInstanceStyle()`
(before the convergence outline), `applyPinStyle()` (`Port::styleColor`
under the diagnosis `Port::color`), and `styledWireColor()` (a diagnosis
color in `NetWire::color`, else `styleColor`, else `traceWireColor()`).
Hosts: `style()` on `ViewerSession`/`ViewerServer`/`Schematic`,
`show(style=)`, `export_svg(style=)`, `--style FILE` on the
`naja-schematic` server and `render` CLIs; natively `--style FILE`
(`loadStyleFile()`), kept by `--export`. Not styled: wiring nested inside
an expanded instance (its internal nets aren't equipotentials).

`design_changed` is another server push, `{"response":"design_changed",
"generation":N}`: the host replaced or edited the design, so every tree
node, instance id and schematic box the viewer holds may be stale -- or,
worse, name something else, since a new design can reuse ids. The viewer
runs `reloadNetlist()` (`AppLogic.cpp`; also what native File > Open uses:
new `NetlistTree`, schematic cleared *immediately* with `resetLayout()`
rather than the next-frame `clearNets()`, which would wipe a focus drawn in
the same frame) and sends `load_root`.

**Design generations** keep the two designs apart, in both directions. The
Python host numbers its designs (`ViewerSession.generation`, 0 at start,
+1 per `design_changed()`) and stamps *every* message it sends -- replies
and pushes -- with `"generation"`: the one current when the request was
answered. The viewer wraps its transport in `GenerationProvider`
(`src/GenerationProvider.h`, `main_wasm.cpp`; tested in
`tests/GenerationProviderTest.cpp`), which adopts the first generation it
sees, switches on a newer `design_changed` (drops an older/repeated one),
drops any other message stamped with a different generation (replies to
requests made for the old design), and stamps every outgoing request with
its current generation. `ViewerSession.answer()` drops a request stamped
with a non-current generation *unanswered and without effect* -- a stale
`instance_selected` never selects or reaches the host's callbacks, a stale
`get_properties` never queries the new design. Unstamped messages mean "no
generations" and pass on both sides: a viewer's first `load_root`, older
viewers/hosts, and `LocalSNLProvider` (no wrapper natively: it never swaps
the design under the viewer). A viewer whose first `load_root` is overtaken
by a `design_changed` gets two roots for the new design; `AppLogic` ignores
a `root_response` when the tree already has a root.

Atomicity is the host's half of the contract: the generation check runs
inside the session's `run` hook, so the host must swap the design *and*
call `design_changed()` in one critical section -- under `lock`
(`ViewerServer.replacing_design()` does both), or on the thread `run` hops
to. Then a request already queued behind the lock is checked after the
bump and dropped. The selection is also re-checked when recorded
(`_commit_selection`, in case a bump lands between answer and record) and
again when each callback is delivered (`_notify`): `ViewerServer` delivers
selection callbacks in order on one `naja-schematic-callbacks` thread
(`dispatch=`; the widget delivers inline), so a selection queued before a
`design_changed()` is never delivered after it. `design_changed()` also
replaces the kept diagnoses/focus (they name the old design) with the ones
passed, resolved against the new design, and clears the selection
(`on_select` callbacks get `(None, None)`). `tests/test_design_generations.py`
covers delayed selections, requests queued behind the swap and several
viewers deterministically; the netlist there is "replaced" by itself, the
worst case where every stale id still resolves.

**Reviews** (full design: `docs/review_format.md`): threads on netlist
objects, shared asynchronously through a directory committed with the
design. The host owns the files (Python: `review.py`, `--review DIR`,
`review=`; not yet `LocalSNLProvider`) and pushes `review_response` after
each `root_response` and every write; the viewer sends `review_add_thread`
(an anchor by ids: instance `id_path`, `term_id`, `net_id` from
`nets_response`, optional `bit`) and `review_add_event` (reply, status), and
a refused write comes back as `{"response":"error","request":"review_..."}`.
Objects are identified by naja ids only, never by names: a thread is pinned
to the netlist it was written on, stored in the review as a naja-if
snapshot named by naja's content hash (`NLUniverse.getContentHash()`,
najaeda >= 0.7.35: a reloaded snapshot keeps it, a fresh load of the
sources keeps it only if naja assigns the same ids), and only drawn when the
host resolves it `exact` on the loaded design (`other_snapshot` threads are
listed, not drawn). A thread can't be opened while the universe holds
several design DBs (a snapshot is the top DB's dump).

`get_properties`/`properties_response` is a general name/value inspector for
whatever object the UI asks about — an instance (including the top design
itself), a term/pin, or a net — answered by both `LocalSNLProvider`
(`buildPropertiesResponse()`) and `protocol.py` the same request/
response way as `load_terms` etc. (unlike `diagnosis_response`, it's not a
push). The object is identified the same way `DiagnosisItem` identifies
things — an `id_path` and/or a name `path`, root excluded — and both
backends resolve it by walking down from the top design:

```json
// request
{
  "request": "get_properties",
  "kind": "instance",        // "instance" | "term" | "net"
  "id_path": [3, 7],         // instance ids (preferred); "instance": path to the object itself ([] = top design);
                              // "term"/"net": path to the *containing* instance ([] = a top-level port/design net)
  "path": ["u1", "u2"],      // instance names, used when id_path is absent
  "terminal": "Q",           // "term" only: pin/port base name, no bus-bit suffix
  "net": "internal_bus",     // "net" only: net base name, no "[bit]" suffix
  "bit": 3                   // "term"/"net" only, optional: a specific bus bit
}
// response
{
  "response": "properties_response",
  "subject": "u1/u2",        // human-readable label for the object, shown as a heading
  "properties": [ {"name": "Name", "value": "u2"}, {"name": "ID", "value": "7"},
                  {"name": "Model", "value": "AND2"}, ... ]   // Name is "" for an anonymous instance
}
```
An unresolvable path or unknown terminal/net yields an empty `properties`
list (not an error) — same "no properties" semantics as an object that
legitimately has none. Reachable from the tree (right-click an instance,
term/bus-bit row, or net/bus-net-bit row → "Show Properties") and from the
schematic (right-click an instance box); see `NetlistTree.cpp`'s render()
and `EquipotentialView.cpp`'s canvas context menu. Nets have no direction,
so unlike a term's properties there's no "Direction" entry, and nets don't
offer a schematic-side "Show Properties" entry point (only terms/instances
appear as boxes/pins there).

**Source references** (full design: `docs/source_information.md`; needs
najaeda >= 0.7.29; VHDL fills declarations since 0.7.32). naja keeps, per object, an optional *declaration*
(where it's written in the loaded input: a gate's netlist line, an
instance's instantiation, a design's definition) and ordered *origins*
(upstream RTL, e.g. Yosys `src`), each qualified by provider/language/
representation (`""` = unknown -- shown as is, never inferred, same rule as
gate types), plus the older unclassified legacy slot (`getSourceLoc()`).
SV and gate-level Verilog fill declarations; Yosys `src` becomes origins
only when asked (`--yosys-src`, `yosys_source_attributes`: naja never
guesses provenance). Entries carry a sparse `"has_sources": true`
(instances: own references -- `hasSourceReferences()`, allocation-free --
or legacy, or the model's declaration; a bus: itself or any bit) -- this
replaced the old inline `source_loc`. `get_sources`
takes the `get_properties` address and answers `sources_response` with
`subject`, the address echoed, and `sources`: `declaration`, `origin`s
(with `index`), `model_declaration` (instances), `legacy` only when there's
no typed reference; a bus bit without its own falls back to its bus's.
`load_source` names one entry (address + `role` + `index`), **never a
path**: the host reads only files the design references -- first through
each source map whose FROM prefixes it at a path boundary (if a file is
there), then an absolute path as is, a relative one under each source root
then the current directory (`resolveSourceFile()`/`resolve_source_file()`,
Python groups roots and maps as `protocol.SourcePaths`), whole up to 4 MiB, else
the lines within 500 of the reference (`first_line`, `truncated`). Both
backends: `requestSources()` in `LocalSNLProvider.cpp`,
`request_sources()` in `protocol.py`; shared cases in
`tests/data/source_protocol_cases.json`, on a Yosys-style gate netlist
both runners load with naja's Verilog loader (add one when changing
either). The raw Python API is read-only for sources, so Python tests get
source data by loading files, never by setting it.
The tree (flagged rows) and schematic boxes (and module frames, always)
offer "Show Sources"; the reply fills the "Source" tab, opens the first
entry's file, and grows the bottom panel to 45% of the height once
(`AppState::growSourcePanel`; never shrinks it). A found `source_response`
also says `"stale"`: the file was modified after the design was loaded
(`LocalSNLProvider::loadedAt_`, stamped in `freshDB()`/`setDB()`;
`SourcePaths.loaded_at`, stamped by `ViewerSession` at start and on each
`design_changed()`), so naja's ranges may not match it -- the Source tab
shows a warning. naja keeps no file hashes, so modification time is the
check. A found `source_response` also carries `path`, the absolute
path the host read, used by the native "Open in Editor".

### Core modules (`src/`)

- **`NetlistTree`** — the design hierarchy tree (instances, terms, nets, bus
  bits), lazily populated by provider requests as nodes are expanded.
- **`GUIData`** — top-level UI state container (the netlist tree plus the list
  of currently-displayed `Equipotential`s).
- **`SchematicView`** / **`EquipotentialView`** — the two main render panels;
  operate on the renderer-side types in `Types.h` (`InstanceShape`, `Port`,
  `NetWire`, `RenderEquipotential`) as opposed to the wire-format types
  (`InstanceResponseJson`, `TermResponseJson`, `Equipotential`, ...) also
  defined there.
- **`Console`** — in-app log/message panel.
- **`WebSocketClient`** — low-level Emscripten WebSocket binding used by
  `WebSocketProvider`.
- **`DiagnosisStore`** — global static store (same pattern as
  `EquipotentialView`) for the current `diagnosis_response`/loaded-JSON
  diagnosis set. Indexes items by instance path and by (path, terminal) net
  key; `NetlistTree` and `EquipotentialView` query it during rendering to
  tint flagged instances/pins/wires and show tooltips — see `instanceColor()`
  /`netColor()` in `src/DiagnosisStore.h`. Cleared on every fresh
  `root_response`/`root_loaded` (stale diagnoses reference the old design).
- **`DiagnosisView`** — renders the flat diagnostics list into the
  bottom-left panel in `AppLogic.cpp`.
- **`PropertiesStore`** — global static store (same pattern as
  `SourceStore`/`DiagnosisStore`) for the name/value list from the most
  recent `properties_response`. Cleared on every fresh
  `root_response`/`root_loaded`.
- **`PropertiesView`** — renders the current `PropertiesStore` contents as a
  two-column name/value table into the "Properties" bottom-panel tab.
- **`SourceStore`** — global static store (same pattern) for the "Source"
  tab: the last `sources_response` (subject, the object's address, its
  `SourceRef`s; `loadRequest(i)` builds the `load_source` for one) and the
  shown file (`source_response`: lines, `first_line` offset, the reference
  range; `highlightBytes()` gives the byte span to shade per line, by the
  1-based inclusive columns, never inventing an unknown end). Cleared on
  every fresh `root_response`/`root_loaded`.
- **`SourceView`** — renders `SourceStore` into the "Source" tab: the
  reference rows (click to load one) above the file, shown in a read-only
  [ImGuiColorTextEdit](https://github.com/goossens/ImGuiColorTextEdit)
  editor (submodule; `TextEditor.cpp` is in `CORE_SOURCES`) drawn with the
  embedded monospace Cousine font (`CousineFont.h`, loaded in
  `setupFonts()`): coloring by `VerilogLanguage`, find (Ctrl+F), copy, the
  reference's range as a background squiggle (byte columns converted with
  `SourceStore::glyphIndex()`), line markers for it and the object's other
  references into the same file, line numbers offset by `first_line` for a
  windowed file. Native only: "Open in Editor" runs
  `$NAJA_SCHEMATIC_EDITOR`, else VS Code (`code` on PATH, else the CLI in
  the app bundle), else `open`/`xdg-open` (`SourceStore::editorPattern()`,
  `editorCommand()`, path shell-quoted) on a worker thread, and shows its
  result or error next to the button.
- **`VerilogLanguage`** — the editor's Verilog/SystemVerilog language
  definition: keywords, types, `` ` `` directive lines, comments, strings,
  `$system` tasks, escaped identifiers and sized/based numbers (its own
  tokenizers, `tests/VerilogLanguageTest.cpp`). Coloring only, not a parser.
- **`TraceStore`** — global static store (same pattern) for the driver
  traces in the schematic: id, label (the start pin), palette color,
  visibility. A trace is registered when its `trace_driver` request is sent,
  and forgotten when the view is cleared (`GUIData::clearEquipotentials()`).
- **`ShowNetStore`** — global static store (same pattern) for the nets a
  host pushed with `show_net` (see Wire protocol): turns a push into a
  tagged `load_equipotential`, or `trace_driver` for `"trace": true`
  (`request()`), maps its reply's `trace_id`
  (`takeReply()`: re-registers a trace a focus cleared in flight, -1 for a
  forgotten net), and `forget()`s them when the view is cleared.
  `tests/ShowNetStoreTest.cpp`.
- **`TraceView`** — the "Traces" bottom-panel tab: one row per trace, to
  show/hide it, change its color or remove it. Clicking the color swatch
  opens a popup (palette swatches, a color picker, Reset); a picked color
  is stored as `Trace::customColor` over the palette `style`, and
  `TraceStore::setColor()` refuses a near-black one (the convergence
  color, so shared gates stay unambiguous) and a near-white one (invisible
  on the white canvas).
- **`ReviewStore`** — global static store (same pattern) for the review
  threads the host last pushed (`review_response`; see "Reviews" under Wire
  protocol): parsed threads, `writable`/`author`, the last refused write.
  `instanceThreads()`/`termThreads()`/`netThreads()` match an object by
  ids only (`id_path` + term/net id, optional bit), and only threads the
  host resolved `exact`; `openCount()`/`openColor()` give the badges.
  Builds the `review_add_thread`/`review_add_event` requests and their
  anchors. Cleared on every fresh `root_response`/`root_loaded` (the host
  pushes the review again after it).
- **`ReviewView`** — the "Review" bottom-panel tab (open/all filter; a
  thread's comments, Reply, Resolve/Won't Fix/Reopen, Go To for an exact
  one, the snapshot id for one on another netlist). Clicking an exact
  thread (or Go To) runs `ReviewView::goTo()`: it selects the thread's
  instance and leaves a `GoTo` that `appFrame()` takes next frame, to
  `NetlistTree::reveal()` the object with a `RevealLeaf` (on into the
  instance's Terms/Nets group to the term, bus bit or net row, highlighted)
  and `EquipotentialView::focusObject()` it: pan to the box or pin if
  drawn (a pin flashes), else add it without clearing the view -- a pin
  with a bit by `load_equipotential`, an instance (or an instance's term
  with no bit, maybe a whole bus) by `resolve_instance`, whose
  `instance_resolved` `takeAddOnResolve()` marks as added rather than
  replacing the view -- then pan once it's drawn. Net threads go to the
  tree only.
  and the modal Add Comment dialog (`openComposer()`, drawn by
  `renderComposer()` at the top level each frame), opened by
  `commentMenuItem()` from the tree's instance/term/net menus and the
  schematic's box and pin menus -- only shown when the host is writable.
  The tree shows `[n]` open threads after a row, the schematic a numbered
  disc on a box's corner (`InstanceShape::reviewOpen`) and next to a pin
  or on a top-level port's flag with threads on its term (`Port::reviewOpen`,
  `ReviewStore::pinThreads()`: that bit's threads plus the whole term's; a
  merged bus pin, every bit's), drawn through the painter. Hovering a tree
  row or a disc lists them; clicking a disc opens one in the Review tab
  (`ReviewView::showThreads()`, `SchematicView::reviewBadges()` hit-tests
  the drawn geometry). Net threads have no schematic badge: a wire tree is
  an equipotential across the hierarchy, which no net id names.
- **`StyleStore`** — global static store (same pattern) for the host's
  `style_response` (see Wire protocol): items indexed by instance path
  per kind, merged in push order; `instanceStyle()`, `termStyle()`,
  `netStyle(const Equipotential&)`. Queried by `EquipotentialView` while
  building shapes (so styles reach SVG exports), cleared on every fresh
  `root_response`/`root_loaded`. `tests/StyleStoreTest.cpp`.
- **`SelectionStore`** — global static store (same pattern) for the one
  selection: an instance, by `InstancePath` (`{}` = top), or a schematic
  net, by `SchematicNetRef` (the pin its wire tree starts from: instance
  path + pin name, or a top-level port name) -- selecting one drops the
  other. An instance is set from the tree, the schematic or a host
  `focus_instance`, and drawn highlighted in both views. A net is set by
  clicking its wires in the schematic (`SchematicInteraction::pickWire`,
  tried after pins and before box bodies) and drawn with a halo
  (`NetRoute::selected`); it's view-only, not reported to the host. Only
  routed (top-level) wires are selectable, not wiring nested in an
  expanded instance. Its listener (installed in `AppLogic.cpp`) reports
  each instance change to the host. `NetlistTree` reveals selections made outside it by comparing
  `revision()`. Cleared on every fresh `root_response`/`root_loaded`.
- **`SchematicLayout`** — the schematic's pure geometry, split out of
  `EquipotentialView` so it's unit-testable without ImGui frames or a
  provider (`tests/SchematicLayoutTest.cpp`). It follows the classic
  schematic-generation pipeline and is recomputed from scratch every frame (deterministic, so
  nothing moves unless the inputs change):
  - `symbolGeometry()`: symbol sizes, and pins on a `kPinPitch` grid (gate
    inputs centered on the output, box pins down from the top).
  - `layeredPlacement()`: Sugiyama-style placement. It breaks cycles, assigns
    longest-path levels, orders each column by barycenter, then places heights
    by isotonic regression so connected pins line up (straight wires).
  - `stackPorts()`: top-level ports on the sheet's left/right edges.
  - `routeNets()`: one orthogonal tree per driving pin. Trunks go in the
    box-free channels between columns, on distinct tracks that respect
    channel routing's vertical constraints, plus a spine that detours around
    boxes, labels and other spines. Junction dots mark real branch points.
  - `layoutHierarchyGroups()`: module frames.

  Changes to how the schematic is laid out belong here, with a test;
  `EquipotentialView` only turns the result into drawn shapes, and
  `SchematicView` draws them. A gate symbol's body fills its box exactly, so
  pin ticks always touch it. Pin names go inside boxes, none on gates, and
  instance names above. An instance name is part of its symbol's footprint
  (`footprintWidth()`, `nameRect()`): columns, module frames, the sheet's
  edges and the fit bounds make room for it, and it's a routing obstacle,
  so it never overlaps a box, another name or a wire. The view measures it
  with its font (`instanceNameWidth()`); past `kNameMaxW` it's shortened in
  the middle (`shortenMiddle()`), and layout and drawing use the same text.
- **`SchematicPainter`** — what `SchematicView` draws with: the subset of
  `ImDrawList` it uses, same names and argument order.
  `ImDrawListPainter` paints on screen. `SvgPainter` (`SvgPainter.cpp`)
  writes the same drawing as SVG, keeping text as `<text>` pinned to the
  ImGui-measured width (`textLength`). `SchematicView::exportSvg()` draws a
  copy of the view at 1x over the whole sheet, with `exporting` set to leave
  out hover, pending pins and the hierarchy toggle; the selected instance or
  net is kept (same outline/halo as on screen). At 1x no
  label or pin fades out. **File > Export Schematic as SVG...** saves it
  (native: `NativeFileDialog::saveFile()`; WASM: a Blob download,
  `najaDownloadText` in `AppLogic.cpp`). New drawing code goes through
  the painter, never straight to an `ImDrawList`, or it won't reach exports.
- **`SchematicInteraction`** — the pure side of the schematic's pin
  interactions: `pickPin()` (nearest-pin hit test, radius capped in world
  units so a pin doesn't swallow box-body clicks at low zoom) and
  `PendingRequests` (in-flight pin requests, which expire after 5 s).
  `tests/SchematicInteractionTest.cpp` tests them. The click flow itself is
  covered end to end by `tests/EquipotentialViewTest.cpp`. That test drives
  the real `EquipotentialView::renderSchematic()` in a headless ImGui context
  with no render backend: it moves the mouse and clicks through the ImGui IO
  API, then asserts on what `FakeNetlistProvider` received and on the
  geometry, read through `EquipotentialView::schematicForTesting()`. Use the
  same pattern for new canvas interactions.

### Diagnosis overlay

Once `DiagnosisStore` holds data, three places render it — no polling, they
just query the store each frame:
- `NetlistTree` (`NetlistTreeInstanceNode::getColor()`/`getDiagnostics()`) —
  colors the tree label and shows a hover tooltip for a flagged instance.
- `EquipotentialView`/`SchematicView` — tints a flagged instance's box
  outline (`InstanceShape::diagOutline`), a flagged pin's dot
  (`Port::color`), and a wire whose endpoint pin is flagged (`NetWire::color`
  inherits the pin's color); hovering an instance box shows the same tooltip
  as the tree.
- `DiagnosisView` — the flat list, independent of what's currently expanded/
  loaded in the tree or schematic.

Path matching convention: `NetlistTreeInstanceNode::getInstancePath()`
and `InstanceShape::path` are the same `InstancePath` (ids + names,
compared by id). A `DiagnosisItem` matches it by `idPath` when it has one,
else by its name `path` (see `DiagnosisStore::findInstanceItems()`).
`get_properties`, `instance_selected` and `resolve_instance` send the path
as `path` + `id_path`, and `expand_instance_terms`/`load_instance_internals`
tag their request with it as `instance_path` + `instance_id_path` (echoed
back in the reply, so the view knows which box it's for -- two anonymous
sibling boxes differ only by id).

### User controls documentation

Every user-facing control -- keyboard shortcut, bare canvas key, mouse
action -- is documented in **two places that must be kept up to date with
the code in the same change**: the in-app **Help > Keyboard Shortcuts**
window (`renderShortcutsWindow()` in `AppLogic.cpp`) and the "Controls"
section of the top `README.md` (`python/README.md` only links to it). When
adding, removing or changing a control, update both. The code they
describe: `handleShortcuts()` (global Ctrl/Cmd chords, labeled in the
View menu with `NAJA_SHORTCUT()`, Cmd on native macOS since ImGui maps
`ImGuiMod_Ctrl` to Cmd there), `SchematicView::handleInteraction()` (bare
canvas keys, ignored while Ctrl/Cmd/Alt is held, and wheel/drag), and the
canvas clicks in `EquipotentialView::renderSchematic()`.

### VSCode integration

`.vscode/settings.json` locks `cmake-tools` to preset mode
(`native-debug`), pointing IntelliSense at
`../naja-schematic-build/native-debug/compile_commands.json`.

### Licensing (REUSE)

Files carry no license headers: `REUSE.toml` lists every file by path
(explicitly, not `**`, so third-party exceptions aren't double-annotated),
and `.github/workflows/reuse.yml` runs `reuse lint`. **Add every new
file to the right `[[annotations]]` list in the same change**, or CI fails
with "no copyright and licensing information". Check locally with
`pip install "reuse[charset-normalizer]" && reuse lint`.

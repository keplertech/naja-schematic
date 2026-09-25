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

Clone with submodules (`thirdparty/imgui`, `thirdparty/naja`) — if already cloned
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
`.v` → Verilog, `.sv` → SystemVerilog via slang, otherwise treated as an SNL
directory), `--liberty <path>` (repeatable, once per file — defines the
primitive cell library for a `.v` design; not supported for `.sv`, since
`LocalSNLProvider::loadSystemVerilog()`/`SNLSVConstructor` has no liberty
hook, and the CLI fails loudly rather than silently ignoring it), and/or a
diagnosis JSON to pre-load (`--diagnosis <path>`, any order, all optional —
see `loadDiagnosisFile()` in `AppLogic.cpp`):

```bash
../naja-schematic-build/native-debug/naja-schematic-standalone [path/to/netlist] [--liberty path/to/cells.lib]... [--diagnosis path/to/diagnosis.json]

# gate-level Verilog + Liberty example:
../naja-schematic-build/native-debug/naja-schematic-standalone design.v --liberty cells.lib
```

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
`build_arg_parser()` in `python/naja_schematic/server.py`;
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

A design is required: either `--verilog`, or SystemVerilog via
`--systemverilog`/`--sv <path>...` and/or `--flist`/`-f <file>` (the two SV
inputs can be combined; `--flist` maps to `SystemVerilogConfig.flist`).
Verilog and SV are mutually exclusive. `--top`/`--define`/`-D` apply only to
SystemVerilog, and `--liberty` is rejected with SV — same restriction as the
native CLI, since the SV loader has no liberty hook.
`--allow_unknown_designs` maps to `SystemVerilogConfig.blackbox_unknown_modules`
for SV.

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
- `server.py` — the `naja-schematic` CLI (same flags as above, plus `--host`,
  `--open`, `--diagnosis <json>`, `--stdio`) and its two transports: a
  WebSocket server that also serves the viewer page on the same port
  (`static/index.html` sets `Module.najaWsUrl` from `location`), and JSON
  lines on stdin/stdout for a host that relays messages itself. In `--stdio`
  mode fd 1 is pointed at stderr (naja's C++ logger writes to stdout) and
  protocol output goes to a private dup of the original stdout. With
  `--diagnosis`, a `diagnosis_response` is pushed after every
  `root_response` (the viewer clears diagnoses on each root load).
- `widget.py` — `naja_schematic.show()`: an anywidget for Jupyter/Colab/
  VSCode notebooks. Its ES module is the bundle + `static/widget.js`; the
  viewer's requests come back over the widget comm channel as
  `{"json": "<message>"}` and are answered in the kernel, so the view shows
  the netlist as edited by earlier cells. `Schematic.annotate(items)`
  pushes diagnoses.

The viewer bundle `static/naja-schematic.js` is **not** checked in: it's the
WASM target configured with `-DNAJA_SCHEMATIC_WASM_MODULE=ON` (single file,
wasm inlined, `createNajaSchematic({canvas, ...})` factory, one instance per
canvas), built by `.github/workflows/python-package.yml`, which also tests
the wheel and publishes it to PyPI on a `python-v<version>` tag (version in
`naja_schematic/__init__.py`). For local work set `NAJA_SCHEMATIC_BUNDLE` to
a locally built bundle (see `_bundle.py`). Tests: `pytest python/tests`.

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

`src/AppLogic.h/.cpp` holds the logic shared by both entry points:
- `setupProvider(AppState&)` wires all provider callbacks and calls
  `provider->start()`. All response-message dispatch (`root_response`,
  `instances_response`/`primitives_response`/`children_loaded`,
  `terms_response`, `nets_response`, `equipotential_response`,
  `expanded_instance_terms`, `error`, ...) lives here, driven by the
  `"response"` field of incoming JSON —
  this is the one place to look when tracing how a server/provider reply turns
  into UI state.
- `appFrame(AppState&)` runs one ImGui frame (poll events, build UI, render)
  and returns `false` on quit.

### Wire protocol

Requests/responses are JSON with a `"request"`/`"response"` type field (e.g.
`load_root`, `load_instance`, `load_primitives`, `load_terms`, `load_nets`,
`load_equipotential` → `*_response`). Both `LocalSNLProvider` (native,
`buildRootResponse()`/`buildInstancesResponse()`/etc.) and
`python/naja_schematic/protocol.py` (WASM/browser/notebook) must independently implement this same
protocol — when changing one side, check the other.

`load_nets`/`nets_response` mirrors `load_terms`/`terms_response` exactly
(same `gui_id`/`design_ref` request shape, same bus-vs-scalar `msb`/`lsb`
response shape, lazily populating a `NetlistTreeGroupNode::Type::Nets` group
next to `Terms` under an instance node) with one deliberate asymmetry: a net
entry carries no `child_id` and offers no "Show Equipotential" action — a
net has no direction, and get_properties identifies it by name (`"net"`
field) rather than by the provider-specific numeric id a term's
`load_equipotential` request needs. See `NetlistTreeNetNode`/
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
each to `GUIData` in order; the incremental layout in `EquipotentialView.cpp`
relies on that ordering (each net shares an already-placed instance with an
earlier one) to chain the cone right-to-left. Reachable from the tree
(right-click a term/bus-bit row -> "Trace to Driver", bus row -> "Trace Bus
to Driver"; both clear the view first, like "Show Equipotential") and from the
schematic (right-click a pin -> "Trace to Driver"; this one *adds* to the view
instead of clearing it).

Occurrence `path` entries (in `equipotential_response` and each
`trace_driver_response` net) are `[name, child_id, model_name]`; the third
element is optional on parse (`InstTermOccurrence::pathModels`, `""` when
absent). It lets the schematic keep the design hierarchy of whatever it
shows (**View > Show Hierarchy**, on by default, also in the canvas context
menu): `layoutHierarchyGroups()` in `EquipotentialView.cpp` draws every
module enclosing a displayed leaf as a nested translucent frame labelled
`instance (Model)`, re-laying the leaves out inside it — each leaf keeps the
logic column the incremental layout gave it, and inside a frame its leaves
and sub-frames are bucketed into columns by that column. Frames are
`InstanceShape`s with `isHierGroup` set, inserted at the front of
`SchematicView::instances` and drawn before the nets
(`SchematicView::render()`); the leaves stay top-level shapes, so wiring and
hit-testing are unchanged. Right-clicking a frame offers Show Properties and
"Zoom to Module". The bottom "Equipotential" table lists every net of the
last trace (not just the last net) with a "Hierarchy" column giving each
occurrence's enclosing modules.

`diagnosis_response` is different: it's a **server push**, not a reply to a
request (a diagnosis run finishes on its own schedule), and it *annotates*
the already-loaded netlist rather than loading anything:

```json
{
  "response": "diagnosis_response",
  "items": [
    {
      "kind": "instance",          // "instance" | "net"
      "path": ["u1", "u2"],        // instance-name path, root excluded; [] = top level
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
`DiagnosisItem` parser) instead.

`get_properties`/`properties_response` is a general name/value inspector for
whatever object the UI asks about — an instance (including the top design
itself), a term/pin, or a net — answered by both `LocalSNLProvider`
(`buildPropertiesResponse()`) and `protocol.py` the same request/
response way as `load_terms` etc. (unlike `diagnosis_response`, it's not a
push). The object is identified the same way `DiagnosisItem` identifies
things — a slash-joined instance-name path, root excluded — rather than
provider-specific numeric `child_id`s, so both backends resolve it by
walking instance names down from the top design:

```json
// request
{
  "request": "get_properties",
  "kind": "instance",        // "instance" | "term" | "net"
  "path": ["u1", "u2"],      // instance-name path; "instance": path to the object itself ([] = top design);
                              // "term"/"net": path to the *containing* instance ([] = a top-level port/design net)
  "terminal": "Q",           // "term" only: pin/port base name, no bus-bit suffix
  "net": "internal_bus",     // "net" only: net base name, no "[bit]" suffix
  "bit": 3                   // "term"/"net" only, optional: a specific bus bit
}
// response
{
  "response": "properties_response",
  "subject": "u1/u2",        // human-readable label for the object, shown as a heading
  "properties": [ {"name": "Name", "value": "u2"}, {"name": "Model", "value": "AND2"}, ... ]
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

Path matching convention: `DiagnosisItem::pathKey()` (slash-joined instance
names, root excluded) must match `NetlistTreeInstanceNode::getPathKey()` and
`EquipotentialView`'s instance-item keys — all three are built the same way,
from instance *names*, not the provider's numeric `child_id`s (those aren't
stable inputs for an external tool like kepler-formal to reference).
`get_properties` reuses this same path/pathKey convention (`splitPathKey()`
in `Types.h` is the inverse of `pathKey()`) so its request-building code in
`NetlistTree.cpp`/`EquipotentialView.cpp` and its resolution code in
`LocalSNLProvider.cpp`/`protocol.py` need no id/name translation layer
of their own.

### VSCode integration

`.vscode/settings.json` locks `cmake-tools` to preset mode
(`native-debug`), pointing IntelliSense at
`../naja-schematic-build/native-debug/compile_commands.json`.

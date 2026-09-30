# naja-schematic

[![Native macOS Build](https://github.com/najaeda/naja-schematic/actions/workflows/native-macos.yml/badge.svg?branch=main)](https://github.com/najaeda/naja-schematic/actions/workflows/native-macos.yml)
[![Native Linux Build](https://github.com/najaeda/naja-schematic/actions/workflows/native-linux.yml/badge.svg?branch=main)](https://github.com/najaeda/naja-schematic/actions/workflows/native-linux.yml)
[![Emscripten Build](https://github.com/najaeda/naja-schematic/actions/workflows/emscripten.yml/badge.svg?branch=main)](https://github.com/najaeda/naja-schematic/actions/workflows/emscripten.yml)
[![Open In Colab](https://colab.research.google.com/assets/colab-badge.svg)](https://colab.research.google.com/github/najaeda/naja-schematic/blob/main/python/notebooks/colab_test.ipynb)


> **Note:** This is a proof of concept in beta. Expect rough edges, breaking
> changes, and incomplete features.

A C++20, [Dear ImGui](https://github.com/ocornut/imgui)-based netlist
viewer/schematic browser for the [naja](https://github.com/najaeda/naja) SNL
netlist data model. It builds to two targets from the same core sources: a
native desktop app and a WASM app that runs in a browser or a VSCode webview.

![naja-schematic demo: tracing an output port back to its drivers, then extending the schematic pin by pin](docs/demo.gif)

## Features

- Interactive hierarchical schematic and equipotential (net fan-out) views
- Lazy-loaded design tree (instances, terms, bus bits) for large netlists
- Diagnosis overlay: color-codes and annotates flagged instances/pins/nets
  from a `diagnosis_response` JSON payload, with hover tooltips and a flat
  diagnostics list panel
- Loads Verilog, SystemVerilog, or pre-built SNL netlists natively; the
  browser build talks to a Python netlist server over WebSocket
- Runs natively on macOS or Linux, or anywhere with a browser/VSCode via WASM

## Building

Clone with submodules:

```bash
git clone --recursive https://github.com/najaeda/naja-schematic.git
# or, if already cloned without them:
git submodule update --init --recursive
```

### Native standalone target (`naja-schematic-standalone`)

Builds on macOS and Linux. Requires SDL2 and OpenGL, plus a native file-picker
backend (Cocoa on macOS, `zenity` on Linux — see `CLAUDE.md` for the full
Linux dependency list).

On macOS:

```bash
brew install sdl2
```

Build with CMake presets, which pin the compiler to Apple Clang so
Emscripten on `PATH` doesn't get auto-detected:

```bash
cmake --preset native-debug      # or native-release
cmake --build --preset native-debug
```

The build directory lives outside the repo, at
`../naja-schematic-build/<preset>`.

On Linux, invoke CMake directly instead (the presets are macOS-specific):

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

Run (`../naja-schematic-build/native-debug/naja-schematic-standalone` on
macOS, `build/naja-schematic-standalone` on Linux), optionally with a
netlist to load (dispatched by extension — `.v` → Verilog, `.sv` →
SystemVerilog, otherwise treated as an SNL directory), one or more
`--liberty` files defining the primitive cell library for a `.v` design, and/or
a diagnosis JSON to pre-load:

```bash
naja-schematic-standalone [path/to/netlist] [--liberty path/to/cells.lib]... [--diagnosis path/to/diagnosis.json]
```

`<netlist>`, `--liberty <path>` and `--diagnosis <path>` are all optional and
order-independent. `--liberty` is repeatable (once per file) and only
supported for `.v` designs — a gate-level Verilog netlist that instantiates
Liberty std cells needs it to resolve them; it errors out for `.sv`, since the
SystemVerilog frontend has no liberty hook. Without `--diagnosis`, use File >
Load Diagnosis JSON... in the app instead.

### WASM target (`naja-schematic`)

Requires [Emscripten](https://emscripten.org/) (`emcmake`/`emrun` on `PATH`).

```bash
mkdir build-wasm && cd build-wasm
emcmake cmake ..
cmake --build .
emrun --port 8080 naja-schematic.html
```

The WASM app doesn't link naja directly — it talks to a netlist server over a
WebSocket. Start the server first, optionally pointing it at a Verilog
netlist and the Liberty files defining its cell library:

```bash
python3 scripts/najaeda_server.py   # serves ws://localhost:8081/ws
# or, to serve a gate-level design instead of the default canned one:
python3 scripts/najaeda_server.py --verilog path/to/netlist.v --liberty path/to/cells.lib
```

Note the flag shapes differ from the native CLI above: the server takes
`--verilog <path>` (single file) plus `--liberty <path> [<path>...]` (one
flag, many files), while the native standalone takes a positional
`<design>` plus a repeatable `--liberty <path>` (once per file) — the two
modes aren't symmetric.

## Architecture

The app has two entry points (`src/main_native.cpp`, `src/main_wasm.cpp`)
that share almost everything except how netlist data is sourced, behind an
`INetlistProvider` interface. Requests/responses between the UI and a
provider are JSON messages over a small request/response protocol; a
`diagnosis_response` message is a server push that annotates the currently
loaded netlist without loading anything new.

See [`CLAUDE.md`](CLAUDE.md) for the full architecture writeup, including the
wire protocol's message shapes and the diagnosis overlay's data flow — it
doubles as the guide used by AI coding agents working in this repo.

## Status

This project is under active development as part of an evolving AI
RTL-diagnosis workflow. Interfaces (in particular the wire protocol and the
`diagnosis_response` shape) may still change.

## Reporting Issues

This project is beta — if something breaks, crashes, or looks wrong, please
[file a GitHub issue](https://github.com/najaeda/naja-schematic/issues/new).
Before reporting, check for error output in the relevant places:

- **Native build**: the terminal you launched `naja-schematic-standalone`
  from.
- **WASM build**: there are *two* separate places to check, since a WASM
  session has two live processes —
  - the terminal running `python3 scripts/najaeda_server.py` (netlist
    parsing/serving errors surface here), and
  - the browser's JavaScript console (Cmd+Opt+J in Chrome, Cmd+Opt+K in
    Firefox; in VSCode's webview, use "Developer: Open Webview Developer
    Tools" from the command palette) — this is where WASM runtime errors,
    WebSocket issues, and rendering problems show up.

Please include the relevant console output (both, for WASM issues), the
netlist/diagnosis files involved if possible, and steps to reproduce.

## License

Apache License 2.0 — see [LICENSE](LICENSE).

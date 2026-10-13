# Source information in naja-schematic

How the viewer shows where a netlist object comes from: its declaration
in the loaded input and, for synthesized logic, the RTL it was produced
from. It is built on the source references of naja 0.7.29
(`docs/source_information_design.md` in naja). Requires
**najaeda >= 0.7.29**.

## What naja provides

Each `SNLDesign` and design object (instance, term, net, bus bit) can carry
`SNLRTLInfos` holding:

- an optional **declaration**: where the object is written in the input
  that was loaded. For an instance, that's its instantiation; for a design,
  its definition; for a gate in a gate-level netlist, its line in that
  netlist.
- ordered **origins**: where it came from upstream, e.g. the RTL ranges of a
  Yosys `src` attribute (several when logic was merged).
- the **legacy location** (`getSourceLoc()`), an older, unclassified slot.

What fills them in najaeda 0.7.29:

- **SystemVerilog** (slang): declarations for module definitions, ports,
  named nets and instantiations; origins for lowered logic. Provider
  `slang`, language `systemverilog`, representation `rtl`. The legacy slot
  is still written too, and the viewer hides it when typed references exist.
- **Gate-level Verilog**: every design, instance, term and declared net
  gets its netlist declaration (provider `naja-verilog`, language
  `verilog`). The spans are partial: a module's keyword and name, an
  instance's name, an anonymous gate's type. With Yosys decoding turned on
  (`yosys_source_attributes`, off by default, see below), each
  `(* src = "f:l.c-l.c|..." *)` attribute becomes ordered origins (provider
  `yosys`). Origins can point at technology/model files as well as RTL.
  Bus bits carry no reference of their own.
- **NajaIF** snapshots keep all of it, so a design loaded natively from an
  SNL directory shows the same sources.
- **VHDL** doesn't fill anything yet.

A reference is a range (file, 1-based line and byte column, inclusive end;
0 means unknown) plus three labels: `provider` (e.g. `slang`, `yosys`),
`language` (e.g. `systemverilog`, `verilog`) and `representation` (`rtl`,
`gate`). An empty label means unknown. The viewer shows labels as given and
never infers them, from a file extension or anything else. A file path is
an opaque string from whatever produced the reference.

## Wire protocol

Both backends implement it: `python/naja_schematic/protocol.py` and
`src/LocalSNLProvider.cpp`. The shared cases in
`tests/data/source_protocol_cases.json` run against both.

### `has_sources` flags

Instance entries (`instances_response`, `primitives_response`, `root`,
`instance_internals_response` children, equipotential occurrences,
`instance_resolved`'s `instance`), term entries (`terms_response`) and net
entries (`nets_response`) carry `"has_sources": true` when there is
something to show. Like `clock`, the flag is only sent when it is true:

- an instance: its own references, or its model's declaration;
- a bus term or net: the bus itself or any of its bits (a bit row
  inherits the flag).

The flag replaces the old `source_loc` object. Sources are fetched on
demand rather than inlined, because a synthesized gate can have many
origins and a trace reply holds up to 500 nets.

### `get_sources` → `sources_response`

The object is addressed exactly as in `get_properties`: `kind`
(`instance`, `term` or `net`), the instance (for an instance) or the
containing instance (for a term or net) as `id_path` and/or `path`
(`[]` is the top), plus `terminal`/`net` and an optional `bit`.

```json
{ "response": "sources_response",
  "subject": "u_alu/g0",
  "kind": "instance", "path": ["u_alu", "g0"],      // the request's address, echoed
  "sources": [
    {"role": "declaration", "file": "gate/alu_syn.v", "line": 4, "column": 3,
     "end_line": 4, "end_column": 39,
     "provider": "", "language": "verilog", "representation": "gate"},
    {"role": "origin", "index": 0, "file": "rtl/alu.v", "line": 2, "column": 14,
     "end_line": 2, "end_column": 24,
     "provider": "yosys", "language": "", "representation": ""} ] }
```

Entries come in this order:

1. `declaration`;
2. each `origin`, in naja's order, numbered by `index`;
3. for an instance, `model_declaration`: the definition of its model;
4. `legacy`, only when the object has no declaration and no origins.

The top design (`kind: "instance"`, `[]`) lists its own definition. A bus
bit with no references of its own lists its bus's. An object that doesn't
resolve, or has no source information, gives an empty `sources` list, not
an error.

### `load_source` → `source_response`

The request names one entry of an object: the same address as
`get_sources`, plus `role` (and `index` for an origin). The host finds the
file from the design itself, so **the viewer never sends a path**. A
`load_source` carrying only a `file` reads nothing. The old protocol let
any connected viewer read any file on the host. Now only files the
design's references name can be read.

```json
{ "response": "source_response", "found": true,
  "role": "origin", "index": 0, "file": "rtl/alu.v", "line": 2, ...,  // the entry
  "text": "...", "first_line": 1, "truncated": false }
```

- **Finding the file**: first, each *source map* (FROM, TO) whose FROM
  starts the path at a path boundary (`/a/syn` maps `/a/syn/x.v`, not
  `/a/synth/x.v`) rewrites it, in order, and is used if a file is there.
  Then an absolute path is used as is, and a relative one is looked up under
  each *source root* in order, then in the host's current directory.
  Nothing is canonicalized. `found: false` (the entry is still echoed)
  means the file isn't there.
- **Large files**: a file up to 4 MiB is sent whole. Past that (typically a
  big gate-level netlist), only the lines within 500 of the reference are
  sent: `truncated: true`, and `first_line` gives the file line number of
  the first line in `text`. Bytes that aren't valid UTF-8 are replaced.

Source roots are given as:

- native: `naja-schematic-standalone --source-root <dir> --source-map FROM=TO`
  (both repeatable);
- the `naja-schematic` CLI: the same two options;
- Python: `source_roots=` and `source_maps=[(FROM, TO), ...]` on
  `ViewerSession`, `ViewerServer` and `naja_schematic.show()`.

On `scripts/tinyrocket.v`, `--source-map /home/xtof=/Users/xtof` takes a
flop such as `buffer.Queue._T_1$_SDFFE_PP0P_` to its
`always @(posedge clock)` block, line 138,804 of the 20 MB tinyRocket RTL.

Yosys `src` decoding is opt-in, as in naja, which never guesses provenance
from a banner, file name or attribute name:

- native: `naja-schematic-standalone design.v --yosys-src`, or the
  "Decode Yosys src attributes" checkbox in File > Open Verilog...;
- the `naja-schematic` CLI: `--verilog design.v --yosys-src`;
- notebooks: load with `VerilogConfig(yosys_source_attributes=True)`
  (`config.yosys_source_attributes = True`) before `show()`.

## User interface

- **Show Sources** is a context-menu entry on flagged rows of the tree
  (instances, terms, bus bits, nets, net bits) and on schematic boxes.
  Module frames always offer it, since no flag is sent for them.
- The **Source** tab (`SourceView`, data in `SourceStore`) lists the
  references, one row each: role, `file:line:col`, representation,
  language and provider ("-" for unknown). The first reference's file
  opens right away. Clicking another row opens that one.
- The file is shown in a read-only code editor (ImGuiColorTextEdit, as a
  submodule) in a monospace font (Cousine), with Verilog/SystemVerilog
  coloring, find (Ctrl+F, Ctrl+G), selection and copy. It scrolls to the
  reference once, highlights its exact byte-column span (from the start
  column on the first line to the end column on the last; an unknown end
  is not made up, so the span stops at the end of the start line), marks
  its lines in the gutter, and marks the object's other references into
  the same file in another color. In a windowed large file, line numbers
  stay those of the file.
- Native builds add **Open in Editor**: it runs `$NAJA_SCHEMATIC_EDITOR`
  with `{file}`, `{line}` and `{col}` filled in, on the absolute path the
  host read (`source_response`'s `path`). Unset, it uses VS Code at the line
  (`code -g` on PATH, else the CLI inside the installed app, which macOS
  doesn't put on PATH by default), else `open`/`xdg-open`. The result or
  error shows next to the button. It is navigation: edits made there reach
  the viewer only once the design is loaded again.
- **Stale files.** naja records ranges when the design is loaded, but the
  viewer reads files later, so a file edited, checked out or re-synthesized
  in between no longer matches them. Each host stamps when its design was
  loaded (native: every load; Python: `ViewerSession` start and each
  `design_changed()`), and a found `source_response` carries `"stale": true`
  when the file's modification time is later. The Source tab then warns
  that the highlighted lines may be off until the design is loaded again.
  naja keeps no file hashes, so this is a modification-time check.
- The bottom panel grows to 45% of the window height when "Show Sources"
  answers, once, and only if it was smaller.
- Everything is cleared when a new root loads, since the references name
  objects of the old design.

## Tests

- `tests/data/source_protocol_cases.json`, with `tests/data/sources/`: a
  Yosys-style gate netlist `gate/alu_syn.v` whose `(* src *)` attributes
  point into `rtl/alu.v` (present) and `rtl/top.v` (missing). Both runners
  load it with naja's own Verilog loader, Yosys decoding on, so the cases
  check what naja really records: netlist declarations, ordered origins,
  an anonymous gate's type span, an origin with no end, bus bits falling
  back to their bus, a model definition. Run by
  `python/tests/test_source_references.py` and
  `tests/provider/LocalSNLProviderProtocolTest.cpp`.
- Python only: the flag and legacy fallback (stand-in objects: the raw
  Python API is read-only), source-root order, the large-file window, the
  session and CLI plumbing.
- C++ only: legacy fallback (`setSourceLoc`), the large-file window.
- `tests/SourceStoreTest.cpp`: the `load_source` request built from the
  list, which row is shown, and highlight spans (single line, multi-line,
  unknown end, windowed file, clamping).

## Next steps

1. **Hover**: show a box's first origin (`alu.v:42`) in the schematic
   tooltip, so you can see which RTL line made a gate without opening
   anything. This needs a compact origin summary inline, or a cached
   `get_sources` per hovered box.
2. **Source history**: several files open, with back/forward.
   **VSCode webview**: "Open in Editor" as a message to the extension.
3. **Source → netlist**: click a source line and highlight the objects
   whose declaration or origins cover it. The host needs a file/line →
   objects index, built once per design generation.
4. **Diagnosis items with source references**: once kepler-formal reports
   point at RTL, a `diagnosis_response` item could carry a source reference
   that opens directly.

## Dependencies on naja

naja 0.7.29 closed what this needed: SV declarations and origins, gate-level
declarations, Yosys `src` decoding, NajaIF persistence, an allocation-free
`hasSourceReferences()` in raw Python (used for the `has_sources` flags),
and wheels that build from a git checkout. Still open there:

- **VHDL** writes no references yet.
- **Preprocessed Verilog** (`preprocess_enabled`) gets no declarations,
  since the preprocessor keeps no source map. Yosys origins still work.
- **Path relocation**: recorded paths are kept as written; the viewer's
  `--source-map` maps them to a local checkout.

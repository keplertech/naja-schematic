"""Writes the recording script for the README demo video.

    python3 scripts/demo_script.py OUT_DIR
    naja-schematic-standalone scripts/tinyrocket.v \
        --liberty scripts/NangateOpenCellLibrary_typical.lib \
        --liberty scripts/fakeram45_64x32.lib --record OUT_DIR/script.json

writes OUT_DIR/demo.mp4 and a still per step (OUT_DIR/s<step>.png); see
src/DemoRecorder.cpp for the script format. Instances and terms are naja
ids in tinyrocket.v as loaded with those Liberty files (dcache=2245,
frontend=2429, gate _2219_=71): re-check them if the design or naja's id
assignment changes.
"""
import json
import sys

out = sys.argv[1] if len(sys.argv) > 1 else "."
blue, orange = "#4aa8ff", "#ffa000"
traceA = {"trace": {"path": [2245], "term_id": 2, "label": "dcache.auto_out_a_ready", "color": blue}}
traceB = {"trace": {"path": [2429], "term_id": 2, "label": "frontend.auto_icache_master_out_a_ready",
                    "color": orange}}
gates = [[i] for i in (71, 66, 79, 1675)]
shared = [[i] for i in (71, 66)]
steps = [
    {"do": [{"clear": True}, traceA], "camera": "fit", "duration": 2.6,
     "caption": "Trace to driver: a fan-in cone across modules"},
    {"camera": {"instances": gates, "margin": 0.25}, "transition": "move", "duration": 1.3,
     "caption": "Gate symbols from the Liberty cell functions"},
    {"duration": 1.8},
    {"do": [traceB], "camera": "fit", "transition": "crossfade", "duration": 0.8,
     "caption": "A second trace, overlaid"},
    {"duration": 1.4},
    {"camera": {"instances": shared, "margin": 0.15}, "transition": "move", "duration": 1.3,
     "caption": "Shared logic: wires striped in both colors"},
    {"duration": 2.0},
    {"do": [{"clear": True}, {"equipotential": {"path": [], "term_id": 1, "label": "reset", "color": blue}}],
     "camera": "fit", "transition": "crossfade", "duration": 0.8,
     "caption": "One net, followed through the hierarchy"},
    {"duration": 2.0},
    {"do": [{"clear": True}, {"equipotential": {"path": [], "term_id": 0, "label": "clock", "color": blue}}],
     "camera": "fit", "transition": "crossfade", "duration": 0.8, "caption": "4,000+ clock pins, summarized"},
    {"duration": 2.0},
    {"do": [{"clear": True}, traceA, {"select": [71]}],
     "camera": {"instance": [71], "margin": 2.5}, "transition": "crossfade", "duration": 0.8,
     "caption": "Select a gate..."},
    {"duration": 0.6},
    {"do": [{"sources": [71]}, {"panel": 0.5}],
     "camera": {"instance": [71], "margin": 2.5}, "transition": "crossfade", "duration": 0.5,
     "caption": "...and jump to its line in the netlist"},
    {"duration": 2.8},
    # Back to the first frame, so the video loops.
    {"do": [{"clear": True}, {"select": None}, {"panel": 0.999}, traceA],
     "camera": "fit", "transition": "crossfade", "duration": 0.9,
     "caption": "Trace to driver: a fan-in cone across modules"},
]
script = {"output": out + "/demo.mp4", "size": [1920, 980], "fps": 30, "ui_scale": 1.5, "theme": "dark",
          "left_width": 0, "schematic_share": 0.999, "caption_size": 38,
          "stills": [{"step": i, "path": f"{out}/s{i}.png"} for i in range(len(steps))],
          "steps": steps}
with open(out + "/script.json", "w") as f:
    json.dump(script, f, indent=1)

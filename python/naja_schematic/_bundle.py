"""Locating the viewer files shipped in static/.

naja-schematic.js is the WASM viewer built as a single-file, modularized
Emscripten bundle (-DNAJA_SCHEMATIC_WASM_MODULE=ON); release wheels carry it
in static/, but it is a build product and not checked in. For a development
checkout, point NAJA_SCHEMATIC_BUNDLE at a locally built one instead.
"""
import os
from pathlib import Path

STATIC_DIR = Path(__file__).parent / "static"
BUNDLE_NAME = "naja-schematic.js"

# The only files the HTTP server hands out (no path traversal to worry about).
_SERVED = {"index.html", BUNDLE_NAME}


def bundle_path():
    """Path of the viewer bundle, or None if this install has none."""
    override = os.environ.get("NAJA_SCHEMATIC_BUNDLE")
    path = Path(override) if override else STATIC_DIR / BUNDLE_NAME
    return path if path.is_file() else None


def static_file(name):
    if name not in _SERVED:
        return None
    if name == BUNDLE_NAME:
        return bundle_path()
    path = STATIC_DIR / name
    return path if path.is_file() else None

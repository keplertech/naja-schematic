# Development entry point for the netlist server the WASM viewer talks to.
# The implementation lives in the naja_schematic Python package
# (python/naja_schematic: protocol.py answers requests, server.py is this
# CLI); this wrapper runs it from a source checkout without installing it.
# Installed equivalent: `naja-schematic ...` or `python -m naja_schematic ...`.
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "python"))

from naja_schematic.server import main  # noqa: E402

if __name__ == "__main__":
    main()

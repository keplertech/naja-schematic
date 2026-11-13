"""Wheel build hook: a wheel carrying the headless SVG renderer.

naja_schematic/bin/naja-schematic-render[.exe] (render.py) is a native
executable, built by CI per platform (-DNAJA_SCHEMATIC_RENDER_ONLY=ON) and
gitignored like the viewer bundle. When it is there, the wheel includes it
and is tagged for the platform it was built for, named by
$NAJA_SCHEMATIC_WHEEL_PLATFORM (e.g. manylinux_2_28_x86_64,
manylinux_2_28_aarch64, macosx_11_0_universal2, win_amd64):
py3-none-<platform>, any Python 3 (it's a subprocess, not an extension
module). Without it the wheel stays pure
(py3-none-any): everything but export_svg() works there.
"""
import os
import sysconfig
from pathlib import Path

from hatchling.builders.hooks.plugin.interface import BuildHookInterface

# naja-schematic-render.exe on Windows.
RENDERERS = ("naja_schematic/bin/naja-schematic-render",
             "naja_schematic/bin/naja-schematic-render.exe")


class CustomBuildHook(BuildHookInterface):
    def initialize(self, version, build_data):
        if self.target_name != "wheel":
            return
        found = [r for r in RENDERERS if (Path(self.root) / r).is_file()]
        if not found:
            return
        renderer = found[0]
        platform = os.environ.get("NAJA_SCHEMATIC_WHEEL_PLATFORM") or \
            sysconfig.get_platform().replace("-", "_").replace(".", "_")
        build_data["pure_python"] = False
        build_data["tag"] = f"py3-none-{platform}"
        build_data["force_include"][str(Path(self.root) / renderer)] = renderer

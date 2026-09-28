"""naja-schematic: interactive schematic viewer for najaeda netlists.

- In a notebook (Jupyter, Colab, VSCode): ``naja_schematic.show()`` displays
  the design currently loaded with najaeda.
- From a shell: ``naja-schematic --verilog design.v --open`` serves the
  viewer page and opens it in a browser.
"""
# Single version for the whole project: CMakeLists.txt reads this line into the
# C++ app too (NAJA_SCHEMATIC_VERSION_STRING), so keep it a plain literal.
__version__ = "0.1.2"

from .protocol import diagnosis_response, handle_request


def show(instance=None, *, height=600, diagnosis=None):
    """Display a view of the loaded design in the current notebook; see
    naja_schematic.widget.show()."""
    # Imported lazily: the CLI/server path doesn't need anywidget/IPython.
    from .widget import show as _show
    return _show(instance, height=height, diagnosis=diagnosis)


__all__ = ["__version__", "show", "handle_request", "diagnosis_response"]

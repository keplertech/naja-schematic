"""naja-schematic: interactive schematic viewer for najaeda netlists.

- In a notebook (Jupyter, Colab, VSCode): ``naja_schematic.show()`` displays
  the design currently loaded with najaeda.
- From a shell: ``naja-schematic --verilog design.v --open`` serves the
  viewer page and opens it in a browser.
- From an application that already holds the design: ``ViewerServer``
  serves the viewer page from a background thread.
- With no browser or display: ``naja_schematic.export_svg()`` (and
  ``export_svgs()``) draws part of the loaded design to SVG, and
  ``export_png()`` to PNG, as does ``naja-schematic render --verilog
  design.v --trace u1:A --svg out.svg --png out.png --scale 3``.
"""
# Single version for the whole project: CMakeLists.txt reads this line into the
# C++ app too (NAJA_SCHEMATIC_VERSION_STRING), so keep it a plain literal.
__version__ = "0.2.1"

from .protocol import diagnosis_response, handle_request
from .render import RenderError, export_png, export_svg, export_svgs
from .review import Review
from .server import ViewerServer
from .session import style_response


def show(instance=None, *, height=600, diagnosis=None, style=None, review=None,
         view_options=None):
    """Display a view of the loaded design in the current notebook; see
    naja_schematic.widget.show()."""
    # Imported lazily: the CLI/server path doesn't need anywidget/IPython.
    from .widget import show as _show
    return _show(instance, height=height, diagnosis=diagnosis, style=style, review=review,
                 view_options=view_options)


__all__ = ["__version__", "show", "handle_request", "diagnosis_response", "style_response",
           "ViewerServer", "Review",
           "export_svg", "export_svgs", "export_png", "RenderError"]

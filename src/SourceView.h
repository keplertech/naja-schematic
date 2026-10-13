#pragma once

struct ImFont;
class INetlistProvider;

// The "Source" tab (see SourceStore): the source references of the object
// last asked about (Show Sources) -- each a row naming its role, location,
// representation, language and provider; clicking one asks `provider` for
// its file -- and below them the shown file in a read-only code editor
// (ImGuiColorTextEdit): monospace, Verilog/SystemVerilog coloring
// (VerilogLanguage), find (Ctrl/Cmd+F), selection and copy. The
// reference's range is highlighted and its lines marked, the object's other
// references into the same file are marked too, and the editor scrolls to
// the reference once per file loaded. Native builds also offer "Open in
// Editor" ($NAJA_SCHEMATIC_EDITOR).
class SourceView {
  public:
    // The monospace font the code is drawn with (setupFonts()).
    static void setFont(ImFont* font);
    static void render(INetlistProvider* provider);
};

#pragma once

#include "ImGuiColorTextEdit/TextEditor.h"

// Syntax coloring for the Source tab (SourceView): Verilog and
// SystemVerilog (IEEE 1364/1800) keywords, data/net types, `directive
// lines, // and /* */ comments, "strings", system tasks ($display) and
// escaped identifiers (\a/b ), and numbers with sizes, bases and
// unknowns (8'hFF, 4'b10xz, 'x, 1_000, 1.5e3). Coloring only: it is no
// parser, and it colors gate-level netlists and RTL alike.
const TextEditor::Language* verilogLanguage();

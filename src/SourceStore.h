#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Types.h"

// What the "Source" tab shows (see docs/source_information.md): the source
// references of the object last asked about (a sources_response, answering
// get_sources), and the text of one referenced file (a source_response,
// answering load_source) with the reference's range to highlight. Same
// static/global state pattern as DiagnosisStore/EquipotentialView. Cleared
// on every fresh root load: references name objects of the old design.
class SourceStore {
  public:
    // -- the object's references ------------------------------------------
    // `address` is the get_sources request's object address (kind,
    // path/id_path, terminal/net, bit), as echoed by the reply.
    static void setReferences(const std::string& subject, const json& address,
                              std::vector<SourceRef> references);
    static bool hasReferences();
    static const std::string& subject();
    static const std::vector<SourceRef>& references();
    // The load_source request reading references()[i]'s file.
    static json loadRequest(size_t i);
    // The entry of references() whose file is shown, if any.
    static std::optional<size_t> shownReference();

    // -- the shown file ---------------------------------------------------
    // `firstLine` is the file line number of text's first line (1 unless
    // the provider sent only the lines around a reference in a big file).
    // `hostPath` is where the netlist host read the file (source_response
    // "path"): for "Open in Editor" when the viewer runs on the same host.
    // `stale`: the file changed after the design was loaded, so the
    // reference's lines may not match it any more.
    static void setSource(const SourceRef& ref, const std::string& text,
                          std::uint64_t firstLine = 1, bool truncated = false,
                          const std::string& hostPath = {}, bool stale = false);
    // The file of `ref` couldn't be read on the netlist host.
    static void setMissing(const SourceRef& ref);
    static bool hasSource();
    static const SourceRef& shown();  // the reference whose file is shown (or missing)
    static bool missing();
    static const std::string& file();
    static const std::string& hostPath();
    static const std::vector<std::string>& lines();
    static std::uint64_t firstLine();
    static bool truncated();
    static bool stale();
    // File line number to scroll to (the reference's first line), 0 if unknown.
    static std::uint64_t targetLine();
    // Whether file line `lineNo` is within the reference's lines.
    static bool inRange(std::uint64_t lineNo);
    // The bytes [begin, end) of lines()[index] the reference covers, by its
    // 1-based inclusive byte columns: from the column on its first line, to
    // the end column on its last, the whole line in between or where a
    // column is unknown (0). nullopt outside the reference's lines.
    static std::optional<std::pair<size_t, size_t>> highlightBytes(size_t index);
    // True once right after a fresh setSource() call, then false again --
    // lets the viewer load the text into its editor and scroll to the
    // reference exactly once instead of every frame.
    static bool takePendingScroll();

    // The code point index of byte `byte` of `line` (UTF-8): naja's columns
    // count bytes, the editor's positions count glyphs.
    static size_t glyphIndex(const std::string& line, size_t byte);

    // The shell command opening `path` at `line`/`column` in an external
    // editor: `pattern` with {file} (shell-quoted), {line} and {col}
    // replaced (an unknown line or column, 0, becomes 1).
    static std::string editorCommand(const std::string& pattern, const std::string& path,
                                     std::uint64_t line, std::uint64_t column);

    // The editor command pattern to use: `configured` ($NAJA_SCHEMATIC_EDITOR)
    // when set; else VS Code's `code -g` when `codeOnPath`; else the CLI
    // inside an installed VS Code app (`bundledCode`, "" if none), which
    // `code` on PATH usually only links to; else the system's opener
    // (`systemOpener`: open / xdg-open), which can't go to a line.
    static std::string editorPattern(const std::string& configured, bool codeOnPath,
                                     const std::string& bundledCode,
                                     const std::string& systemOpener);

    static void clear();
};

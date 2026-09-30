#pragma once
#ifndef __EMSCRIPTEN__

#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

class LocalSNLProvider;

// What `naja-schematic-standalone ... --export out.svg` draws, with no
// window: the schematic built by the same requests and response handling
// as the GUI, then SchematicView::exportSvg().
struct HeadlessExportOptions {
  std::string outPath;
  std::string diagnosisPath;
  // One instance drawn alone with every pin open (resolve_instance): a JSON
  // list of ids ([3, 7], an id_path) or of names (["u1", "u2"], a path), or
  // an object with "id_path"/"path". [] is the top design.
  std::optional<nlohmann::json> focus;
  // Nets added on top, each a terminal: {"id_path"|"path": containing
  // instance ([] = top-level port), "terminal": base name, "bit": bus bit}.
  std::vector<nlohmann::json> traces;          // trace_driver: fan-in cone
  std::vector<nlohmann::json> equipotentials;  // load_equipotential: one net
};

// Parses a --focus/--trace/--equipotential argument (JSON). nullopt, with
// `error` set, when it isn't valid JSON of the expected shape.
std::optional<nlohmann::json> parseFocusArg(const std::string& arg, std::string& error);
std::optional<nlohmann::json> parseTermArg(const std::string& arg, std::string& error);

// Runs the export on an already-loaded provider. Needs no SDL window or GL
// context; creates and destroys its own ImGui context. Returns the process
// exit code (0 on success, errors on stderr).
int runHeadlessExport(LocalSNLProvider* provider, const HeadlessExportOptions& options);

#endif // __EMSCRIPTEN__

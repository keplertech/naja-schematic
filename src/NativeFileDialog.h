#pragma once
#ifndef __EMSCRIPTEN__

#include <string>
#include <vector>

// Thin wrapper around the platform file-picker.
// On macOS: NSOpenPanel (modal, blocks until the user dismisses it).
namespace NativeFileDialog {

  // Open a directory picker. Returns selected path or "" if cancelled.
  std::string pickDirectory(const std::string& title = "Select Directory");

  // Open a multi-file picker filtered to the given extensions (e.g. {"v", "sv"}).
  // Returns the list of selected paths (may be empty if cancelled).
  std::vector<std::string> pickFiles(
    const std::string& title      = "Open Files",
    const std::vector<std::string>& extensions = {}
  );

  // Open a save dialog proposing `defaultName`, restricted to `extension`
  // (e.g. "svg"). Returns the chosen path or "" if cancelled.
  std::string saveFile(
    const std::string& title,
    const std::string& defaultName,
    const std::string& extension
  );

} // namespace NativeFileDialog

#endif // __EMSCRIPTEN__

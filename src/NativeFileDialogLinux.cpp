#ifndef __EMSCRIPTEN__

#include "NativeFileDialog.h"

#include <array>
#include <cstdio>
#include <sstream>

// Linux backend: shells out to `zenity`, matching the modal/blocking
// contract of the macOS NSOpenPanel implementation in NativeFileDialog.mm.

namespace {

std::vector<std::string> runZenity(const std::string& command) {
  std::vector<std::string> lines;
  std::array<char, 4096> buffer{};
  FILE* pipe = popen(command.c_str(), "r");
  if (!pipe) {
    return lines;
  }
  std::string output;
  while (fgets(buffer.data(), buffer.size(), pipe)) {
    output += buffer.data();
  }
  pclose(pipe);

  std::istringstream stream(output);
  std::string line;
  while (std::getline(stream, line, '|')) {
    // zenity separates multi-selection results with '|' and still
    // terminates the whole output with a trailing '\n'.
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
      line.pop_back();
    }
    if (!line.empty()) {
      lines.push_back(line);
    }
  }
  return lines;
}

std::string shellQuote(const std::string& s) {
  std::string quoted = "'";
  for (char c : s) {
    if (c == '\'') {
      quoted += "'\\''";
    } else {
      quoted += c;
    }
  }
  quoted += "'";
  return quoted;
}

} // namespace

namespace NativeFileDialog {

std::string pickDirectory(const std::string& title) {
  std::string command =
      "zenity --file-selection --directory --title=" + shellQuote(title) + " 2>/dev/null";
  auto lines = runZenity(command);
  return lines.empty() ? std::string{} : lines.front();
}

std::vector<std::string> pickFiles(
    const std::string& title,
    const std::vector<std::string>& extensions)
{
  std::string command =
      "zenity --file-selection --multiple --title=" + shellQuote(title);
  if (!extensions.empty()) {
    std::string pattern;
    for (const auto& ext : extensions) {
      if (!pattern.empty()) {
        pattern += " ";
      }
      pattern += "*." + ext;
    }
    command += " --file-filter=" + shellQuote(pattern);
  }
  command += " 2>/dev/null";
  return runZenity(command);
}

std::string saveFile(const std::string& title, const std::string& defaultName,
                     const std::string& extension)
{
  std::string command =
      "zenity --file-selection --save --confirm-overwrite --title=" + shellQuote(title) +
      " --filename=" + shellQuote(defaultName) +
      " --file-filter=" + shellQuote("*." + extension) + " 2>/dev/null";
  auto lines = runZenity(command);
  if (lines.empty()) {
    return {};
  }
  std::string path = lines.front();
  const std::string suffix = "." + extension;
  if (path.size() < suffix.size() ||
      path.compare(path.size() - suffix.size(), suffix.size(), suffix) != 0) {
    path += suffix;
  }
  return path;
}

} // namespace NativeFileDialog

#endif // __EMSCRIPTEN__

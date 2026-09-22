#pragma once

#include <string>

// Mirrors thirdparty/naja/src/core/NajaVersion.h: a literal MAJOR.MINOR.PATCH
// string plus helpers to split it, kept separate from the git commit hash
// (NAJA_SCHEMATIC_GIT_HASH, baked in as a compile definition by CMakeLists.txt
// since it changes every commit, unlike this literal).
namespace naja_schematic {

namespace detail {

inline std::string getVersionPart(const std::string& version, int index) {
  std::string::size_type start = 0;
  for (int i = 0; i < index; ++i) {
    std::string::size_type dot = version.find('.', start);
    if (dot == std::string::npos) {
      return std::string {};
    }
    start = dot + 1;
  }
  std::string::size_type end = version.find('.', start);
  if (end == std::string::npos) {
    end = version.size();
  }
  return version.substr(start, end - start);
}

} // namespace detail

const std::string VERSION { "0.0.6" };
const std::string VERSION_MAJOR { detail::getVersionPart(VERSION, 0) };
const std::string VERSION_MINOR { detail::getVersionPart(VERSION, 1) };
const std::string VERSION_PATCH { detail::getVersionPart(VERSION, 2) };

} // namespace naja_schematic

#include "SourceStore.h"

#include <algorithm>

namespace {
std::string              g_subject;
json                     g_address = json::object();
std::vector<SourceRef>   g_references;
bool                     g_hasReferences = false;

SourceRef                g_shown;
std::string              g_hostPath;
std::vector<std::string> g_lines;
std::uint64_t            g_firstLine     = 1;
bool                     g_truncated     = false;
bool                     g_stale         = false;
bool                     g_hasSource     = false;
bool                     g_missing       = false;
bool                     g_pendingScroll = false;

bool sameReference(const SourceRef& a, const SourceRef& b) {
  return a.role == b.role && a.index == b.index;
}

// The reference's last line: its end line, or its first when that's unknown.
std::uint64_t lastLine(const SourceRef& ref) {
  return std::max<std::uint64_t>(ref.line, ref.endLine);
}
} // namespace

void SourceStore::setReferences(const std::string& subject, const json& address,
                                std::vector<SourceRef> references) {
  g_subject       = subject;
  g_address       = address;
  g_references    = std::move(references);
  g_hasReferences = true;
}

bool SourceStore::hasReferences() { return g_hasReferences; }
const std::string& SourceStore::subject() { return g_subject; }
const std::vector<SourceRef>& SourceStore::references() { return g_references; }

json SourceStore::loadRequest(size_t i) {
  json req = g_address;
  req["request"] = "load_source";
  req["role"]    = g_references.at(i).role;
  if (g_references[i].index) req["index"] = *g_references[i].index;
  return req;
}

std::optional<size_t> SourceStore::shownReference() {
  if (!g_hasSource && !g_missing) return std::nullopt;
  for (size_t i = 0; i < g_references.size(); ++i)
    if (sameReference(g_references[i], g_shown) && g_references[i].file == g_shown.file) return i;
  return std::nullopt;
}

void SourceStore::setSource(const SourceRef& ref, const std::string& text,
                            std::uint64_t firstLine, bool truncated,
                            const std::string& hostPath, bool stale) {
  g_shown     = ref;
  g_hostPath  = hostPath;
  g_stale     = stale;
  g_firstLine = firstLine ? firstLine : 1;
  g_truncated = truncated;
  g_lines.clear();

  size_t start = 0;
  while (start <= text.size()) {
    size_t nl = text.find('\n', start);
    if (nl == std::string::npos) {
      g_lines.push_back(text.substr(start));
      break;
    }
    std::string lineStr = text.substr(start, nl - start);
    if (!lineStr.empty() && lineStr.back() == '\r') lineStr.pop_back();
    g_lines.push_back(std::move(lineStr));
    start = nl + 1;
  }

  g_hasSource     = true;
  g_missing       = false;
  g_pendingScroll = true;
}

void SourceStore::setMissing(const SourceRef& ref) {
  g_shown     = ref;
  g_hostPath.clear();
  g_stale     = false;
  g_lines.clear();
  g_firstLine = 1;
  g_truncated = false;
  g_hasSource = false;
  g_missing   = true;
  g_pendingScroll = false;
}

bool SourceStore::hasSource() { return g_hasSource; }
const SourceRef& SourceStore::shown() { return g_shown; }
bool SourceStore::missing() { return g_missing; }
const std::string& SourceStore::file() { return g_shown.file; }
const std::string& SourceStore::hostPath() { return g_hostPath; }
const std::vector<std::string>& SourceStore::lines() { return g_lines; }
std::uint64_t SourceStore::firstLine() { return g_firstLine; }
bool SourceStore::truncated() { return g_truncated; }
bool SourceStore::stale() { return g_stale; }
std::uint64_t SourceStore::targetLine() { return g_shown.line; }

bool SourceStore::inRange(std::uint64_t lineNo) {
  return g_shown.line && lineNo >= g_shown.line && lineNo <= lastLine(g_shown);
}

std::optional<std::pair<size_t, size_t>> SourceStore::highlightBytes(size_t index) {
  if (index >= g_lines.size()) return std::nullopt;
  const std::uint64_t lineNo = g_firstLine + index;
  if (!inRange(lineNo)) return std::nullopt;
  const size_t length = g_lines[index].size();
  size_t begin = 0, end = length;
  if (lineNo == g_shown.line && g_shown.column)
    begin = std::min<size_t>(g_shown.column - 1, length);
  // The end column belongs to the end line, so only when that's known.
  if (g_shown.endLine && lineNo == g_shown.endLine && g_shown.endColumn)
    end = std::min<size_t>(g_shown.endColumn, length);
  if (end < begin) end = begin;
  return std::make_pair(begin, end);
}

size_t SourceStore::glyphIndex(const std::string& line, size_t byte) {
  size_t glyphs = 0;
  const size_t n = std::min(byte, line.size());
  for (size_t i = 0; i < n; ++i)
    if ((static_cast<unsigned char>(line[i]) & 0xC0) != 0x80) ++glyphs;  // not a continuation byte
  return glyphs;
}

std::string SourceStore::editorCommand(const std::string& pattern, const std::string& path,
                                       std::uint64_t line, std::uint64_t column) {
  std::string quoted = "'";
  for (char c : path) quoted += c == '\'' ? std::string("'\\''") : std::string(1, c);
  quoted += "'";
  const std::pair<std::string, std::string> fields[] = {
    {"{file}", quoted},
    {"{line}", std::to_string(line ? line : 1)},
    {"{col}",  std::to_string(column ? column : 1)},
  };
  std::string out;
  for (size_t i = 0; i < pattern.size();) {
    bool replaced = false;
    for (const auto& [key, value] : fields) {
      if (pattern.compare(i, key.size(), key) == 0) {
        out += value;
        i += key.size();
        replaced = true;
        break;
      }
    }
    if (!replaced) out += pattern[i++];
  }
  return out;
}

std::string SourceStore::editorPattern(const std::string& configured, bool codeOnPath,
                                       const std::string& bundledCode,
                                       const std::string& systemOpener) {
  if (!configured.empty()) return configured;
  if (codeOnPath) return "code -g {file}:{line}:{col}";
  if (!bundledCode.empty())
    return editorCommand("{file}", bundledCode, 0, 0) + " -g {file}:{line}:{col}";
  return systemOpener + " {file}";
}

bool SourceStore::takePendingScroll() {
  bool v = g_pendingScroll;
  g_pendingScroll = false;
  return v;
}

void SourceStore::clear() {
  g_subject.clear();
  g_address = json::object();
  g_references.clear();
  g_hasReferences = false;
  g_shown         = SourceRef{};
  g_hostPath.clear();
  g_stale         = false;
  g_lines.clear();
  g_firstLine     = 1;
  g_truncated     = false;
  g_hasSource     = false;
  g_missing       = false;
  g_pendingScroll = false;
}

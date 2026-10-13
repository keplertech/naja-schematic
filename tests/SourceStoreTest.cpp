#include "SourceStore.h"

#include <gtest/gtest.h>

namespace {

SourceRef ref(std::uint32_t line, std::uint32_t column, std::uint32_t endLine, std::uint32_t endColumn,
              const std::string& role = "declaration", std::optional<unsigned> index = std::nullopt) {
  SourceRef r;
  r.role = role;
  r.index = index;
  r.file = "top.sv";
  r.line = line;
  r.column = column;
  r.endLine = endLine;
  r.endColumn = endColumn;
  return r;
}

} // namespace

class SourceStoreTest : public ::testing::Test {
  protected:
    void TearDown() override { SourceStore::clear(); }
};

TEST_F(SourceStoreTest, StartsEmpty) {
  EXPECT_FALSE(SourceStore::hasSource());
  EXPECT_FALSE(SourceStore::hasReferences());
  EXPECT_FALSE(SourceStore::missing());
  EXPECT_TRUE(SourceStore::lines().empty());
}

TEST_F(SourceStoreTest, SplitsLfSeparatedLines) {
  SourceStore::setSource(ref(2, 0, 0, 0), "line0\nline1\nline2");
  ASSERT_TRUE(SourceStore::hasSource());
  EXPECT_EQ(SourceStore::file(), "top.sv");
  EXPECT_EQ(SourceStore::targetLine(), 2u);
  ASSERT_EQ(SourceStore::lines().size(), 3u);
  EXPECT_EQ(SourceStore::lines()[0], "line0");
  EXPECT_EQ(SourceStore::lines()[1], "line1");
  EXPECT_EQ(SourceStore::lines()[2], "line2");
}

TEST_F(SourceStoreTest, StripsTrailingCarriageReturn) {
  SourceStore::setSource(ref(0, 0, 0, 0), "line0\r\nline1\r\n");
  ASSERT_EQ(SourceStore::lines().size(), 3u);
  EXPECT_EQ(SourceStore::lines()[0], "line0");
  EXPECT_EQ(SourceStore::lines()[1], "line1");
  EXPECT_EQ(SourceStore::lines()[2], "");
}

TEST_F(SourceStoreTest, EmptyTextYieldsOneEmptyLine) {
  SourceStore::setSource(ref(0, 0, 0, 0), "");
  ASSERT_EQ(SourceStore::lines().size(), 1u);
  EXPECT_EQ(SourceStore::lines()[0], "");
}

TEST_F(SourceStoreTest, ClearResetsState) {
  SourceStore::setReferences("u1", json{{"kind", "instance"}}, {ref(5, 0, 0, 0)});
  SourceStore::setSource(ref(5, 0, 0, 0), "a\nb");
  SourceStore::clear();
  EXPECT_FALSE(SourceStore::hasSource());
  EXPECT_FALSE(SourceStore::hasReferences());
  EXPECT_TRUE(SourceStore::references().empty());
  EXPECT_EQ(SourceStore::file(), "");
  EXPECT_TRUE(SourceStore::lines().empty());
  EXPECT_EQ(SourceStore::targetLine(), 0u);
}

TEST_F(SourceStoreTest, PendingScrollIsConsumedExactlyOnce) {
  SourceStore::setSource(ref(3, 0, 0, 0), "a\nb");
  EXPECT_TRUE(SourceStore::takePendingScroll());
  EXPECT_FALSE(SourceStore::takePendingScroll());
}

TEST_F(SourceStoreTest, NewSourceRearmsPendingScroll) {
  SourceStore::setSource(ref(1, 0, 0, 0), "x");
  EXPECT_TRUE(SourceStore::takePendingScroll());
  SourceStore::setSource(ref(2, 0, 0, 0), "y");
  EXPECT_TRUE(SourceStore::takePendingScroll());
}

TEST_F(SourceStoreTest, LoadRequestNamesTheObjectAndTheReference) {
  json address = {{"kind", "term"}, {"id_path", {0}}, {"terminal", "a"}, {"bit", 1}};
  SourceStore::setReferences("u_alu/a[1]", address,
                             {ref(4, 3, 4, 39), ref(2, 14, 2, 24, "origin", 0)});
  json declaration = SourceStore::loadRequest(0);
  EXPECT_EQ(declaration["request"], "load_source");
  EXPECT_EQ(declaration["role"], "declaration");
  EXPECT_FALSE(declaration.contains("index"));
  EXPECT_EQ(declaration["terminal"], "a");
  EXPECT_EQ(declaration["bit"], 1);
  json origin = SourceStore::loadRequest(1);
  EXPECT_EQ(origin["role"], "origin");
  EXPECT_EQ(origin["index"], 0);
  EXPECT_EQ(origin["id_path"], json::array({0}));
}

TEST_F(SourceStoreTest, ShownReferenceFollowsTheLoadedFile) {
  SourceStore::setReferences("g0", json::object(),
                             {ref(4, 3, 4, 39), ref(2, 14, 2, 24, "origin", 0),
                              ref(1, 12, 1, 24, "origin", 1)});
  EXPECT_FALSE(SourceStore::shownReference().has_value());
  SourceStore::setSource(ref(1, 12, 1, 24, "origin", 1), "x");
  EXPECT_EQ(SourceStore::shownReference(), 2u);
  SourceStore::setMissing(ref(4, 3, 4, 39));
  EXPECT_TRUE(SourceStore::missing());
  EXPECT_FALSE(SourceStore::hasSource());
  EXPECT_EQ(SourceStore::shownReference(), 0u);
}

TEST_F(SourceStoreTest, HighlightsTheColumnsOfASingleLineRange) {
  // Columns are 1-based, inclusive, in bytes: 14..24 is "a[0] & a[1]".
  SourceStore::setSource(ref(2, 14, 2, 24),
                         "module alu(input [1:0] a, output y);\n  assign y = a[0] & a[1];\n");
  EXPECT_FALSE(SourceStore::highlightBytes(0).has_value());
  auto span = SourceStore::highlightBytes(1);
  ASSERT_TRUE(span.has_value());
  EXPECT_EQ(SourceStore::lines()[1].substr(span->first, span->second - span->first), "a[0] & a[1]");
  EXPECT_FALSE(SourceStore::highlightBytes(2).has_value());
  EXPECT_TRUE(SourceStore::inRange(2));
  EXPECT_FALSE(SourceStore::inRange(1));
}

TEST_F(SourceStoreTest, HighlightsAMultiLineRangeFromStartColumnToEndColumn) {
  SourceStore::setSource(ref(1, 3, 3, 2), "abcdef\nghijkl\nmnopqr");
  EXPECT_EQ(SourceStore::highlightBytes(0), std::make_pair(size_t(2), size_t(6)));
  EXPECT_EQ(SourceStore::highlightBytes(1), std::make_pair(size_t(0), size_t(6)));
  EXPECT_EQ(SourceStore::highlightBytes(2), std::make_pair(size_t(0), size_t(2)));
}

TEST_F(SourceStoreTest, UnknownEndHighlightsOnlyTheStartLine) {
  // A Yosys src with no end: line 2 from column 3, nothing invented after.
  SourceStore::setSource(ref(2, 3, 0, 0), "abc\ndefgh\nijk");
  EXPECT_FALSE(SourceStore::highlightBytes(0).has_value());
  EXPECT_EQ(SourceStore::highlightBytes(1), std::make_pair(size_t(2), size_t(5)));
  EXPECT_FALSE(SourceStore::highlightBytes(2).has_value());
}

TEST_F(SourceStoreTest, WindowOfALargeFileKeepsFileLineNumbers) {
  // Only lines 99..101 of the file were sent; the reference is line 100.
  SourceStore::setSource(ref(100, 2, 100, 3), "a\nbcd\ne", /*firstLine=*/99, /*truncated=*/true);
  EXPECT_TRUE(SourceStore::truncated());
  EXPECT_EQ(SourceStore::firstLine(), 99u);
  EXPECT_FALSE(SourceStore::highlightBytes(0).has_value());
  EXPECT_EQ(SourceStore::highlightBytes(1), std::make_pair(size_t(1), size_t(3)));
}

TEST_F(SourceStoreTest, ColumnsPastTheLineAreClamped) {
  SourceStore::setSource(ref(1, 10, 1, 40), "short");
  EXPECT_EQ(SourceStore::highlightBytes(0), std::make_pair(size_t(5), size_t(5)));
}

TEST_F(SourceStoreTest, GlyphIndexCountsCodePointsNotBytes) {
  const std::string line = "  y = a \xC2\xB5 b;";  // a two-byte character (micro sign)
  EXPECT_EQ(SourceStore::glyphIndex(line, 0), 0u);
  EXPECT_EQ(SourceStore::glyphIndex(line, 8), 8u);   // up to the multi-byte character
  EXPECT_EQ(SourceStore::glyphIndex(line, 10), 9u);  // past it: one glyph for two bytes
  EXPECT_EQ(SourceStore::glyphIndex(line, 99), 12u); // clamped to the line
}

TEST_F(SourceStoreTest, EditorCommandQuotesThePath) {
  EXPECT_EQ(SourceStore::editorCommand("code -g {file}:{line}:{col}", "/w/rtl/alu.v", 12, 5),
            "code -g '/w/rtl/alu.v':12:5");
  // Unknown coordinates open at 1; quotes in the path can't break out.
  EXPECT_EQ(SourceStore::editorCommand("vim +{line} {file}", "/w/it's here.v", 0, 0),
            "vim +1 '/w/it'\\''s here.v'");
  EXPECT_EQ(SourceStore::editorCommand("subl {file}:{line}:{col} {", "a.v", 3, 0),
            "subl 'a.v':3:1 {");
}

TEST_F(SourceStoreTest, KeepsTheHostPathOfTheShownFile) {
  SourceStore::setSource(ref(1, 0, 0, 0), "x", 1, false, "/abs/top.sv");
  EXPECT_EQ(SourceStore::hostPath(), "/abs/top.sv");
  SourceStore::setMissing(ref(1, 0, 0, 0));
  EXPECT_EQ(SourceStore::hostPath(), "");
}

TEST_F(SourceStoreTest, EditorPatternFallsBackToWhatIsInstalled) {
  const std::string app = "/Applications/Visual Studio Code.app/Contents/Resources/app/bin/code";
  // A configured command always wins.
  EXPECT_EQ(SourceStore::editorPattern("subl {file}:{line}", true, app, "open"), "subl {file}:{line}");
  EXPECT_EQ(SourceStore::editorPattern("", true, app, "open"), "code -g {file}:{line}:{col}");
  // VS Code installed without its `code` shell command: its bundled CLI, quoted.
  EXPECT_EQ(SourceStore::editorPattern("", false, app, "open"),
            "'" + app + "' -g {file}:{line}:{col}");
  EXPECT_EQ(SourceStore::editorPattern("", false, "", "xdg-open"), "xdg-open {file}");
}

TEST_F(SourceStoreTest, KeepsWhetherTheFileIsStale) {
  SourceStore::setSource(ref(1, 0, 0, 0), "x", 1, false, "/abs/top.sv", /*stale=*/true);
  EXPECT_TRUE(SourceStore::stale());
  SourceStore::setSource(ref(1, 0, 0, 0), "x");
  EXPECT_FALSE(SourceStore::stale());
  SourceStore::setSource(ref(1, 0, 0, 0), "x", 1, false, "", true);
  SourceStore::clear();
  EXPECT_FALSE(SourceStore::stale());
}

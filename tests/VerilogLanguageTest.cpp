#include "VerilogLanguage.h"

#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {

// How many glyphs of `text` a tokenizer takes from its start (0: none).
size_t taken(const std::function<TextEditor::Iterator(TextEditor::Iterator, TextEditor::Iterator)>& f,
             const std::string& text) {
  std::vector<TextEditor::Glyph> glyphs;
  for (unsigned char c : text) glyphs.emplace_back(static_cast<ImWchar>(c));
  glyphs.emplace_back(ImWchar(0));  // something past the end, as in a line
  TextEditor::Iterator begin(glyphs.data());
  TextEditor::Iterator end(glyphs.data() + text.size());
  return f(begin, end) - begin;
}

size_t number(const std::string& s)     { return taken(verilogLanguage()->getNumber, s); }
size_t identifier(const std::string& s) { return taken(verilogLanguage()->getIdentifier, s); }

} // namespace

TEST(VerilogLanguage, Numbers) {
  EXPECT_EQ(number("42;"), 2u);
  EXPECT_EQ(number("1_000 "), 5u);
  EXPECT_EQ(number("8'hFF)"), 5u);
  EXPECT_EQ(number("4'b10xz;"), 7u);
  EXPECT_EQ(number("16'sd5,"), 6u);
  EXPECT_EQ(number("8'h FF"), 6u);    // white space after the base is legal
  EXPECT_EQ(number("'h1F "), 4u);     // unsized
  EXPECT_EQ(number("'0;"), 2u);       // unbased unsized
  EXPECT_EQ(number("'x"), 2u);
  EXPECT_EQ(number("1.5e-3 "), 6u);
  EXPECT_EQ(number("3.14"), 4u);
  EXPECT_EQ(number("2'"), 1u);        // a size alone: the quote isn't the number's
  EXPECT_EQ(number("'{a, b}"), 0u);   // an assignment pattern
  EXPECT_EQ(number("a1"), 0u);
}

TEST(VerilogLanguage, Identifiers) {
  EXPECT_EQ(identifier("data_in[3]"), 7u);
  EXPECT_EQ(identifier("u1$2 "), 4u);
  EXPECT_EQ(identifier("$display("), 8u);
  EXPECT_EQ(identifier("\\a/b[0] .Y"), 7u);  // escaped: up to white space
  EXPECT_EQ(identifier("$ "), 0u);
  EXPECT_EQ(identifier("3x"), 0u);
}

TEST(VerilogLanguage, KeywordsAndTypes) {
  const auto* l = verilogLanguage();
  EXPECT_EQ(l->name, "Verilog");
  EXPECT_TRUE(l->keywords.count("module"));
  EXPECT_TRUE(l->keywords.count("always_ff"));
  EXPECT_TRUE(l->keywords.count("nand"));  // a built-in gate
  EXPECT_TRUE(l->declarations.count("wire"));
  EXPECT_TRUE(l->declarations.count("logic"));
  EXPECT_TRUE(l->identifiers.count("$clog2"));
  EXPECT_EQ(l->preprocess, ImWchar('`'));
  EXPECT_EQ(l->singleLineComment, "//");
}

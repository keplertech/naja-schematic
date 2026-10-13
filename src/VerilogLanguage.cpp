#include "VerilogLanguage.h"

namespace {

using Iterator = TextEditor::Iterator;

bool isIdentifierStart(ImWchar c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool isIdentifierChar(ImWchar c) {
  return isIdentifierStart(c) || (c >= '0' && c <= '9') || c == '$';
}

bool isDecimalDigit(ImWchar c) { return (c >= '0' && c <= '9') || c == '_'; }

bool isBaseChar(ImWchar c) {
  switch (c) {
    case 'b': case 'B': case 'o': case 'O': case 'd': case 'D': case 'h': case 'H': return true;
    default: return false;
  }
}

// Digits of a based number: hex digits, unknown/high-impedance values, `_`.
bool isBasedDigit(ImWchar c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') ||
         c == 'x' || c == 'X' || c == 'z' || c == 'Z' || c == '?' || c == '_';
}

bool isUnbasedUnsizedValue(ImWchar c) {
  return c == '0' || c == '1' || c == 'x' || c == 'X' || c == 'z' || c == 'Z';
}

// foo, foo$1, $display (system task), \escaped/name (up to white space).
Iterator getIdentifier(Iterator start, Iterator end) {
  Iterator i = start;
  if (i == end) return start;
  if (*i == '\\') {
    ++i;
    while (i < end && *i != ' ' && *i != '\t') ++i;
    return i;
  }
  if (*i == '$') {
    ++i;
    if (i == end || !isIdentifierStart(*i)) return start;
  } else if (!isIdentifierStart(*i)) {
    return start;
  }
  while (i < end && isIdentifierChar(*i)) ++i;
  return i;
}

// 42, 1_000, 3.14, 1.5e-3, 8'hFF, 4'sb10xz, 'h1F, '0, 'x.
Iterator getNumber(Iterator start, Iterator end) {
  Iterator i = start;
  while (i < end && isDecimalDigit(*i)) ++i;
  const bool sized = i != start;
  if (i < end && *i == '\'') {
    Iterator j = i;
    ++j;
    if (j < end && (*j == 's' || *j == 'S')) ++j;
    if (j < end && isBaseChar(*j)) {
      ++j;
      while (j < end && (*j == ' ' || *j == '\t')) ++j;  // 8'h FF is legal
      Iterator digits = j;
      while (j < end && isBasedDigit(*j)) ++j;
      if (j != digits) return j;
    } else if (!sized && j < end && isUnbasedUnsizedValue(*j)) {
      ++j;
      return j;
    }
    return sized ? i : start;  // a size alone; the quote is punctuation
  }
  if (!sized) return start;
  // Real: digits "." digits, optional exponent.
  if (i < end && *i == '.') {
    Iterator j = i;
    ++j;
    if (j < end && isDecimalDigit(*j)) {
      while (j < end && isDecimalDigit(*j)) ++j;
      i = j;
    }
  }
  if (i < end && (*i == 'e' || *i == 'E')) {
    Iterator j = i;
    ++j;
    if (j < end && (*j == '+' || *j == '-')) ++j;
    if (j < end && isDecimalDigit(*j)) {
      while (j < end && isDecimalDigit(*j)) ++j;
      i = j;
    }
  }
  return i;
}

bool isPunctuation(ImWchar c) {
  switch (c) {
    case '!': case '%': case '&': case '(': case ')': case '*': case '+': case ',':
    case '-': case '.': case '/': case ':': case ';': case '<': case '=': case '>':
    case '?': case '@': case '[': case ']': case '^': case '{': case '|': case '}':
    case '~': case '#': case '\'':
      return true;
    default:
      return false;
  }
}

} // namespace

const TextEditor::Language* verilogLanguage() {
  static bool initialized = false;
  static TextEditor::Language language;
  if (initialized) return &language;

  language.name = "Verilog";
  language.preprocess = '`';
  language.singleLineComment = "//";
  language.commentStart = "/*";
  language.commentEnd = "*/";
  language.hasDoubleQuotedStrings = true;
  language.stringEscape = '\\';

  // Control and structure (IEEE 1364 + the 1800 additions in common use).
  static const char* const keywords[] = {
    "always", "always_comb", "always_ff", "always_latch", "assign", "assert", "assume", "automatic",
    "begin", "break", "case", "casex", "casez", "cell", "config", "continue", "cover", "deassign",
    "default", "defparam", "design", "disable", "do", "else", "end", "endcase", "endclass",
    "endconfig", "endfunction", "endgenerate", "endinterface", "endmodule", "endpackage",
    "endprimitive", "endprogram", "endproperty", "endspecify", "endtable", "endtask", "export",
    "extends", "final", "for", "force", "foreach", "forever", "fork", "function", "generate",
    "genvar", "if", "iff", "import", "incdir", "include", "initial", "inside", "instance", "join",
    "join_any", "join_none", "liblist", "library", "localparam", "macromodule", "modport", "module",
    "negedge", "new", "null", "or", "package", "parameter", "posedge", "primitive", "priority",
    "program", "property", "release", "repeat", "return", "specify", "specparam", "super", "table",
    "task", "this", "unique", "unique0", "use", "virtual", "wait", "while", "with",
    // built-in gates
    "and", "buf", "bufif0", "bufif1", "cmos", "nand", "nmos", "nor", "not", "notif0", "notif1",
    "pmos", "pulldown", "pullup", "rcmos", "rnmos", "rpmos", "rtran", "rtranif0", "rtranif1",
    "tran", "tranif0", "tranif1", "xnor", "xor",
  };

  // Ports, nets, variables and types.
  static const char* const declarations[] = {
    "bit", "byte", "chandle", "class", "const", "enum", "event", "inout", "input", "int",
    "integer", "interface", "logic", "longint", "output", "packed", "real", "realtime", "ref",
    "reg", "shortint", "shortreal", "signed", "static", "string", "struct", "supply0", "supply1",
    "time", "tri", "tri0", "tri1", "triand", "trior", "trireg", "type", "typedef", "union",
    "unsigned", "uwire", "var", "void", "wand", "wire", "wor",
  };

  // System tasks and functions.
  static const char* const identifiers[] = {
    "$bits", "$clog2", "$display", "$error", "$fatal", "$finish", "$fwrite", "$info", "$monitor",
    "$random", "$readmemb", "$readmemh", "$signed", "$stop", "$strobe", "$time", "$unsigned",
    "$warning", "$write",
  };

  for (auto* k : keywords) language.keywords.insert(k);
  for (auto* d : declarations) language.declarations.insert(d);
  for (auto* i : identifiers) language.identifiers.insert(i);
  language.isPunctuation = isPunctuation;
  language.getIdentifier = getIdentifier;
  language.getNumber = getNumber;
  initialized = true;
  return &language;
}

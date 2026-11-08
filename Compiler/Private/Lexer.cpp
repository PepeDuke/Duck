#include "Literals.h"
#include "Pipeline.h"
#include "Unicode.h"
#include <cctype>

namespace duck {
namespace {

bool isDigit(char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }

bool isTwoCharOperator(const std::string &pair) {
  return pair == "::" || pair == "==" || pair == "!=" || pair == "<=" || pair == ">=" ||
         pair == "++" || pair == "--" || pair == "&&" || pair == "||" ||
         pair == "+=" || pair == "-=" || pair == "*=" || pair == "/=" ||
         pair == "%=" || pair == "&=" || pair == "|=" || pair == "^=" ||
         pair == "<<" || pair == ">>";
}

// Turns one file's bytes into tokens. Newlines are tokens, including the ones
// inside block comments, so statements keep their line structure.
class Lexer {
  const std::string &source;
  const std::string &sourceName;
  std::vector<Token> out;
  size_t i = 0;
  int line = 1, col = 1;

  bool atEnd() const { return i >= source.size(); }
  bool nextIs(char c) const { return !atEnd() && source[i] == c; }
  bool startsWith(const char *text) const { return source.compare(i, 2, text) == 0; }
  Token startToken() const { return Token{"", line, col, false, sourceName}; }

  char take() {
    const char c = source[i++];

    if (c == '\n') {
      ++line;
      col = 1;
    } else
      ++col;

    return c;
  }

  template <class Predicate> void takeWhile(Token &t, Predicate matches) {
    while (!atEnd() && matches(source[i])) t.text += take();
  }

  void skipLineComment() {
    while (!atEnd() && source[i] != '\n') take();
  }

  void skipBlockComment() {
    const Token start = startToken();
    take();
    take();

    while (!atEnd() && !startsWith("*/")) {
      if (source[i] == '\n') out.push_back({"\n", line, col, false, sourceName});

      take();
    }

    if (atEnd()) fail(start, "unterminated comment");

    take();
    take();
  }

  void unicodeEscape(Token &t, int digits) {
    uint32_t codePoint = 0;

    for (int n = 0; n < digits; ++n) {
      if (atEnd()) fail(t, "incomplete Unicode escape");

      const int digit = digitValue(take());

      if (digit < 0) fail(t, "invalid Unicode escape");

      codePoint = (codePoint << 4) | static_cast<uint32_t>(digit);
    }

    if (codePoint > 0x10FFFF || (codePoint >= 0xD800 && codePoint <= 0xDFFF))
      fail(t, "Unicode escape must be a scalar value");

    appendUTF8(t.text, codePoint);
  }

  void escapeSequence(Token &t) {
    if (atEnd()) fail(t, "unterminated string literal");

    char c = take();

    switch (c) {
    case 'u':
      return unicodeEscape(t, 4);
    case 'U':
      return unicodeEscape(t, 8);
    case '0':
      c = '\0';
      break;
    case 'n':
      c = '\n';
      break;
    case 'r':
      c = '\r';
      break;
    case 't':
      c = '\t';
      break;
    case '\\':
    case '"':
    case '\'':
      break;
    default:
      fail(t, "unsupported escape sequence");
    }

    t.text += c;
  }

  // A character literal shares string escapes but must hold one scalar.
  void textLiteral(Token &t, char quote) {
    t.string = true;
    t.character = quote == '\'';
    take();

    while (!atEnd() && source[i] != quote) {
      const char c = take();

      if (c == '\n' || c == '\r')
        fail(t, t.character ? "newline in character literal" : "newline in string literal");

      if (c == '\\')
        escapeSequence(t);
      else
        t.text += c;
    }

    if (atEnd()) fail(t, t.character ? "unterminated character literal" : "unterminated string literal");

    take();

    if (decodeUTF8(t).size() != 1 && t.character)
      fail(t, "character literal must contain exactly one Unicode scalar");
  }

  void exponent(Token &t) {
    t.text += take();

    if (nextIs('+') || nextIs('-')) t.text += take();

    if (atEnd() || !isDigit(source[i])) fail(t, "exponent requires digits");

    takeWhile(t, isDigit);
  }

  void number(Token &t) {
    // Consume a prefixed integer as one token, including invalid digits, so
    // semantic diagnostics point to the complete literal. Hex e/E is a digit,
    // never a decimal exponent marker.
    if (i + 1 < source.size() && integerLiteralBase(source.substr(i, 2)) != 10) {
      t.text += take();
      t.text += take();
      takeWhile(t, isIdentifierChar);
      return;
    }

    takeWhile(t, isDigit);

    if (i + 1 < source.size() && source[i] == '.' && isDigit(source[i + 1])) {
      t.text += take();
      takeWhile(t, isDigit);
    }

    if (nextIs('e') || nextIs('E')) exponent(t);

    takeWhile(t, isIdentifierChar);
  }

  void word(Token &t) {
    takeWhile(t, isIdentifierChar);

    if (t.text == "struct" || t.text == "class")
      fail(t, "'" + t.text + "' is reserved in Duck. Use the container name directly");
  }

  void punctuation(Token &t) {
    t.text += take();

    if (t.text == "." && source.compare(i, 2, "..") == 0) {
      t.text += take();
      t.text += take();
      return;
    }

    if (t.text == "." && nextIs('.')) {
      t.text += take(); // The duck call operator.
      return;
    }

    if (atEnd()) return;

    if (isTwoCharOperator(t.text + source[i])) t.text += take();

    if ((t.text == "<<" || t.text == ">>") && nextIs('=')) t.text += take();
  }

  void token(char c) {
    Token t = startToken();

    if (c == '"' || c == '\'')
      textLiteral(t, c);
    else if (isDigit(c))
      number(t);
    else if (isIdentifierChar(c))
      word(t);
    else
      punctuation(t);

    out.push_back(std::move(t));
  }

public:
  Lexer(const std::string &source, const std::string &sourceName) : source(source), sourceName(sourceName) {}

  std::vector<Token> run() {
    while (!atEnd()) {
      const char c = source[i];

      if (c == ' ' || c == '\t' || c == '\r')
        take();
      else if (startsWith("//"))
        skipLineComment();
      else if (startsWith("/*"))
        skipBlockComment();
      else
        token(c);
    }

    out.push_back({"<eof>", line, col, false, sourceName});
    return std::move(out);
  }
};

} // namespace

std::vector<Token> lex(const std::string &source, const std::string &sourceName) {
  return Lexer(source, sourceName).run();
}

} // namespace duck

#include "Literals.h"
#include "Pipeline.h"
#include <limits>

namespace duck {
namespace {
// A #define value. Defines are typed so #if never guesses what a word means.
struct Constant {
  enum Kind { Bool, Integer, Text } kind = Bool;
  bool truth = false;
  long long number = 0;
  std::string text;
};
const char *kindName(Constant::Kind kind) {
  return kind == Constant::Bool ? "bool" : kind == Constant::Integer ? "integer" : "string";
}

class Preprocessor {
  const std::vector<Token> &in;
  std::vector<Token> out;
  struct Definition {
    Token name;
    Constant value;
  };
  std::map<std::string, Definition> defines;

  bool punct(size_t k, Tok kind) const { return k < in.size() && !in[k].string && in[k].text == kind; }
  bool punct(size_t k, const char *text) const { return k < in.size() && !in[k].string && in[k].text == text; }
  bool directive(size_t k, Tok word) const { return punct(k, Tok::Hash) && punct(k + 1, word); }
  void requireLineStart(size_t k) const {
    if (k && !punct(k - 1, Tok::Newline) && !punct(k - 1, Tok::LeftBrace) && !punct(k - 1, Tok::RightBrace))
      fail(in[k], "'#" + in[k + 1].text + "' must start a line");
  }
  size_t skipLines(size_t k) const {
    while (punct(k, Tok::Newline)) ++k;

    return k;
  }
  // The '}' matching the '{' at open. Strings never count as braces.
  size_t closing(size_t open, const std::string &directiveName) const {
    int depth = 0;

    for (size_t k = open; k < in.size(); ++k) {
      if (punct(k, Tok::LeftBrace)) ++depth;
      else if (punct(k, Tok::RightBrace) && --depth == 0) return k;
    }

    fail(in[open], "unterminated " + directiveName + " block");
  }

  // Conditions and define values share one small typed expression language
  // over the tokens in [cursor, end).
  size_t cursor = 0, end = 0;
  bool at(Tok kind) const { return cursor < end && punct(cursor, kind); }
  bool at(const char *text) const { return cursor < end && punct(cursor, text); }
  bool accept(Tok kind) {
    if (!at(kind)) return false;

    ++cursor;
    return true;
  }
  bool accept(const char *text) {
    if (!at(text)) return false;

    ++cursor;
    return true;
  }
  const Token &current() const { return in[cursor < end ? cursor : end - 1]; }
  [[noreturn]] void wrongType(const Token &t, const std::string &what, const Constant &value) const {
    fail(t, what + " requires " + (what[0] == '!' || what[0] == '&' || what[0] == '|' ? "bool" : "integer") +
                " operands, got " + kindName(value.kind));
  }
  Constant literal(const Token &t) const {
    Constant value;
    value.kind = Constant::Integer;
    const int base = integerLiteralBase(t.text);
    const std::string digits = base == 10 ? t.text.str() : t.text.substr(2);

    if (digits.empty()) fail(t, "integer literal requires digits after its prefix");

    for (char c : digits) {
      const int digit = digitValue(c);

      if (digit < 0 || digit >= base) fail(t, "#define and #if support bool, integer, and string values, not '" + t.text + "'");

      if (value.number > (std::numeric_limits<long long>::max() - digit) / base)
        fail(t, "integer in #define or #if is out of range for int64");

      value.number = value.number * base + digit;
    }

    return value;
  }
  Constant primary() {
    if (cursor >= end) fail(current(), "expected a value in #define or #if expression");

    const auto &t = in[cursor];

    if (accept(Tok::LeftParen)) {
      auto value = logicalOr();

      if (!accept(Tok::RightParen)) fail(current(), "expected ')' in #define or #if expression");

      return value;
    }

    ++cursor;

    if (t.character) fail(t, "#define and #if support bool, integer, and string values, not char");

    if (t.string) {
      Constant value;
      value.kind = Constant::Text;
      value.text = t.text;
      return value;
    }

    if (std::isdigit(static_cast<unsigned char>(t.text[0]))) return literal(t);

    if (t.text == Tok::True || t.text == Tok::False) {
      Constant value;
      value.truth = t.text == Tok::True;
      return value;
    }

    if (!isWord(t)) fail(t, "unexpected '" + t.text + "' in #define or #if expression");

    const auto found = defines.find(t.text);

    if (found == defines.end())
      fail(t, "'" + t.text + "' is not defined here; a #define is visible from its line to the end of its file");

    return found->second.value;
  }
  Constant unary() {
    const auto &t = current();

    if (accept(Tok::Bang)) {
      auto value = unary();

      if (value.kind != Constant::Bool) wrongType(t, "!", value);

      value.truth = !value.truth;
      return value;
    }

    if (accept(Tok::Minus)) {
      auto value = unary();

      if (value.kind != Constant::Integer) wrongType(t, "-", value);

      value.number = -value.number;
      return value;
    }

    return primary();
  }
  Constant relational() {
    auto left = unary();

    while (at(Tok::Less) || at(Tok::Greater) || at(Tok::LessEqual) || at(Tok::GreaterEqual)) {
      const auto &op = in[cursor++];
      const auto right = unary();

      if (left.kind != Constant::Integer) wrongType(op, op.text, left);
      if (right.kind != Constant::Integer) wrongType(op, op.text, right);

      Constant result;
      result.truth = op.text == Tok::Less    ? left.number < right.number
                     : op.text == Tok::Greater  ? left.number > right.number
                     : op.text == Tok::LessEqual ? left.number <= right.number
                                       : left.number >= right.number;
      left = result;
    }

    return left;
  }
  Constant equality() {
    auto left = relational();

    while (at(Tok::EqualEqual) || at(Tok::NotEqual)) {
      const auto &op = in[cursor++];
      const auto right = relational();

      if (left.kind != right.kind)
        fail(op, std::string("cannot compare ") + kindName(left.kind) + " with " + kindName(right.kind) +
                     " in #define or #if");

      const bool same = left.truth == right.truth && left.number == right.number && left.text == right.text;
      Constant result;
      result.truth = op.text == Tok::EqualEqual ? same : !same;
      left = result;
    }

    return left;
  }
  // Both operands of && and || are always checked: a misspelled define must
  // not hide behind a short circuit.
  Constant logicalAnd() {
    auto left = equality();

    while (at(Tok::AndAnd)) {
      const auto &op = in[cursor++];
      const auto right = equality();

      if (left.kind != Constant::Bool) wrongType(op, "&&", left);
      if (right.kind != Constant::Bool) wrongType(op, "&&", right);

      left.truth = left.truth && right.truth;
    }

    return left;
  }
  Constant logicalOr() {
    auto left = logicalAnd();

    while (at(Tok::OrOr)) {
      const auto &op = in[cursor++];
      const auto right = logicalAnd();

      if (left.kind != Constant::Bool) wrongType(op, "||", left);
      if (right.kind != Constant::Bool) wrongType(op, "||", right);

      left.truth = left.truth || right.truth;
    }

    return left;
  }
  Constant evaluate(size_t first, size_t last, const Token &directiveToken) {
    if (first >= last) fail(directiveToken, "'#" + directiveToken.text + "' requires a value");

    cursor = first;
    end = last;
    auto value = logicalOr();

    if (cursor != end) fail(in[cursor], "unexpected '" + in[cursor].text + "' in #define or #if expression");

    return value;
  }

  // Returns the index of the directive's last token.
  size_t define(size_t k) {
    requireLineStart(k);
    const auto &keyword = in[k + 1];
    const size_t nameAt = k + 2;

    if (nameAt >= in.size() || !isWord(in[nameAt]) || in[nameAt].text == Tok::Eof)
      fail(keyword, "#define requires a name, as in #define WIN_64 true");

    size_t last = nameAt + 1;

    while (last < in.size() && !punct(last, Tok::Newline) && !punct(last, Tok::Eof)) ++last;

    if (const auto existing = defines.find(in[nameAt].text); existing != defines.end())
      fail(in[nameAt], "'" + in[nameAt].text + "' is already defined at " + where(existing->second.name));

    if (in[nameAt].text == Tok::True || in[nameAt].text == Tok::False)
      fail(in[nameAt], "cannot #define '" + in[nameAt].text + "'");

    auto value = evaluate(nameAt + 1, last, keyword);
    defines[in[nameAt].text] = {in[nameAt], std::move(value)};
    return last - 1;
  }
  // Only the active branch is copied, with its braces removed, so its
  // declarations land in the surrounding scope. An inactive branch must only
  // balance its braces; nothing inside it is read.
  size_t conditional(size_t k) {
    requireLineStart(k);
    const auto &keyword = in[k + 1];
    size_t last = k + 2;

    while (last < in.size() && !punct(last, Tok::Newline) && !punct(last, Tok::LeftBrace) && !punct(last, Tok::Eof)) ++last;

    const auto condition = evaluate(k + 2, last, keyword);

    if (condition.kind != Constant::Bool)
      fail(keyword, std::string("#if requires a bool condition, got ") + kindName(condition.kind));

    const size_t open = skipLines(last);

    if (!punct(open, Tok::LeftBrace)) fail(keyword, "#if requires a { ... } block");

    const size_t close = closing(open, "#if");

    if (condition.truth) run(open + 1, close);

    size_t finish = close;
    const size_t elseAt = skipLines(close + 1);

    if (directive(elseAt, Tok::Else)) {
      const size_t elseOpen = skipLines(elseAt + 2);

      if (!punct(elseOpen, Tok::LeftBrace)) fail(in[elseAt + 1], "#else requires a { ... } block");

      const size_t elseClose = closing(elseOpen, "#else");

      if (!condition.truth) run(elseOpen + 1, elseClose);

      finish = elseClose;
    }

    if (!punct(finish + 1, Tok::Newline) && !punct(finish + 1, Tok::RightBrace) && !punct(finish + 1, Tok::Eof))
      fail(in[finish + 1], "expected a new line after the #if block");

    return finish;
  }
  void run(size_t first, size_t last) {
    for (size_t k = first; k < last; ++k) {
      if (directive(k, Tok::Define)) k = define(k);
      else if (directive(k, Tok::If)) k = conditional(k);
      else if (directive(k, Tok::Else)) fail(in[k + 1], "#else without a matching #if");
      else out.push_back(in[k]);
    }
  }

public:
  explicit Preprocessor(const std::vector<Token> &tokens) : in(tokens) {}
  std::vector<Token> result() {
    out.reserve(in.size());
    run(0, in.size());
    return std::move(out);
  }
};
} // namespace

std::vector<Token> preprocess(std::vector<Token> tokens) {
  return Preprocessor(tokens).result();
}
} // namespace duck

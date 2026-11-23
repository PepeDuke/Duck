#include "Literals.h"
#include "Pipeline.h"
#include "Unicode.h"
#include <atomic>
#include <cctype>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace duck {
namespace {
std::atomic<bool> sourcesFrozen{false};
// A deque, so a path returned by reference stays put while others are added.
std::deque<std::string> sourcePaths{std::string()};
std::unordered_map<std::string, SourceId> sourceIds{{std::string(), 0}};
} // namespace

SourceId registerSource(const std::string &path) {
  if (const auto found = sourceIds.find(path); found != sourceIds.end()) return found->second;

  if (sourcesFrozen.load())
    throw std::logic_error("source path '" + path + "' registered while worker threads read the table");

  const auto id = static_cast<SourceId>(sourcePaths.size());
  sourcePaths.push_back(path);
  sourceIds.emplace(path, id);
  return id;
}

SourceId findSource(const std::string &path) {
  const auto found = sourceIds.find(path);

  if (found == sourceIds.end()) throw std::logic_error("source path '" + path + "' was never registered");

  return found->second;
}

const std::string &sourcePath(SourceId id) { return sourcePaths.at(id); }

void freezeSources(bool frozen) { sourcesFrozen.store(frozen); }

namespace {
// Literal text lives in fixed blocks behind a directory that never moves, so
// a reader needs no lock: an index is only handed out after its string is
// stored, and other threads get indexes through tokens passed to them.
struct LiteralTable {
  static constexpr std::uint32_t BlockBits = 12, BlockSize = 1u << BlockBits, MaxBlocks = 1u << 14;
  std::mutex lock;
  std::unordered_map<std::string, std::uint32_t> ids;
  std::uint32_t count = 0;
  std::unique_ptr<std::string[]> blocks[MaxBlocks];
};

LiteralTable &literalTable() {
  static LiteralTable table;
  return table;
}

const std::string *spellings() {
  static const std::string table[] = {
      std::string(), // Tok::Literal has no spelling of its own.
#define DUCK_TOKEN_TEXT(name, text) std::string(text),
      DUCK_TOKEN_SPELLINGS(DUCK_TOKEN_TEXT)
#undef DUCK_TOKEN_TEXT
  };
  return table;
}

const std::unordered_map<std::string, Tok> &spellingKinds() {
  static const auto kinds = [] {
    std::unordered_map<std::string, Tok> map;
#define DUCK_TOKEN_KIND(name, text) map.emplace(text, Tok::name);
    DUCK_TOKEN_SPELLINGS(DUCK_TOKEN_KIND)
#undef DUCK_TOKEN_KIND
    return map;
  }();
  return kinds;
}
} // namespace

const std::string &TokenText::literalText(std::uint32_t literal) {
  const auto &table = literalTable();
  return table.blocks[literal >> LiteralTable::BlockBits][literal & (LiteralTable::BlockSize - 1)];
}

const std::string &TokenText::spelling(Tok kind) { return spellings()[static_cast<size_t>(kind)]; }

TokenText TokenText::intern(const std::string &text) {
  if (const auto found = spellingKinds().find(text); found != spellingKinds().end()) return TokenText(found->second);

  auto &table = literalTable();
  std::lock_guard<std::mutex> guard(table.lock);

  if (const auto found = table.ids.find(text); found != table.ids.end()) return TokenText(Tok::Literal, found->second);

  const auto id = table.count;
  const auto block = id >> LiteralTable::BlockBits;

  if (block >= LiteralTable::MaxBlocks) throw std::logic_error("too many distinct token texts");

  if (!table.blocks[block]) table.blocks[block].reset(new std::string[LiteralTable::BlockSize]);

  table.blocks[block][id & (LiteralTable::BlockSize - 1)] = text;
  table.ids.emplace(text, id);
  ++table.count;
  return TokenText(Tok::Literal, id);
}

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
  const SourceId sourceName;
  std::vector<Token> out;
  std::string text;      // The token being read; interned once it is complete.
  TokenTextCache cache;
  size_t i = 0;
  int line = 1, col = 1;

  bool atEnd() const { return i >= source.size(); }
  bool nextIs(char c) const { return !atEnd() && source[i] == c; }
  bool startsWith(const char *prefix) const { return source.compare(i, 2, prefix) == 0; }
  // An error in the token being read shows the text read so far, as the
  // source line printer needs a token with text.
  [[noreturn]] void failAt(Token t, const std::string &message) {
    t.text = cache.intern(text);
    fail(t, message);
  }
  Token startToken() const { return Token{Tok::Empty, line, col, false, sourceName}; }

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
    while (!atEnd() && matches(source[i])) text += take();
  }

  void skipLineComment() {
    while (!atEnd() && source[i] != '\n') take();
  }

  void skipBlockComment() {
    const Token start = startToken();
    take();
    take();

    while (!atEnd() && !startsWith("*/")) {
      if (source[i] == '\n') out.push_back({Tok::Newline, line, col, false, sourceName});

      take();
    }

    if (atEnd()) fail(start, "unterminated comment");

    take();
    take();
  }

  void unicodeEscape(Token &t, int digits) {
    uint32_t codePoint = 0;

    for (int n = 0; n < digits; ++n) {
      if (atEnd()) failAt(t, "incomplete Unicode escape");

      const int digit = digitValue(take());

      if (digit < 0) failAt(t, "invalid Unicode escape");

      codePoint = (codePoint << 4) | static_cast<uint32_t>(digit);
    }

    if (codePoint > 0x10FFFF || (codePoint >= 0xD800 && codePoint <= 0xDFFF))
      failAt(t, "Unicode escape must be a scalar value");

    appendUTF8(text, codePoint);
  }

  void escapeSequence(Token &t) {
    if (atEnd()) failAt(t, "unterminated string literal");

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
      failAt(t, "unsupported escape sequence");
    }

    text += c;
  }

  // A character literal shares string escapes but must hold one scalar.
  void textLiteral(Token &t, char quote) {
    t.string = true;
    t.character = quote == '\'';
    take();

    while (!atEnd() && source[i] != quote) {
      const char c = take();

      if (c == '\n' || c == '\r')
        failAt(t, t.character ? "newline in character literal" : "newline in string literal");

      if (c == '\\')
        escapeSequence(t);
      else
        text += c;
    }

    if (atEnd()) failAt(t, t.character ? "unterminated character literal" : "unterminated string literal");

    take();

    t.text = cache.intern(text);

    if (decodeUTF8(t).size() != 1 && t.character)
      failAt(t, "character literal must contain exactly one Unicode scalar");
  }

  void exponent(Token &t) {
    text += take();

    if (nextIs('+') || nextIs('-')) text += take();

    if (atEnd() || !isDigit(source[i])) failAt(t, "exponent requires digits");

    takeWhile(t, isDigit);
  }

  void number(Token &t) {
    // Consume a prefixed integer as one token, including invalid digits, so
    // semantic diagnostics point to the complete literal. Hex e/E is a digit,
    // never a decimal exponent marker.
    if (i + 1 < source.size() && integerLiteralBase(source.substr(i, 2)) != 10) {
      text += take();
      text += take();
      takeWhile(t, isIdentifierChar);
      return;
    }

    takeWhile(t, isDigit);

    if (i + 1 < source.size() && source[i] == '.' && isDigit(source[i + 1])) {
      text += take();
      takeWhile(t, isDigit);
    }

    if (nextIs('e') || nextIs('E')) exponent(t);

    takeWhile(t, isIdentifierChar);
  }

  void word(Token &t) {
    takeWhile(t, isIdentifierChar);

    if (text == "struct" || text == "class")
      failAt(t, "'" + text + "' is reserved in Duck. Use the container name directly");
  }

  void punctuation(Token &t) {
    text += take();

    if (text == "." && source.compare(i, 2, "..") == 0) {
      text += take();
      text += take();
      return;
    }

    if (text == "." && nextIs('.')) {
      text += take(); // The duck call operator.
      return;
    }

    if (atEnd()) return;

    if (isTwoCharOperator(text + source[i])) text += take();

    if ((text == "<<" || text == ">>") && nextIs('=')) text += take();
  }

  void token(char c) {
    Token t = startToken();
    text.clear();

    if (c == '"' || c == '\'')
      textLiteral(t, c);
    else if (isDigit(c))
      number(t);
    else if (isIdentifierChar(c))
      word(t);
    else
      punctuation(t);

    t.text = cache.intern(text);
    out.push_back(std::move(t));
  }

public:
  Lexer(const std::string &source, SourceId sourceName) : source(source), sourceName(sourceName) {}

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

    out.push_back({Tok::Eof, line, col, false, sourceName});
    return std::move(out);
  }
};

} // namespace

std::vector<Token> lex(const std::string &source, SourceId sourceName) {
  return Lexer(source, sourceName).run();
}

} // namespace duck

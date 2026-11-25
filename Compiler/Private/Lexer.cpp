#include "Literals.h"
#include "Pipeline.h"
#include "Unicode.h"
#include <atomic>
#include <algorithm>
#include <cctype>
#include <cstring>
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

// A set of bytes: testing a character is one load, not <cctype> calls or a
// chain of comparisons.
struct ByteSet {
  bool has[256] = {};

  template <class Pick> constexpr explicit ByteSet(Pick pick) {
    for (int c = 0; c < 256; ++c) has[c] = pick(c);
  }
  bool operator()(char c) const { return has[static_cast<unsigned char>(c)]; }
};

constexpr ByteSet digits([](int c) { return c >= '0' && c <= '9'; });
constexpr ByteSet identifierBytes([](int c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
});
// Bytes a text literal copies as they are. Any other ends a run: a quote, an
// escape, a line break, or the '\0' that may be the end of the source.
constexpr ByteSet plainTextBytes(
    [](int c) { return c != '"' && c != '\'' && c != '\\' && c != '\n' && c != '\r' && c != 0; });

// How many bytes each fixed spelling takes, so an operator's kind also says
// how far to move past it.
constexpr std::uint8_t spellingLengths[] = {
    0, // Tok::Literal
#define DUCK_TOKEN_LENGTH(name, text) static_cast<std::uint8_t>(sizeof(text) - 1),
    DUCK_TOKEN_SPELLINGS(DUCK_TOKEN_LENGTH)
#undef DUCK_TOKEN_LENGTH
};

// Turns one file's bytes into tokens in a single pass. The byte at `p` picks
// what is read, and each later byte only checks what can still follow it, so
// a fixed spelling becomes its Tok without any text being built or looked up.
// The source's terminating '\0' is a sentinel: a run of letters, digits or
// text stops there without a separate end check, and only a '\0' byte asks
// whether it is the end. Newlines are tokens, including the ones inside block
// comments, so statements keep their line structure.
class Lexer {
  const SourceId sourceName;
  const char *p;         // The next byte.
  const char *const end; // The terminating '\0'.
  const char *lineStart; // A column is the distance from here, plus one.
  int line = 1;
  std::vector<Token> out;
  std::string text; // A text literal's decoded bytes, or a word's or number's to intern.
  TokenTextCache cache;

  Token tokenAt(const char *at, TokenText kind = Tok::Empty) const {
    return Token{kind, line, static_cast<int>(at - lineStart) + 1, false, sourceName};
  }

  void emit(const char *at, TokenText kind) { out.push_back(tokenAt(at, kind)); }

  // A fixed spelling at `p`, moved past by its length.
  void op(Tok kind) {
    emit(p, kind);
    p += spellingLengths[static_cast<size_t>(kind)];
  }

  void newline() {
    emit(p, Tok::Newline);
    lineStart = ++p;
    ++line;
  }

  // A byte that starts no token is a one-byte literal; the parser rejects it.
  void oneByte() {
    text.assign(1, *p);
    emit(p, cache.intern(text));
    ++p;
  }

  // An error in the token being read shows the text read so far, as the
  // source line printer needs a token with text.
  [[noreturn]] void failAt(Token t, const std::string &soFar, const std::string &message) {
    t.text = cache.intern(soFar);
    fail(t, message);
  }

  void lineComment() {
    const auto *found = static_cast<const char *>(std::memchr(p, '\n', static_cast<size_t>(end - p)));
    p = found ? found : end;
  }

  void blockComment() {
    const Token start = tokenAt(p);
    p += 2;

    for (;;) {
      const char c = *p;

      if (c == '*' && p[1] == '/') break;

      if (c == '\n')
        newline();
      else if (c == '\0' && p == end)
        fail(start, "unterminated comment");
      else
        ++p;
    }

    p += 2;
  }

  void word() {
    const char *const start = p;

    while (identifierBytes(*++p)) {
    }

    text.assign(start, p);
    Token t = tokenAt(start, cache.intern(text));

    if (t.text == Tok::Struct || t.text == Tok::Class)
      fail(t, "'" + text + "' is reserved in Duck. Use the container name directly");

    out.push_back(std::move(t));
  }

  void number() {
    const char *const start = p;
    const char prefix = static_cast<char>(p[1] | 0x20);

    // A prefixed integer is one token up to the end of its word, invalid
    // digits included, so semantic diagnostics point to the whole literal.
    // Hex e/E is a digit, never a decimal exponent marker.
    if (*p == '0' && (prefix == 'x' || prefix == 'b'))
      p += 2;
    else {
      while (digits(*++p)) {
      }

      if (*p == '.' && digits(p[1]))
        while (digits(*++p)) {
        }

      if ((*p | 0x20) == 'e') {
        ++p;

        if (*p == '+' || *p == '-') ++p;

        if (!digits(*p)) failAt(tokenAt(start), std::string(start, p), "exponent requires digits");

        while (digits(*++p)) {
        }
      }
    }

    while (identifierBytes(*p)) ++p;

    text.assign(start, p);
    emit(start, cache.intern(text));
  }

  void unicodeEscape(const Token &t, int count) {
    std::uint32_t codePoint = 0;

    for (int n = 0; n < count; ++n, ++p) {
      if (p == end) failAt(t, text, "incomplete Unicode escape");

      const int digit = digitValue(*p);

      if (digit < 0) failAt(t, text, "invalid Unicode escape");

      codePoint = (codePoint << 4) | static_cast<std::uint32_t>(digit);
    }

    if (codePoint > 0x10FFFF || (codePoint >= 0xD800 && codePoint <= 0xDFFF))
      failAt(t, text, "Unicode escape must be a scalar value");

    appendUTF8(text, codePoint);
  }

  void escapeSequence(const Token &t) {
    char c = *p;

    if (c == '\0' && p == end) failAt(t, text, "unterminated string literal");

    ++p;

    switch (c) {
    case 'u': return unicodeEscape(t, 4);
    case 'U': return unicodeEscape(t, 8);
    case '0': c = '\0'; break;
    case 'n': c = '\n'; break;
    case 'r': c = '\r'; break;
    case 't': c = '\t'; break;
    case '\\':
    case '"':
    case '\'': break;
    default: failAt(t, text, "unsupported escape sequence");
    }

    text += c;
  }

  // A string, or a character literal, which shares its escapes but must hold
  // one scalar. Plain bytes are copied a run at a time.
  void textLiteral(const char quote) {
    Token t = tokenAt(p);
    t.string = true;
    t.character = quote == '\'';
    text.clear();
    ++p;

    for (;;) {
      const char *const run = p;

      while (plainTextBytes(*p)) ++p;

      text.append(run, p);
      const char c = *p;

      if (c == quote) break;

      if (c == '\\') {
        ++p;
        escapeSequence(t);
      } else if (c == '\n' || c == '\r')
        failAt(t, text, t.character ? "newline in character literal" : "newline in string literal");
      else if (c == '\0' && p == end)
        failAt(t, text, t.character ? "unterminated character literal" : "unterminated string literal");
      else { // The other quote, or a '\0' byte inside the source.
        text += c;
        ++p;
      }
    }

    ++p;
    t.text = cache.intern(text);

    // ASCII text is valid as it is; anything else is decoded to be checked.
    const bool ascii = std::all_of(text.begin(), text.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; });

    if (!ascii || t.character) {
      const size_t scalars = ascii ? text.size() : decodeUTF8(t).size();

      if (t.character && scalars != 1) failAt(t, text, "character literal must contain exactly one Unicode scalar");
    }

    out.push_back(std::move(t));
  }

public:
  // Duck source averages one token per 4.5 bytes (878k tokens in 3.9 MB of
  // .dk files), so a quarter of the size is room for almost every file in one
  // allocation, without a large block for a small one.
  static constexpr size_t BytesPerToken = 4, SpareTokens = 16;

  Lexer(const std::string &source, SourceId sourceName)
      : sourceName(sourceName), p(source.data()), end(source.data() + source.size()), lineStart(p) {
    out.reserve(source.size() / BytesPerToken + SpareTokens);
  }

  std::vector<Token> run() {
    for (;;) {
      switch (static_cast<unsigned char>(*p)) {
      case ' ':
      case '\t':
      case '\r': ++p; break;
      case '\n': newline(); break;
      case '\0':
        if (p == end) {
          emit(p, Tok::Eof);
          return std::move(out);
        }

        oneByte();
        break;
      case '"':
      case '\'': textLiteral(*p); break;
      case '0': case '1': case '2': case '3': case '4':
      case '5': case '6': case '7': case '8': case '9': number(); break;
      case '/':
        if (p[1] == '/')
          lineComment();
        else if (p[1] == '*')
          blockComment();
        else
          op(p[1] == '=' ? Tok::SlashAssign : Tok::Slash);
        break;
      // `..` is the duck call operator.
      case '.': op(p[1] != '.' ? Tok::Dot : p[2] == '.' ? Tok::Ellipsis : Tok::DotDot); break;
      case ':': op(p[1] == ':' ? Tok::ColonColon : Tok::Colon); break;
      case '=': op(p[1] == '=' ? Tok::EqualEqual : Tok::Assign); break;
      case '!': op(p[1] == '=' ? Tok::NotEqual : Tok::Bang); break;
      case '+': op(p[1] == '+' ? Tok::PlusPlus : p[1] == '=' ? Tok::PlusAssign : Tok::Plus); break;
      case '-': op(p[1] == '-' ? Tok::MinusMinus : p[1] == '=' ? Tok::MinusAssign : Tok::Minus); break;
      case '*': op(p[1] == '=' ? Tok::StarAssign : Tok::Star); break;
      case '%': op(p[1] == '=' ? Tok::PercentAssign : Tok::Percent); break;
      case '^': op(p[1] == '=' ? Tok::CaretAssign : Tok::Caret); break;
      case '&': op(p[1] == '&' ? Tok::AndAnd : p[1] == '=' ? Tok::AmpersandAssign : Tok::Ampersand); break;
      case '|': op(p[1] == '|' ? Tok::OrOr : p[1] == '=' ? Tok::PipeAssign : Tok::Pipe); break;
      case '<':
        op(p[1] == '<'   ? (p[2] == '=' ? Tok::ShiftLeftAssign : Tok::ShiftLeft)
           : p[1] == '=' ? Tok::LessEqual
                         : Tok::Less);
        break;
      case '>':
        op(p[1] == '>'   ? (p[2] == '=' ? Tok::ShiftRightAssign : Tok::ShiftRight)
           : p[1] == '=' ? Tok::GreaterEqual
                         : Tok::Greater);
        break;
      case '#': op(Tok::Hash); break;
      case '$': op(Tok::Dollar); break;
      case '(': op(Tok::LeftParen); break;
      case ')': op(Tok::RightParen); break;
      case ',': op(Tok::Comma); break;
      case ';': op(Tok::Semicolon); break;
      case '?': op(Tok::Question); break;
      case '@': op(Tok::At); break;
      case '[': op(Tok::LeftBracket); break;
      case '\\': op(Tok::Backslash); break;
      case ']': op(Tok::RightBracket); break;
      case '`': op(Tok::Backtick); break;
      case '{': op(Tok::LeftBrace); break;
      case '}': op(Tok::RightBrace); break;
      case '~': op(Tok::Tilde); break;
      default:
        if (identifierBytes(*p))
          word();
        else
          oneByte();
      }
    }
  }
};

} // namespace

std::vector<Token> lex(const std::string &source, SourceId sourceName) {
  return Lexer(source, sourceName).run();
}

} // namespace duck

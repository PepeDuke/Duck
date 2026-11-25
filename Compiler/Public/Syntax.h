#pragma once
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <ostream>
#include <unordered_map>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace duck {
// Source file paths are stored once, in a table shared by the whole process,
// and a token names its file by index into it. Index 0 is the empty path of
// text that came from no file, and a path keeps its index for the life of the
// process. The table has no lock: paths are registered only while one thread
// runs, and while worker threads run it is frozen, so they only read it.
using SourceId = std::uint32_t;
// Adds a path, or gives the index it already has. Adding one while the table
// is frozen is a compiler bug, and throws std::logic_error.
SourceId registerSource(const std::string &path);
// The index of a path registered earlier. A path never registered is a
// compiler bug, and throws std::logic_error.
SourceId findSource(const std::string &path);
const std::string &sourcePath(SourceId id);
// Brackets the parallel phases: worker threads may read the table only while it is frozen.
void freezeSources(bool frozen);

// Every fixed spelling a token can have: punctuation, keywords and type
// words. Any other text (names, numbers, string contents) is a Literal and
// lives once in a shared table. The order is the .dki numbering, so adding or
// moving an entry changes the interface format; interfaces are rebuilt anyway.
#define DUCK_TOKEN_SPELLINGS(X)                                                                                  \
  X(Empty, "") X(Newline, "\n") X(Eof, "<eof>")                                                                  \
  X(Bang, "!") X(Hash, "#") X(Dollar, "$") X(Percent, "%") X(Ampersand, "&") X(LeftParen, "(")                   \
  X(RightParen, ")") X(Star, "*") X(Plus, "+") X(Comma, ",") X(Minus, "-") X(Dot, ".") X(Slash, "/")            \
  X(Colon, ":") X(Semicolon, ";") X(Less, "<") X(Assign, "=") X(Greater, ">") X(Question, "?") X(At, "@")        \
  X(LeftBracket, "[") X(Backslash, "\\") X(RightBracket, "]") X(Caret, "^") X(Backtick, "`")                    \
  X(LeftBrace, "{") X(Pipe, "|") X(RightBrace, "}") X(Tilde, "~")                                               \
  X(ColonColon, "::") X(EqualEqual, "==") X(NotEqual, "!=") X(LessEqual, "<=") X(GreaterEqual, ">=")            \
  X(PlusPlus, "++") X(MinusMinus, "--") X(AndAnd, "&&") X(OrOr, "||") X(PlusAssign, "+=")                       \
  X(MinusAssign, "-=") X(StarAssign, "*=") X(SlashAssign, "/=") X(PercentAssign, "%=")                          \
  X(AmpersandAssign, "&=") X(PipeAssign, "|=") X(CaretAssign, "^=") X(ShiftLeft, "<<") X(ShiftRight, ">>")      \
  X(ShiftLeftAssign, "<<=") X(ShiftRightAssign, ">>=") X(DotDot, "..") X(Ellipsis, "...")                      \
  X(If, "if") X(Else, "else") X(For, "for") X(While, "while") X(Do, "do") X(Return, "return")                   \
  X(Break, "break") X(Continue, "continue") X(Switch, "Switch") X(Case, "case") X(Default, "default")           \
  X(Using, "using") X(Extern, "extern") X(Macro, "macro") X(True, "true") X(False, "false") X(Null, "null")      \
  X(This, "this") X(Define, "define") X(Struct, "struct") X(Class, "class")                                    \
  X(Void, "void") X(Bool, "bool") X(Byte, "byte") X(Short, "short") X(Int, "int") X(Long, "long")               \
  X(Float, "float") X(Double, "double") X(Name, "name") X(String, "string") X(Cstring, "cstring")               \
  X(Char, "char") X(Uint, "uint") X(Int8, "int8") X(Int16, "int16") X(Int32, "int32") X(Int64, "int64")         \
  X(Uint8, "uint8") X(Uint16, "uint16") X(Uint32, "uint32") X(Uint64, "uint64") X(Date, "date")                \
  X(Time, "time") X(Datetime, "datetime") X(Big, "big") X(Ubig, "ubig") X(Tiny, "tiny") X(Utiny, "utiny")       \
  X(Func, "func") X(File, "file") X(Markup, "markup") X(Element, "element") X(Vec2, "vec2") X(Vec3, "vec3")     \
  X(Vec4, "vec4") X(Dvec2, "dvec2") X(Dvec3, "dvec3") X(Dvec4, "dvec4") X(Ivec2, "ivec2") X(Ivec3, "ivec3")     \
  X(Ivec4, "ivec4") X(Mat3, "mat3") X(Mat4, "mat4") X(Dmat3, "dmat3") X(Dmat4, "dmat4") X(Quat, "quat")         \
  X(Rot, "rot")                                                                                                  \
  /* Names the compiler looks for or generates: identifiers, not reserved words. */                              \
  X(Main, "Main") X(Print, "Print") X(Exec, "Exec") X(Status, "Status") X(ThreadSlots, "ThreadSlots")            \
  X(ThreadCount, "ThreadCount") X(Thread, "Thread") X(P, "P") X(T, "T") X(C, "C") X(Len, "Len")                  \
  X(Length, "Length") X(CodeUnitAt, "CodeUnitAt") X(IsOpen, "IsOpen") X(CountProperty, "Count") X(Min, "Min")    \
  X(Max, "Max") X(NameProperty, "Name") X(Value, "Value") X(ImplicitValue, "value") X(AreEqual, "AreEqual")      \
  X(Fail, "Fail") X(IsTrue, "IsTrue") X(Start, "start") X(End, "end") X(Args, "args") X(Pair, "pair")            \
  X(Queue, "queue") X(Stack, "stack") X(Lambda, "$lambda") X(Children, "$children") X(Self, "$self")             \
  X(HeldScope, "$scope") X(HeldUse, "$use") X(Ordered, "$ordered") X(SelfMarkup, "$self.$markup")                \
  X(FirstScript, "$exec.0") X(MacroValue, "$value") X(MacroResult, "$result") X(MacroEnd, "$end")                \
  X(AddDays, "AddDays") X(DateOf, "DateOf") X(Day, "Day") X(DayOfWeek, "DayOfWeek")                              \
  X(DaysBetween, "DaysBetween") X(Hour, "Hour") X(IsValid, "IsValid") X(Minute, "Minute") X(Month, "Month")      \
  X(Nanosecond, "Nanosecond") X(Second, "Second") X(TimeOf, "TimeOf") X(Year, "Year") X(TestFail, "$T.fail")     \
  X(TestShow, "$T.show") X(Clear, "Clear") X(CellAssign, "[...] =")                                              \
  X(Net, "Net") X(NetReceiveEvent, "NetReceiveEvent") X(NetReceiveSettings, "NetReceiveSettings")                \
  X(NetIncoming, "NetIncoming") X(WebTransportSession, "WebTransportSession")                                    \
  X(WebTransportStream, "WebTransportStream") X(TcpListener, "TcpListener") X(TcpConnection, "TcpConnection")    \
  X(UdpSocket, "UdpSocket") X(UdpEndpoint, "UdpEndpoint")                                                     \
  X(Task, "task") X(Stopping, "Stopping") X(Stop, "Stop") X(Join, "Join") X(Done, "Done")

enum class Tok : std::uint16_t {
  Literal, // Text from the literal table.
#define DUCK_TOKEN_ENUM(name, text) name,
  DUCK_TOKEN_SPELLINGS(DUCK_TOKEN_ENUM)
#undef DUCK_TOKEN_ENUM
  Count
};

// What an Expr or a Stmt is. The spellings are only for diagnostics.
#define DUCK_EXPR_KINDS(X)                                                                                       \
  X(None, "") X(Binary, "binary") X(Bool, "bool") X(Call, "call") X(Cast, "cast") X(Char, "char")                \
  X(CompileTime, "compileTime") X(Conditional, "conditional") X(Construct, "construct") X(DuckCall, "duckCall")  \
  X(DuckCheck, "duckCheck") X(DuckExists, "duckExists") X(Entry, "entry")                                        \
  X(FieldInitializer, "fieldInitializer") X(Guard, "guard") X(Index, "index") X(Initializer, "initializer")      \
  X(Int, "int") X(Invoke, "invoke") X(Lambda, "lambda") X(MacroValue, "macroValue") X(MarkupTake, "markupTake")  \
  X(MoveCheck, "moveCheck") X(Mutation, "mutation") X(Name, "name") X(Null, "null") X(Range, "range")            \
  X(Real, "real") X(Removal, "removal") X(ScopeActive, "scopeActive") X(Text, "text")                            \
  X(TypeMember, "typeMember") X(Unary, "unary") X(Member, ".") X(Detached, "detached")
#define DUCK_STMT_KINDS(X)                                                                                       \
  X(None, "") X(Assign, "assign") X(Block, "block") X(Break, "break") X(Case, "case") X(Continue, "continue")    \
  X(Do, "do") X(Expr, "expr") X(For, "for") X(ForEach, "foreach") X(If, "if") X(Let, "let")                      \
  X(MarkupAdd, "markupAdd") X(MarkupSplice, "markupSplice") X(MoveInto, "moveInto") X(Ordered, "ordered")        \
  X(Own, "own") X(Parallel, "parallel") X(RangeFor, "rangefor") X(Return, "return")                              \
  X(ReturnOwner, "returnOwner") X(ScopedBody, "scopedBody") X(ScopeEnd, "scopeEnd") X(ScopeStart, "scopeStart")  \
  X(Switch, "switch") X(While, "while") X(PercentAssign, "%=") X(AmpersandAssign, "&=") X(StarAssign, "*=")      \
  X(Increment, "++") X(PlusAssign, "+=") X(Decrement, "--") X(MinusAssign, "-=") X(SlashAssign, "/=")            \
  X(ShiftLeftAssign, "<<=") X(ShiftRightAssign, ">>=") X(CaretAssign, "^=") X(PipeAssign, "|=")

enum class ExprKind : std::uint8_t {
#define DUCK_KIND_ENUM(name, text) name,
  DUCK_EXPR_KINDS(DUCK_KIND_ENUM)
};
enum class StmtKind : std::uint8_t {
  DUCK_STMT_KINDS(DUCK_KIND_ENUM)
#undef DUCK_KIND_ENUM
};
inline const char *kindName(ExprKind kind) {
#define DUCK_KIND_TEXT(name, text) text,
  static const char *const names[] = {DUCK_EXPR_KINDS(DUCK_KIND_TEXT)};
  return names[static_cast<size_t>(kind)];
}
inline const char *kindName(StmtKind kind) {
  static const char *const names[] = {DUCK_STMT_KINDS(DUCK_KIND_TEXT)};
#undef DUCK_KIND_TEXT
  return names[static_cast<size_t>(kind)];
}
// The statement an assignment operator makes: `x += 1` is a PlusAssign.
inline StmtKind assignmentKind(Tok op) {
  switch (op) {
  case Tok::Assign: return StmtKind::Assign;
  case Tok::PlusAssign: return StmtKind::PlusAssign;
  case Tok::MinusAssign: return StmtKind::MinusAssign;
  case Tok::StarAssign: return StmtKind::StarAssign;
  case Tok::SlashAssign: return StmtKind::SlashAssign;
  case Tok::PercentAssign: return StmtKind::PercentAssign;
  case Tok::AmpersandAssign: return StmtKind::AmpersandAssign;
  case Tok::PipeAssign: return StmtKind::PipeAssign;
  case Tok::CaretAssign: return StmtKind::CaretAssign;
  case Tok::ShiftLeftAssign: return StmtKind::ShiftLeftAssign;
  case Tok::ShiftRightAssign: return StmtKind::ShiftRightAssign;
  case Tok::PlusPlus: return StmtKind::Increment;
  case Tok::MinusMinus: return StmtKind::Decrement;
  default: return StmtKind::None;
  }
}
// The operator an assignment statement was written with, or Empty for any
// other statement.
inline Tok assignmentOperator(StmtKind kind) {
  switch (kind) {
  case StmtKind::Assign: return Tok::Assign;
  case StmtKind::PlusAssign: return Tok::PlusAssign;
  case StmtKind::MinusAssign: return Tok::MinusAssign;
  case StmtKind::StarAssign: return Tok::StarAssign;
  case StmtKind::SlashAssign: return Tok::SlashAssign;
  case StmtKind::PercentAssign: return Tok::PercentAssign;
  case StmtKind::AmpersandAssign: return Tok::AmpersandAssign;
  case StmtKind::PipeAssign: return Tok::PipeAssign;
  case StmtKind::CaretAssign: return Tok::CaretAssign;
  case StmtKind::ShiftLeftAssign: return Tok::ShiftLeftAssign;
  case StmtKind::ShiftRightAssign: return Tok::ShiftRightAssign;
  case StmtKind::Increment: return Tok::PlusPlus;
  case StmtKind::Decrement: return Tok::MinusMinus;
  default: return Tok::Empty;
  }
}
inline bool assignment(StmtKind kind) { return assignmentOperator(kind) != Tok::Empty; }
// The binary operator a compound assignment applies: + for += and ++.
inline Tok compoundOperator(StmtKind kind) {
  switch (kind) {
  case StmtKind::PlusAssign:
  case StmtKind::Increment: return Tok::Plus;
  case StmtKind::MinusAssign:
  case StmtKind::Decrement: return Tok::Minus;
  case StmtKind::StarAssign: return Tok::Star;
  case StmtKind::SlashAssign: return Tok::Slash;
  case StmtKind::PercentAssign: return Tok::Percent;
  case StmtKind::AmpersandAssign: return Tok::Ampersand;
  case StmtKind::PipeAssign: return Tok::Pipe;
  case StmtKind::CaretAssign: return Tok::Caret;
  case StmtKind::ShiftLeftAssign: return Tok::ShiftLeft;
  case StmtKind::ShiftRightAssign: return Tok::ShiftRight;
  default: return Tok::Empty;
  }
}

// A token's text: a fixed spelling, or an index into the shared literal table.
// The same text always gets the same value, so comparing two of them compares
// two integers. It reads like a const std::string where code needs one.
// Adding to the literal table takes a lock; reading it never does.
class TokenText {
  Tok kind_ = Tok::Empty;
  std::uint32_t literal_ = 0; // Meaningful only for Tok::Literal.

  TokenText(Tok kind, std::uint32_t literal) : kind_(kind), literal_(literal) {}
  static const std::string &literalText(std::uint32_t literal);
  static const std::string &spelling(Tok kind);

public:
  TokenText() = default;
  TokenText(Tok kind) : kind_(kind) {}
  TokenText(const std::string &text) : TokenText(intern(text)) {}
  TokenText(const char *) = delete; // A known spelling is a Tok.
  static TokenText intern(const std::string &text);

  Tok kind() const { return kind_; }
  std::uint32_t literal() const { return literal_; }
  const std::string &str() const { return kind_ == Tok::Literal ? literalText(literal_) : spelling(kind_); }
  operator const std::string &() const { return str(); }

  bool empty() const { return kind_ == Tok::Empty; }
  size_t size() const { return str().size(); }
  size_t length() const { return str().size(); }
  const char *c_str() const { return str().c_str(); }
  char front() const { return str().front(); }
  char back() const { return str().back(); }
  char operator[](size_t i) const { return str()[i]; }
  std::string substr(size_t pos = 0, size_t count = std::string::npos) const { return str().substr(pos, count); }
  size_t find(const std::string &s, size_t pos = 0) const { return str().find(s, pos); }
  size_t find(char c, size_t pos = 0) const { return str().find(c, pos); }
  size_t rfind(const std::string &s, size_t pos = std::string::npos) const { return str().rfind(s, pos); }
  size_t rfind(char c, size_t pos = std::string::npos) const { return str().rfind(c, pos); }
  size_t find_first_of(const char *chars, size_t pos = 0) const { return str().find_first_of(chars, pos); }
  size_t find_first_not_of(const char *chars, size_t pos = 0) const { return str().find_first_not_of(chars, pos); }
  int compare(const std::string &s) const { return str().compare(s); }
  int compare(size_t pos, size_t count, const std::string &s) const { return str().compare(pos, count, s); }
  std::string::const_iterator begin() const { return str().begin(); }
  std::string::const_iterator end() const { return str().end(); }
  TokenText &operator+=(const std::string &s) { return *this = intern(str() + s); }
  TokenText &operator+=(char c) { return *this = intern(str() + c); }

  // Against a fixed spelling: one integer comparison.
  friend bool operator==(const TokenText &a, Tok b) { return a.kind_ == b; }
  friend bool operator!=(const TokenText &a, Tok b) { return a.kind_ != b; }
  friend bool operator==(const TokenText &a, const TokenText &b) {
    return a.kind_ == b.kind_ && a.literal_ == b.literal_;
  }
  friend bool operator!=(const TokenText &a, const TokenText &b) { return !(a == b); }
  friend bool operator==(const TokenText &a, const std::string &b) { return a.str() == b; }
  friend bool operator!=(const TokenText &a, const std::string &b) { return a.str() != b; }
  friend bool operator==(const std::string &a, const TokenText &b) { return a == b.str(); }
  friend bool operator!=(const std::string &a, const TokenText &b) { return a != b.str(); }
  // Against a quoted string: a known spelling is a Tok, so write that instead.
  friend bool operator==(const TokenText &, const char *) = delete;
  friend bool operator!=(const TokenText &, const char *) = delete;
  friend bool operator==(const char *, const TokenText &) = delete;
  friend bool operator!=(const char *, const TokenText &) = delete;
  friend bool operator<(const TokenText &a, const TokenText &b) { return a.str() < b.str(); }
  friend std::string operator+(const TokenText &a, const TokenText &b) { return a.str() + b.str(); }
  friend std::string operator+(const TokenText &a, const std::string &b) { return a.str() + b; }
  friend std::string operator+(const std::string &a, const TokenText &b) { return a + b.str(); }
  friend std::string operator+(std::string &&a, const TokenText &b) { return std::move(a) + b.str(); }
  friend std::string operator+(const TokenText &a, const char *b) { return a.str() + b; }
  friend std::string operator+(const char *a, const TokenText &b) { return a + b.str(); }
  friend std::string operator+(const TokenText &a, char b) { return a.str() + b; }
  friend std::string operator+(char a, const TokenText &b) { return a + b.str(); }
  friend std::ostream &operator<<(std::ostream &out, const TokenText &t) { return out << t.str(); }
};

// A Tok is a token spelling. Comparing it with a plain string is a mistake:
// the string is not token text, so the comparison would only look like one.
bool operator==(const std::string &, Tok) = delete;
bool operator!=(const std::string &, Tok) = delete;
bool operator==(Tok, const std::string &) = delete;
bool operator!=(Tok, const std::string &) = delete;

// Interns for one thread, so text it has seen before skips the shared lock.
class TokenTextCache {
  std::unordered_map<std::string, TokenText> seen;

public:
  TokenText intern(const std::string &text) {
    if (const auto found = seen.find(text); found != seen.end()) return found->second;

    const auto interned = TokenText::intern(text);
    seen.emplace(text, interned);
    return interned;
  }
};

struct Token {
  TokenText text;
  int line, column;
  bool string = false;
  SourceId source = 0;
  bool character = false; // A 'x' literal; also marked string so it is never punctuation.
  // Set on tokens copied from a macro body. `mark` is unique per expansion
  // and keeps the names that body declares apart from every other scope.
  unsigned mark = 0;
  std::shared_ptr<const struct Expansion> origin;
};
// One expansion of a macro: where it was called and where it was defined.
struct Expansion {
  Token call;
  std::string macro; // Qualified name.
  Token definition;
  unsigned mark = 0;
};
// "file:line:column" of a token, for diagnostics that point at a second place.
inline std::string where(const Token &t) {
  const auto &path = sourcePath(t.source);
  return (path.empty() ? "<input>" : path) + ":" + std::to_string(t.line) + ":" +
         std::to_string(t.column);
}
// A rejected program. what() is the complete plain text; the token lets the
// driver show the offending source line under the first line of that text.
struct CompileError : std::runtime_error {
  Token token;
  CompileError(const std::string &text, Token at) : std::runtime_error(text), token(std::move(at)) {}
};
// Independent failures found in one pass, in source order.
struct CompileErrors : std::runtime_error {
  std::vector<CompileError> errors;
  static std::string joined(const std::vector<CompileError> &errors) {
    std::string text;

    for (const auto &error : errors) text += (text.empty() ? "" : "\n") + std::string(error.what());

    return text;
  }
  explicit CompileErrors(std::vector<CompileError> found) : std::runtime_error(joined(found)), errors(std::move(found)) {}
};
// A failure inside macro output names every expansion that produced the token.
[[noreturn]] inline void fail(const Token &t, const std::string &message) {
  std::string text = where(t) + ": error: " + message;

  for (const auto *e = t.origin.get(); e; e = e->call.origin.get())
    text += "\n  note: in expansion of macro '" + e->macro + "' defined at " + where(e->definition) +
            ", called at " + where(e->call);

  throw CompileError(text, t);
}
// Case-insensitive optimal string alignment distance: edits, with an adjacent
// swap counted as one, so `coutn` is one edit from `count`.
inline size_t editDistance(std::string a, std::string b) {
  auto lower = [](std::string &s) {
    for (auto &c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  };
  lower(a);
  lower(b);
  std::vector<std::vector<size_t>> d(a.size() + 1, std::vector<size_t>(b.size() + 1));

  for (size_t i = 0; i <= a.size(); ++i) d[i][0] = i;
  for (size_t j = 0; j <= b.size(); ++j) d[0][j] = j;

  for (size_t i = 1; i <= a.size(); ++i)
    for (size_t j = 1; j <= b.size(); ++j) {
      d[i][j] = std::min({d[i - 1][j] + 1, d[i][j - 1] + 1, d[i - 1][j - 1] + (a[i - 1] != b[j - 1])});

      if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1])
        d[i][j] = std::min(d[i][j], d[i - 2][j - 2] + 1);
    }

  return d[a.size()][b.size()];
}
// The candidate a misspelled name most likely meant, or empty when none is
// close. Compiler-generated names, which hold $, - or #, are never offered.
inline std::string closestName(const std::string &name, const std::vector<std::string> &candidates) {
  const size_t limit = std::min<size_t>(3, (name.size() + 1) / 3);
  std::string best;
  size_t bestDistance = limit + 1;

  for (const auto &candidate : candidates) {
    if (candidate == name || candidate.find_first_of("$-#") != std::string::npos) continue;

    const auto distance = editDistance(name, candidate);

    if (distance < bestDistance) {
      best = candidate;
      bestDistance = distance;
    }
  }

  return best;
}
// "; did you mean 'x'?" to append to a message, or nothing.
inline std::string didYouMean(const std::string &name, const std::vector<std::string> &candidates) {
  const auto match = closestName(name, candidates);
  return match.empty() ? "" : "; did you mean '" + match + "'?";
}
struct FuncSignature;
bool sameSignature(const FuncSignature *a, const FuncSignature *b);
struct Type {
  enum Kind {
    Void, Int8, Short, Int, Long, UInt8, UInt16, UInt32, UInt64,
    Float, Double, Bool, Text, String, Name, Record,
    Char, Date, Time, DateTime,
    IntN, UIntN, // Custom byte-width iN/uN; `bits` holds 8*N.
    Big, UBig,   // Arbitrary-precision integers (reference-counted storage).
    Tiny, UTiny, // 69-digit decimal floating point values.
    Enum,        // A #@ enum member; `record` holds the enum name.
    Queue, Stack, Pair, // Collections; `element` (and a pair's `key`) hold their types.
    Array,              // `element` holds the element type; `extent` is 0 when dynamic.
    Func,               // A callable; `signature` is null until inferred from its initializer.
    File,               // An open file handle (the C FILE*), null while closed.
    Task                // A thread started by P{ }: its runtime record, null when none.
  };
  enum Role { Raw, Owner, Alias };
  Role role = Raw;
  Kind base;
  unsigned depth = 0;
  unsigned bits = 0; // Only for IntN and UIntN.
  std::string record;
  // A collection's element type, or a pair's value type, and a pair's key type.
  std::shared_ptr<const Type> element, key;
  std::shared_ptr<const FuncSignature> signature;
  unsigned long long extent = 0; // A fixed array's element count.
  // `T [50*]` and `T [*]`: how many elements a dynamic array is expected to
  // hold. Only the scheduler's cost model reads it, so it is not part of the
  // type's identity and never limits how far the array can grow.
  unsigned long long sizeHint = 0;
  static constexpr unsigned long long HugeHint = ~0ull; // `T [*]`.
  Type(Kind kind = Void) : base(kind) {}
  static Type array(const Type &element, unsigned long long extent) {
    Type t(Array);
    t.element = std::make_shared<const Type>(element);
    t.extent = extent;
    return t;
  }
  // `T [-][-]`: a sparse grid of int coordinates. It is stored as a pair
  // keyed by an [N x int] coordinate tuple, with extent holding N.
  static Type grid(const Type &element, unsigned rank) {
    Type t = collection(Pair, element, array(Int, rank));
    t.extent = rank;
    return t;
  }
  static Type collection(Kind kind, const Type &element, const Type &key = Type()) {
    Type t(kind);
    t.element = std::make_shared<const Type>(element);

    if (kind == Pair) t.key = std::make_shared<const Type>(key);

    return t;
  }
  // iN/uN for 1, 2, 4, and 8 bytes are the exact host-width types.
  static Type integer(unsigned width, bool isUnsigned) {
    switch (width) {
    case 8: return isUnsigned ? UInt8 : Int8;
    case 16: return isUnsigned ? UInt16 : Short;
    case 32: return isUnsigned ? UInt32 : Int;
    case 64: return isUnsigned ? UInt64 : Long;
    }

    Type t(isUnsigned ? UIntN : IntN);
    t.bits = width;
    return t;
  }
  Type pointerTo() const { auto t = *this; ++t.depth; return t; }
  Type pointee() const { auto t = *this; --t.depth; t.role = Raw; return t; }
  bool operator==(Kind kind) const { return !depth && base == kind; }
  bool operator!=(Kind kind) const { return !(*this == kind); }
  bool operator==(const Type &t) const {
    auto same = [](const std::shared_ptr<const Type> &a, const std::shared_ptr<const Type> &b) {
      return a == b || (a && b && *a == *b);
    };
    return role == t.role && base == t.base && depth == t.depth && bits == t.bits &&
           record == t.record && extent == t.extent && same(element, t.element) && same(key, t.key) &&
           sameSignature(signature.get(), t.signature.get());
  }
  bool operator!=(const Type &t) const { return !(*this == t); }
  static const Type Pointer;
};
inline const Type Type::Pointer = Type(Type::Void).pointerTo();
// What a func value takes and gives. Indirect calls are checked against it.
struct FuncParameter {
  Type type;
  bool reference = false;
};
struct FuncSignature {
  Type result;
  std::vector<FuncParameter> parameters;
};
inline bool sameSignature(const FuncSignature *a, const FuncSignature *b) {
  if (a == b) return true;
  if (!a || !b || a->result != b->result || a->parameters.size() != b->parameters.size()) return false;

  for (size_t i = 0; i < a->parameters.size(); ++i)
    if (a->parameters[i].type != b->parameters[i].type ||
        a->parameters[i].reference != b->parameters[i].reference)
      return false;

  return true;
}
// A non-owning heap reference held in a field or collection slot: the address
// of the object's control block, or null. It expires when the owner releases
// the object. A slot holding an owner has role Owner instead.
inline bool trackedAlias(const Type &t) { return t.depth == 1 && t.role == Type::Alias; }
// A func value is one word: a reference-counted closure, or null.
inline bool funcValue(const Type &t) { return !t.depth && t.base == Type::Func; }
inline bool recordValue(const Type &t) { return !t.depth && t.base == Type::Record; }
// The compiler's own vecN, ivecN, dvecN, matN and dmatN value types. They are
// containers the compiler registers, packed like an array of their scalars;
// the type words are reserved, so no program can declare one of these names.
struct NativeMath {
  Type::Kind scalar = Type::Void;
  unsigned columns = 0, rows = 0; // A vector has one column.
  bool quaternion = false, euler = false; // quat and rot, stored like vectors.
  explicit operator bool() const { return rows != 0; }
  bool matrix() const { return columns > 1; }
  // What indexing gives: a vector's scalar, or a matrix's column vector.
  Type element() const {
    if (!matrix()) return scalar;

    Type column(Type::Record);
    column.record = (scalar == Type::Double ? "dvec" : "vec") + std::to_string(rows);
    return column;
  }
};
inline NativeMath nativeMath(const std::string &name) {
  if (name == "quat") return {Type::Float, 1, 4, true, false};
  if (name == "rot") return {Type::Float, 1, 3, false, true};
  if (name.size() < 4 || name.size() > 5) return {};

  const unsigned size = unsigned(name.back() - '0');
  const auto stem = name.substr(0, name.size() - 1);

  if (size >= 2 && size <= 4) {
    if (stem == "vec") return {Type::Float, 1, size};
    if (stem == "ivec") return {Type::Int, 1, size};
    if (stem == "dvec") return {Type::Double, 1, size};
  }

  if (size >= 3 && size <= 4) {
    if (stem == "mat") return {Type::Float, size, size};
    if (stem == "dmat") return {Type::Double, size, size};
  }

  return {};
}
inline NativeMath nativeMath(const Type &t) { return recordValue(t) ? nativeMath(t.record) : NativeMath{}; }
inline Type nativeNamed(const std::string &name) {
  Type t(Type::Record);
  t.record = name;
  return t;
}
// `T [N]` stores N elements inline; `T []` is a handle to growable heap
// storage, null while empty.
inline bool arrayValue(const Type &t) { return !t.depth && t.base == Type::Array; }
inline bool fixedArray(const Type &t) { return arrayValue(t) && t.extent != 0; }
inline bool dynamicArray(const Type &t) { return arrayValue(t) && t.extent == 0; }
// A queue, stack, or pair: a move-only handle to heap storage, null while empty.
inline bool collectionValue(const Type &t) {
  return !t.depth && (t.base == Type::Queue || t.base == Type::Stack || t.base == Type::Pair);
}
// A sparse signed grid, `T [-]` or `T [-][-]`: a pair keyed by coordinates.
inline bool gridValue(const Type &t) { return !t.depth && t.base == Type::Pair && t.extent != 0; }
// A pair written as `K, V pair`, not a grid.
inline bool pairValue(const Type &t) { return !t.depth && t.base == Type::Pair && t.extent == 0; }
// An enum value is stored as its member's declaration index. Printing uses the
// member name; operators, conversions, and concatenation use its underlying value.
inline bool enumValue(const Type &t) { return !t.depth && t.base == Type::Enum; }
// A file is a move-only handle: the C FILE*, null while closed. It closes
// itself when its variable, field, or temporary dies.
inline bool fileValue(const Type &t) { return !t.depth && t.base == Type::File; }
// A task is a move-only handle to a thread started by `P{ }`: its runtime
// record, null when there is none. When its variable, field, or temporary
// dies, the task is asked to stop and then waited for.
inline bool taskValue(const Type &t) { return !t.depth && t.base == Type::Task; }
// A markup value holds a tree's top-level elements: a queue of element nodes.
// Being a collection, it is a move-only handle that is null while empty.
inline Type elementType() {
  Type t = Type::Pointer;
  t.record = "$node";
  return t;
}
inline Type markupType() {
  Type t = Type::collection(Type::Queue, elementType());
  t.extent = 1;
  return t;
}
inline bool markupValue(const Type &t) { return !t.depth && t.base == Type::Queue && t.extent == 1; }
// A borrowed view of one element of a markup tree, from GetChildren().
inline bool elementValue(const Type &t) { return t.depth == 1 && t.base == Type::Void && t.record == "$node"; }
// A variable in a named lifetime scope is stored as a slot of two words, its
// arena address and generation. The slot has no cleanup of its own: the scope
// owns the value, so copying or destroying a container never touches it.
inline Type scopeSlotType() { return Type::array(Type::Long, 2); }
inline bool nativeScalar(const Type &t) {
  return t.depth || t == Type::Int || t == Type::UInt32 || t == Type::Long ||
         t == Type::UInt64 || t == Type::Float || t == Type::Double;
}
// Distinct calendar and character value types. They support comparison but
// no arithmetic, and convert to integers or text only explicitly.
inline bool calendarValue(const Type &t) {
  return t == Type::Char || t == Type::Date || t == Type::Time || t == Type::DateTime;
}
inline bool managedText(const Type &t) { return t == Type::String || t == Type::Name; }
inline bool bigInteger(const Type &t) { return t == Type::Big || t == Type::UBig; }
inline bool decimalValue(const Type &t) { return t == Type::Tiny || t == Type::UTiny; }
inline bool arbitraryNumber(const Type &t) { return bigInteger(t) || decimalValue(t); }
// Values whose storage is reference counted and released at scope exit.
inline bool managedValue(const Type &t) { return managedText(t) || bigInteger(t); }
inline bool textType(const Type &t) { return managedText(t) || t == Type::Text; }
inline bool floating(const Type &t) { return t == Type::Float || t == Type::Double; }
inline bool customInteger(const Type &t) {
  return !t.depth && (t.base == Type::IntN || t.base == Type::UIntN);
}
inline bool unsignedInteger(const Type &t) {
  return t == Type::UInt8 || t == Type::UInt16 ||
         t == Type::UInt32 || t == Type::UInt64 || (customInteger(t) && t.base == Type::UIntN);
}
inline int integerWidth(const Type &t) {
  return customInteger(t) ? int(t.bits)
       : t == Type::Int8 || t == Type::UInt8 ? 8
       : t == Type::Short || t == Type::UInt16 ? 16
       : t == Type::Int || t == Type::UInt32 ? 32
       : t == Type::Long || t == Type::UInt64 ? 64 : 0;
}
inline bool integral(const Type &t) { return integerWidth(t) != 0; }
inline bool numeric(const Type &t) { return integral(t) || floating(t); }
inline bool heapValue(const Type &t) { return numeric(t) || t == Type::Bool || recordValue(t); }
// Whether Print accepts a value of this type.
inline bool printable(const Type &t) {
  if (nativeMath(t)) return true;

  return t != Type::Void && !t.depth && !recordValue(t) && !arrayValue(t) && !collectionValue(t) && !funcValue(t);
}
inline int integerLiteralBase(const std::string &text) {
  if (text.size() >= 2 && text[0] == '0') {
    if (text[1] == 'x' || text[1] == 'X') return 16;
    if (text[1] == 'b' || text[1] == 'B') return 2;
  }

  return 10;
}
inline Type commonNumeric(Type a, Type b) {
  if (a == Type::Double || b == Type::Double)
    return Type::Double;

  if (a == Type::Float || b == Type::Float)
    return Type::Float;

  if (unsignedInteger(a) == unsignedInteger(b))
    return integerWidth(a) >= integerWidth(b) ? a : b;

  const auto signedType = unsignedInteger(a) ? b : a;
  const auto unsignedType = unsignedInteger(a) ? a : b;
  // Host widths first. Custom byte widths are considered only when an operand
  // already uses one, so uint64 mixed with a signed host type still needs a cast.
  std::vector<int> widths{8, 16, 32, 64};

  if (customInteger(a) || customInteger(b))
    for (int bytes = 9; bytes <= 64; ++bytes) widths.push_back(8 * bytes);

  for (auto width : widths)
    if (width >= integerWidth(signedType) && width > integerWidth(unsignedType))
      return Type::integer(unsigned(width), false);

  return Type::Void; // No lossless common integer type in this bootstrap.
}
// Common type of big/ubig or tiny/utiny with each other or with an integer of
// at most 64 bits. Both operands nonnegative keeps the unsigned type; big and
// tiny never mix, and floating values never mix with either.
inline Type arbitraryCommon(const Type &a, const Type &b) {
  const bool big = bigInteger(a) || bigInteger(b), decimal = decimalValue(a) || decimalValue(b);

  if (big == decimal) return Type::Void;

  auto compatible = [&](const Type &t) {
    return (big ? bigInteger(t) : decimalValue(t)) || (integral(t) && integerWidth(t) <= 64);
  };

  if (!compatible(a) || !compatible(b)) return Type::Void;

  auto nonnegative = [](const Type &t) {
    return t == Type::UBig || t == Type::UTiny || unsignedInteger(t);
  };
  const bool unsignedResult = nonnegative(a) && nonnegative(b);
  return big ? (unsignedResult ? Type::UBig : Type::Big)
             : (unsignedResult ? Type::UTiny : Type::Tiny);
}
struct Function;
struct Expr {
  Token token;
  ExprKind kind = ExprKind::None;
  Type castType = Type::Void;
  std::vector<std::unique_ptr<Expr>> args;
  std::unique_ptr<Expr> receiver;
  // A lambda's generated function. Shared because field defaults are cloned.
  std::shared_ptr<Function> lambda;
  // A "macroValue": read-only locals for the macro's arguments, in scope for
  // its result in args[0] and evaluated only when the expression is reached.
  std::vector<std::unique_ptr<struct Stmt>> bindings;
  bool addressable() const {
    return kind == ExprKind::Name || kind == ExprKind::Member || kind == ExprKind::Index || (kind == ExprKind::Unary && token.text == Tok::Star);
  }
};

// statment
struct Stmt {
  Token token;
  StmtKind kind = StmtKind::None;
  std::string name;
  std::string key;      // A foreach's key variable, as in `for k, v : pair`.
  bool reverse = false; // A foreach written `for -x : c`.
  Type type = Type::Void;
  bool constant = false;
  std::unique_ptr<Expr> expr, target;
  std::unique_ptr<Stmt> initializer, step;
  std::vector<std::unique_ptr<Stmt>> body, alternate;
  // A "switch" holds its selector as the hidden local `initializer`, reads it
  // back through `target`, runs macro argument bindings from `alternate`, and
  // has one "case" statement per section in `body`. A section's labels are
  // here; stacked labels and `default` share one section.
  std::vector<std::unique_ptr<Expr>> labels; // Also holds the callable operands of a P group.
  bool isDefault = false;
};
// A `using Name.Space` directive. It applies to the declarations after it in
// the file scope or static body it appears in, and in their nested bodies.
// `level` is that scope's qualified container ("" at file scope); `target`
// is the imported container's qualified name.
struct Using {
  Token token;
  std::string level, target;
};
// The directives visible at a declaration, outermost first. Shared because
// every declaration in a scope sees the same list.
using Usings = std::shared_ptr<const std::vector<Using>>;
// One `[Name]` attribute. `type` and `member` name the enum member it
// resolves to, filled in by resolveAttributes.
struct Tag {
  Token token;
  std::string type;
  size_t member = 0;
};
struct Parameter {
  Token name;
  Type type;
  bool reference = false;
  bool protectedAccess = false; // A container field declared with '-'.
  // A synthesized leading field holding a parent's state, named after the
  // parent. Protected, so `this.Parent` is reachable only from derived code.
  bool parentView = false;
  // A container field's default, a compile-time constant. Shared so field
  // lists stay copyable; initialValue is its LLVM constant once resolved,
  // or empty for a string/name literal emitted from the expression.
  std::shared_ptr<Expr> initializer;
  std::string initialValue;
  // The default is not a compile-time constant; a generated method
  // evaluates it during construction.
  bool runtimeDefault = false;
  Usings usings; // Visible where a field default was written.
  std::vector<Tag> tags;
  // A func field whose default is a lambda: its body runs against the object
  // it is called on, so it is called through a receiver and never read.
  bool memberLambda = false;
  // `[@Play]`: the scope holding this field, and its declared type. `type`
  // is then the slot stored in the container.
  std::string scope;
  Type scopedType = Type::Void;
};
// Instance methods live in their own namespace, so a chained static block
// can declare a function with the same name: Meter.Read is static and
// Meter.$Read is the instance method, chosen by the receiver's kind.
inline std::string methodKey(const std::string &record, const std::string &member) {
  return record + ".$" + member;
}
// The member name of a qualified function key, without the method marker.
inline std::string memberName(const std::string &key) {
  auto member = key.substr(key.rfind('.') + 1);
  return !member.empty() && member[0] == '$' ? member.substr(1) : member;
}
struct Function {
  Token name;
  // Mutable so an untyped property's accessors can take the type inferred
  // from its getter after parsing (see resolveProperties).
  mutable Type result;
  mutable std::vector<Parameter> params;
  std::vector<std::unique_ptr<Stmt>> body;
  bool method = false;
  std::string nativeSymbol; // Nonempty for a bodyless extern "C" declaration.
  bool protectedAccess = false; // A container member declared with '-'.
  // Generated by the compiler to evaluate a default or static initializer.
  // Its fields are not bound as locals, so defaults cannot read them.
  bool generated = false;
  std::string initializes; // The static field a generated $eval assigns.
  Usings usings;
  // A property accessor written without a type: a getter's result, or a
  // setter's value parameter, comes from the property's getter expression.
  mutable bool inferred = false;
  // The hidden function holding a file's executable statements, run by Exec.
  bool script = false;
  std::vector<Tag> tags;
  // A test from a T{ } block: its stable name, as in Container.Test. Empty
  // for every other function.
  std::string testName;
  // The braces of a body written in source, so an interface file can drop
  // the bodies importers never read. Empty text when there are none.
  Token bodyOpen{}, bodyClose{};
};
struct Record {
  Token name;
  std::vector<Parameter> fields;
  std::vector<Token> parents; // In priority order, leftmost first.
  bool imported = false;      // Defined by an imported module.
  // Direct children of this container's `this{ }` block, by type. When
  // closed, nothing else can add children, so lookups are checked statically.
  std::map<std::string, int> markupChildren;
  bool markupClosed = false;
};
struct EnumMember {
  Token name;
  std::unique_ptr<Expr> value; // Null for an implicit value.
};
// One enum block. Its qualified name must be unique across all input files.
struct EnumFragment {
  Token name;
  Type type = Type::Void; // Void when this fragment omits the underlying type.
  std::vector<EnumMember> members;
  bool attribute = false; // Marked [args], so members can be used as attributes.
};
// A field declared in a static block, as in `Math #{ int Count = 0 }`. The
// name is qualified by its enclosing containers, like a static function.
struct StaticField {
  Token name;
  Type type;
  bool constant = false;
  bool protectedAccess = false;
  std::unique_ptr<Expr> initializer; // Null for zero initialization.
  Usings usings;
  std::string scope; // As for a Parameter's scope: `type` is then its slot.
  Type scopedType = Type::Void;
};
struct Program : std::vector<Function> {
  std::vector<Record> records;
  std::vector<EnumFragment> enums;
  std::vector<StaticField> statics;
  std::vector<std::string> sources; // Source names in input order.
  // T{ } tests. They stay out of the program unless enableTests moves them in,
  // so an ordinary build compiles exactly what it would without them.
  std::vector<Function> tests;
  // Containers used as markup tags that hold children, which need child
  // storage, and those holding a literal value, which need a `value` field.
  std::vector<Token> markupHosts;
  struct MarkupValue {
    Token tag;
    Type type;
  };
  std::vector<MarkupValue> markupValues;
};
} // namespace duck

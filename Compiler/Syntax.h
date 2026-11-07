#pragma once
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace duck {
struct Token {
  std::string text;
  int line, column;
  bool string = false;
  std::string source;
  bool character = false; // A 'x' literal; also marked string so it is never punctuation.
};
[[noreturn]] inline void fail(const Token &t, const std::string &message) {
  throw std::runtime_error((t.source.empty() ? "<input>" : t.source) + ":" +
                           std::to_string(t.line) + ":" +
                           std::to_string(t.column) + ": error: " + message);
}
struct Type {
  enum Kind {
    Void, Int8, Short, Int, Long, UInt8, UInt16, UInt32, UInt64,
    Float, Double, Bool, Text, String, Name, Record,
    Char, Date, Time, DateTime,
    IntN, UIntN, // Custom byte-width iN/uN; `bits` holds 8*N.
    Big, UBig,   // Arbitrary-precision integers (reference-counted storage).
    Tiny, UTiny, // 69-digit decimal floating point values.
    Enum         // A #@ enum member; `record` holds the enum name.
  };
  enum Role { Raw, Owner, Alias };
  Role role = Raw;
  Kind base;
  unsigned depth = 0;
  unsigned bits = 0; // Only for IntN and UIntN.
  std::string record;
  Type(Kind kind = Void) : base(kind) {}
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
    return role == t.role && base == t.base && depth == t.depth && bits == t.bits &&
           record == t.record;
  }
  bool operator!=(const Type &t) const { return !(*this == t); }
  static const Type Pointer;
};
inline const Type Type::Pointer = Type(Type::Void).pointerTo();
inline bool recordValue(const Type &t) { return !t.depth && t.base == Type::Record; }
// An enum value is stored as its member's declaration index. Printing uses the
// member name; operators, conversions, and concatenation use its underlying value.
inline bool enumValue(const Type &t) { return !t.depth && t.base == Type::Enum; }
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
struct Expr {
  Token token;
  std::string kind;
  Type castType = Type::Void;
  std::vector<std::unique_ptr<Expr>> args;
  std::unique_ptr<Expr> receiver;
  bool addressable() const {
    return kind == "name" || kind == "." || (kind == "unary" && token.text == "*");
  }
};
struct Stmt {
  Token token;
  std::string kind, name;
  Type type = Type::Void;
  bool constant = false;
  std::unique_ptr<Expr> expr, target;
  std::unique_ptr<Stmt> initializer, step;
  std::vector<std::unique_ptr<Stmt>> body, alternate;
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
  Type result;
  std::vector<Parameter> params;
  std::vector<std::unique_ptr<Stmt>> body;
  bool method = false;
  std::string nativeSymbol; // Nonempty for a bodyless extern "C" declaration.
  bool protectedAccess = false; // A container member declared with '-'.
};
struct Record {
  Token name;
  std::vector<Parameter> fields;
  std::vector<Token> parents; // In priority order, leftmost first.
};
struct EnumMember {
  Token name;
  std::unique_ptr<Expr> value; // Null for an implicit value.
};
// One `Name [: type] #@{ ... }` block. Fragments with the same name merge in
// input order before implicit values are assigned.
struct EnumFragment {
  Token name;
  Type type = Type::Void; // Void when this fragment omits the underlying type.
  std::vector<EnumMember> members;
};
// A field declared in a static block, as in `Math #{ int Count = 0 }`. The
// name is qualified by its enclosing containers, like a static function.
struct StaticField {
  Token name;
  Type type;
  bool constant = false;
  bool protectedAccess = false;
  std::unique_ptr<Expr> initializer; // Null for zero initialization.
};
struct Program : std::vector<Function> {
  std::vector<Record> records;
  std::vector<EnumFragment> enums;
  std::vector<StaticField> statics;
};
} // namespace duck

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
};
[[noreturn]] inline void fail(const Token &t, const std::string &message) {
  throw std::runtime_error((t.source.empty() ? "<input>" : t.source) + ":" +
                           std::to_string(t.line) + ":" +
                           std::to_string(t.column) + ": error: " + message);
}
struct Type {
  enum Kind {
    Void, Int8, Short, Int, Long, UInt8, UInt16, UInt32, UInt64,
    Float, Double, Bool, Text, String, Name, Record
  };
  enum Role { Raw, Owner, Alias };
  Role role = Raw;
  Kind base;
  unsigned depth = 0;
  std::string record;
  Type(Kind kind = Void) : base(kind) {}
  Type pointerTo() const { auto t = *this; ++t.depth; return t; }
  Type pointee() const { auto t = *this; --t.depth; t.role = Raw; return t; }
  bool operator==(Kind kind) const { return !depth && base == kind; }
  bool operator!=(Kind kind) const { return !(*this == kind); }
  bool operator==(const Type &t) const {
    return role == t.role && base == t.base && depth == t.depth && record == t.record;
  }
  bool operator!=(const Type &t) const { return !(*this == t); }
  static const Type Pointer;
};
inline const Type Type::Pointer = Type(Type::Void).pointerTo();
inline bool recordValue(const Type &t) { return !t.depth && t.base == Type::Record; }
inline bool nativeScalar(const Type &t) {
  return t.depth || t == Type::Int || t == Type::UInt32 || t == Type::Long ||
         t == Type::UInt64 || t == Type::Float || t == Type::Double;
}
inline bool managedText(const Type &t) { return t == Type::String || t == Type::Name; }
inline bool textType(const Type &t) { return managedText(t) || t == Type::Text; }
inline bool floating(const Type &t) { return t == Type::Float || t == Type::Double; }
inline bool unsignedInteger(const Type &t) {
  return t == Type::UInt8 || t == Type::UInt16 ||
         t == Type::UInt32 || t == Type::UInt64;
}
inline int integerWidth(const Type &t) {
  return t == Type::Int8 || t == Type::UInt8 ? 8
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
  for (auto candidate : {Type::Int8, Type::Short, Type::Int, Type::Long})
    if (integerWidth(candidate) >= integerWidth(signedType) &&
        integerWidth(candidate) > integerWidth(unsignedType))
      return candidate;
  return Type::Void; // No lossless common integer type in this bootstrap.
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
};
struct Function {
  Token name;
  Type result;
  std::vector<Parameter> params;
  std::vector<std::unique_ptr<Stmt>> body;
  bool method = false;
  std::string nativeSymbol; // Nonempty for a bodyless extern "C" declaration.
};
struct Record {
  Token name;
  std::vector<Parameter> fields;
};
struct Program : std::vector<Function> {
  std::vector<Record> records;
};
} // namespace duck

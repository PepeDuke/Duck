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
enum class Type {
  Void, Int8, Short, Int, Long, UInt8, UInt16, UInt32, UInt64,
  Float, Double, Bool, Text
};
inline bool floating(Type t) { return t == Type::Float || t == Type::Double; }
inline bool unsignedInteger(Type t) {
  return t == Type::UInt8 || t == Type::UInt16 ||
         t == Type::UInt32 || t == Type::UInt64;
}
inline int integerWidth(Type t) {
  return t == Type::Int8 || t == Type::UInt8 ? 8
       : t == Type::Short || t == Type::UInt16 ? 16
       : t == Type::Int || t == Type::UInt32 ? 32
       : t == Type::Long || t == Type::UInt64 ? 64 : 0;
}
inline bool integral(Type t) { return integerWidth(t) != 0; }
inline bool numeric(Type t) { return integral(t) || floating(t); }
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
};
struct Stmt {
  Token token;
  std::string kind, name;
  Type type = Type::Void;
  bool constant = false;
  std::unique_ptr<Expr> expr;
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
};
using Program = std::vector<Function>;
} // namespace duck

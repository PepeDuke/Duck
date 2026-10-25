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
enum class Type { Void, Int, Long, Float, Double, Bool, Text };
inline bool floating(Type t) { return t == Type::Float || t == Type::Double; }
inline bool integral(Type t) { return t == Type::Int || t == Type::Long; }
inline bool numeric(Type t) { return integral(t) || floating(t); }
inline Type commonNumeric(Type a, Type b) {
  return a == Type::Double || b == Type::Double ? Type::Double
         : a == Type::Float || b == Type::Float ? Type::Float
         : a == Type::Long || b == Type::Long   ? Type::Long
                                                : Type::Int;
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

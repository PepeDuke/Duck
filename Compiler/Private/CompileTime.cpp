#include "Pipeline.h"
#include "WideInteger.h"
#include <cmath>
#include <cstring>
#include <iomanip>
#include <optional>
#include <sstream>

namespace duck {
namespace {
bool scalar(const Type &t) { return numeric(t) || t == Type::Bool; }
unsigned width(const Type &t) { return t == Type::Bool ? 1u : unsigned(integerWidth(t)); }
// Interpreter values only carry scalar type information. Keeping the full
// aggregate Type (names, element types, signatures) on every recursive frame
// would exhaust the small default Windows stack before the depth guard.
struct ScalarType {
  Type::Kind base = Type::Void;
  unsigned bits = 0;
  ScalarType(Type t = Type::Void) : base(t.base), bits(t.bits) {}
  operator Type() const { Type t(base); t.bits = bits; return t; }
  bool operator==(const Type &t) const { return Type(*this) == t; }
  bool operator!=(const Type &t) const { return !(*this == t); }
};
struct Value {
  ScalarType type;
  Bits bits;
  double real = 0;
  bool negative() const { return integral(type) && !unsignedInteger(type) && bits.bit(width(type) - 1); }
  bool truth() const { return floating(type) ? real != 0 : !bits.zero(); }
};
using Scope = std::map<std::string, std::optional<Value>>;
using Environment = std::vector<Scope>;
struct EvaluationFailure : std::runtime_error { using std::runtime_error::runtime_error; };
[[noreturn]] void reject(const std::string &message) { throw EvaluationFailure(message); }
std::optional<Value> &binding(Environment &env, const std::string &name) {
  for (auto i = env.rbegin(); i != env.rend(); ++i) {
    auto found = i->find(name);

    if (found != i->end()) return found->second;
  }

  reject("'" + name + "' is not a constant local argument");
}
Value integer(Type type, Bits bits) {
  bits.trim(width(type)); return {type, bits, 0};
}
Value real(Type type, double number) {
  // Round after every float operation, just as the LLVM float instructions do.
  if (type == Type::Float) number = static_cast<float>(number);

  return {type, {}, number};
}
std::string encode(double number) {
  uint64_t bits; std::memcpy(&bits, &number, sizeof(bits));
  std::ostringstream out;
  out << "0x" << std::uppercase << std::hex << std::setfill('0') << std::setw(16) << bits;
  return out.str();
}

class Interpreter {
  const std::vector<const Function *> &functions;
  std::vector<FunctionAnalysis> &analyses;
  std::map<std::string, size_t> indices;
  std::set<size_t> validated;
  size_t remaining = 100000;
  unsigned depth = 0;
  unsigned expressionDepth = 0;
  unsigned validationDepth = 0;
  unsigned validationExpressionDepth = 0;
  struct Frame {
    unsigned &depth;
    Frame(unsigned &depth) : depth(depth) { ++depth; }
    ~Frame() { --depth; }
  };
  void tick(size_t cost = 1) {
    if (cost > remaining) reject("evaluation exceeded the 100000-step limit");

    remaining -= cost;
  }
  Value convert(Value v, Type target) {
    if (!scalar(v.type) || !scalar(target)) reject("only fixed-width numbers and bool are supported");
    if (v.type == target) return v;
    if (target == Type::Bool) return integer(target, Bits(v.truth()));

    if (floating(target)) {
      if (floating(v.type)) return real(target, v.real);

      // Accumulate in the destination precision. For wide integers, retain
      // the top significand plus guard/sticky bits to avoid double rounding.
      const bool negative = v.negative();
      auto magnitude = negative ? v.bits.negate() : v.bits;
      magnitude.trim(width(v.type));
      unsigned top = 512;

      while (top && !magnitude.bit(top - 1)) --top;

      const unsigned precision = target == Type::Float ? 24 : 53;
      unsigned shift = top > precision ? top - precision : 0;
      uint64_t significant = 0;

      for (unsigned i = top; i > shift; --i) significant = (significant << 1) | uint64_t(magnitude.bit(i - 1));

      if (shift) {
        bool sticky = false;

        for (unsigned i = 0; i + 1 < shift; ++i) sticky |= magnitude.bit(i);
        if (magnitude.bit(shift - 1) && (sticky || (significant & 1))) ++significant;
      }

      double result = std::ldexp(double(significant), int(shift));
      return real(target, negative ? -result : result);
    }

    if (floating(v.type)) {
      const double rounded = std::floor(v.real);
      const unsigned exponent = width(target) - (unsignedInteger(target) ? 0 : 1);
      const double high = std::ldexp(1.0, int(exponent));
      const double low = unsignedInteger(target) ? 0 : -high;

      if (!std::isfinite(rounded) || rounded < low || rounded >= high)
        reject("floating-to-integer conversion is out of range");

      double magnitude = std::fabs(rounded);
      Bits bits;

      for (unsigned i = width(target); i > 0; --i) {
        const double place = std::ldexp(1.0, int(i - 1));

        if (magnitude >= place) { bits.set(i - 1); magnitude -= place; }
      }

      return integer(target, rounded < 0 ? bits.negate() : bits);
    }

    auto bits = v.bits;

    if (v.negative()) for (unsigned i = width(v.type); i < 512; ++i) bits.set(i);

    return integer(target, bits);
  }
  // Mirrors floatEquality in Codegen.
  static bool floatsMatch(const Value &a, const Value &b, const Value *tolerance) {
    if (a.real == b.real) return true;

    const double gap = std::fabs(real(a.type, a.real - b.real).real);

    if (!std::isfinite(gap)) return false;
    if (tolerance) return gap <= tolerance->real;

    const double epsilon = a.type == Type::Float ? double(1e-5f) : 1e-9;
    const double larger = real(a.type, std::fmax(std::fabs(a.real), std::fabs(b.real))).real;
    return gap <= real(a.type, epsilon * std::fmax(larger, 1.0)).real;
  }
  Value binary(const std::string &op, Value a, Value b) {
    tick();
    const bool comparison = op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">=";

    if (comparison) {
      bool result = false;

      if (floating(a.type)) {
        if (op == "==") result = floatsMatch(a, b, nullptr);
        if (op == "!=") result = !floatsMatch(a, b, nullptr);
        if (op == "<") result = a.real < b.real;
        if (op == ">") result = a.real > b.real;
        if (op == "<=") result = a.real <= b.real;
        if (op == ">=") result = a.real >= b.real;
      } else {
        const int order = a.negative() != b.negative() ? (a.negative() ? -1 : 1) : a.bits.compare(b.bits);

        if (op == "==") result = order == 0;
        if (op == "!=") result = order != 0;
        if (op == "<") result = order < 0;
        if (op == ">") result = order > 0;
        if (op == "<=") result = order <= 0;
        if (op == ">=") result = order >= 0;
      }

      return integer(Type::Bool, Bits(result));
    }

    if (floating(a.type)) {
      if (op == "+") return real(a.type, a.real + b.real);
      if (op == "-") return real(a.type, a.real - b.real);
      if (op == "*") return real(a.type, a.real * b.real);
      if (op == "/") return real(a.type, a.real / b.real);

      reject("unsupported floating operator '" + op + "'");
    }

    const unsigned n = width(a.type);
    Bits bits;

    if (op == "+") bits = a.bits.add(b.bits);
    else if (op == "-") bits = a.bits.add(b.bits.negate());
    else if (op == "*") bits = a.bits.multiply(b.bits);
    else if (op == "<<" || op == ">>") {
      if (b.negative() || b.bits.compare(Bits(n)) >= 0) reject("shift count is outside the operand width");

      bits = a.bits.shift(b.bits.words[0], n, op == ">>", a.negative());
    } else if (op == "/" || op == "%") {
      if (b.bits.zero()) reject("integer division or remainder by zero");

      auto x = a.negative() ? a.bits.negate() : a.bits;
      auto y = b.negative() ? b.bits.negate() : b.bits;
      x.trim(n); y.trim(n);
      Bits minimum; minimum.set(n - 1);

      if (a.negative() && b.negative() && a.bits.compare(minimum) == 0 && y.compare(Bits(1)) == 0)
        reject("signed minimum divided by -1");

      tick(n);
      const auto qr = x.divide(y, n);
      bits = op == "/" ? qr.first : qr.second;

      if (op == "/" ? a.negative() != b.negative() : a.negative()) bits = bits.negate();
    } else if (op == "&" || op == "|" || op == "^") {
      for (size_t i = 0; i < bits.words.size(); ++i)
        bits.words[i] = op == "&" ? a.bits.words[i] & b.bits.words[i]
                      : op == "|" ? a.bits.words[i] | b.bits.words[i] : a.bits.words[i] ^ b.bits.words[i];
    } else reject("unsupported operator '" + op + "'");

    return integer(a.type, bits);
  }
  size_t target(const Expr &e, size_t owner) {
    const auto &a = analyses[owner];

    if (e.token.text == "Exec" || e.token.text == "Print")
      reject("I/O and Exec are forbidden");

    auto call = a.callTargets.find(&e);

    if (call == a.callTargets.end() || a.indirectCalls.count(&e) || a.receiverPaths.count(&e))
      reject("only direct Duck function calls are allowed; I/O, native calls and runtime dispatch are forbidden");

    auto found = indices.find(call->second);

    if (found == indices.end()) reject("function body is unavailable for '" + call->second + "'");

    return found->second;
  }
  void validateExpression(const Expr &e, size_t owner) {
    tick();

    if (validationExpressionDepth >= 64)
      reject("evaluation exceeded the 64-expression depth limit while checking effects");

    Frame frame(validationExpressionDepth);
    const auto &a = analyses[owner];

    if (a.staticFields.count(&e)) reject("static storage reads and writes are forbidden");

    if (!scalar(a.expressionTypes.at(&e)) &&
        !((e.kind == "call" || e.kind == "compileTime") && a.expressionTypes.at(&e) == Type::Void))
      reject("only fixed-width numbers and bool are supported");

    if (a.integerConstants.count(&e) || a.floatingConstants.count(&e)) return;

    if (e.kind == "call") {
      if (!a.mathCalls.count(&e)) validate(target(e, owner));
    } else if (e.kind != "int" && e.kind != "real" && e.kind != "bool" && e.kind != "name" &&
             e.kind != "typeMember" && e.kind != "cast" && e.kind != "unary" && e.kind != "binary" &&
             e.kind != "conditional" && e.kind != "compileTime" && e.kind != "macroValue")
      reject("expression '" + e.kind + "' is not allowed");

    if (e.kind == "unary" && (e.token.text == "&" || e.token.text == "*")) reject("pointer access is forbidden");
    for (const auto &bound : e.bindings) validateStatement(*bound, owner);
    // A qualified static function's syntactic receiver is a namespace, not a value.
    for (const auto &child : e.args) validateExpression(*child, owner);
  }
  void validateStatement(const Stmt &s, size_t owner) {
    tick();
    const std::set<std::string> allowed{"let", "expr", "assign", "++", "--", "+=", "-=", "*=", "/=", "%=",
      "&=", "|=", "^=", "<<=", ">>=", "block", "ordered", "if", "while", "for", "rangefor", "do", "break", "continue", "return",
      "switch", "case"};

    if (!allowed.count(s.kind)) reject("statement '" + s.kind + "' is not allowed");
    if (s.kind == "let" && !scalar(analyses[owner].localTypes.at(&s))) reject("only scalar locals are allowed");

    if (s.target) {
      if (s.target->kind != "name" || analyses[owner].staticFields.count(s.target.get()))
        reject("only local scalar mutation is allowed");
    }

    if (s.kind == "rangefor") {
      for (const auto &endpoint : s.expr->args) validateExpression(*endpoint, owner);
    } else if (s.expr) validateExpression(*s.expr, owner);

    if (s.initializer) validateStatement(*s.initializer, owner);
    if (s.step) validateStatement(*s.step, owner);
    for (const auto &label : s.labels) validateExpression(*label, owner);
    for (const auto &child : s.body) validateStatement(*child, owner);
    for (const auto &child : s.alternate) validateStatement(*child, owner);
  }
  void validate(size_t index) {
    if (!validated.insert(index).second) return;

    tick();

    if (validationDepth >= 32) reject("evaluation exceeded the 32-call depth limit while checking effects");

    Frame frame(validationDepth);
    const auto &f = *functions[index];

    if (!f.nativeSymbol.empty() || f.method || (!scalar(f.result) && f.result != Type::Void))
      reject("only direct Duck functions with scalar or void results are allowed");

    for (const auto &p : f.params)
      if (p.reference || !scalar(p.type)) reject("reference and non-scalar parameters are forbidden");

    for (const auto &s : f.body) validateStatement(*s, index);
  }
  // Mirrors the lowering in Codegen: floats compute at double and round once.
  Value math(const Expr &e, size_t owner, Environment &env) {
    using Function = double (*)(double, double);
    static const std::map<std::string, Function> reals{
        {"Sqrt", [](double a, double) { return std::sqrt(a); }},
        {"Cbrt", [](double a, double) { return std::cbrt(a); }},
        {"Exp", [](double a, double) { return std::exp(a); }},
        {"Exp2", [](double a, double) { return std::exp2(a); }},
        {"Log", [](double a, double) { return std::log(a); }},
        {"Log2", [](double a, double) { return std::log2(a); }},
        {"Log10", [](double a, double) { return std::log10(a); }},
        {"Sin", [](double a, double) { return std::sin(a); }},
        {"Cos", [](double a, double) { return std::cos(a); }},
        {"Tan", [](double a, double) { return std::tan(a); }},
        {"Asin", [](double a, double) { return std::asin(a); }},
        {"Acos", [](double a, double) { return std::acos(a); }},
        {"Atan", [](double a, double) { return std::atan(a); }},
        {"Sinh", [](double a, double) { return std::sinh(a); }},
        {"Cosh", [](double a, double) { return std::cosh(a); }},
        {"Tanh", [](double a, double) { return std::tanh(a); }},
        {"Floor", [](double a, double) { return std::floor(a); }},
        {"Ceil", [](double a, double) { return std::ceil(a); }},
        {"Round", [](double a, double) { return std::round(a); }},
        {"Trunc", [](double a, double) { return std::trunc(a); }},
        {"Pow", [](double a, double b) { return std::pow(a, b); }},
        {"Atan2", [](double a, double b) { return std::atan2(a, b); }},
        {"Hypot", [](double a, double b) { return std::hypot(a, b); }},
        {"Mod", [](double a, double b) { return std::fmod(a, b); }},
        {"Min", [](double a, double b) { return std::fmin(a, b); }},
        {"Max", [](double a, double b) { return std::fmax(a, b); }},
    };
    const auto name = memberName(e.token.text);
    std::vector<Value> v;

    for (const auto &argument : e.args) v.push_back(expression(*argument, owner, env));

    tick();
    const auto &x = v[0];
    const Type type = x.type;
    auto less = [&](const Value &a, const Value &b) { return binary("<", a, b).truth(); };

    if (name == "Clamp") return less(x, v[1]) ? v[1] : less(v[2], x) ? v[2] : x;
    if (name == "Lerp") return binary("+", x, binary("*", binary("-", v[1], x), v[2]));

    if (!floating(type)) {
      if (name == "Abs") return x.negative() ? integer(type, x.bits.negate()) : x;
      if (name == "Sign") return integer(type, x.negative() ? Bits(1).negate() : Bits(x.bits.zero() ? 0 : 1));
      if (name == "Min") return less(v[1], x) ? v[1] : x;
      if (name == "Max") return less(x, v[1]) ? v[1] : x;
    }

    if (name == "Abs") return real(type, std::fabs(x.real));
    if (name == "Sign") return real(type, x.real > 0 ? 1.0 : x.real < 0 ? -1.0 : x.real);

    return real(type, reals.at(name)(x.real, v.size() > 1 ? v[1].real : 0.0));
  }
  Value expression(const Expr &e, size_t owner, Environment &env) {
    tick();

    if (expressionDepth >= 64) reject("evaluation exceeded the 64-expression depth limit");

    Frame frameDepth(expressionDepth);
    const auto &a = analyses[owner];
    const auto type = a.expressionTypes.at(&e);
    Value result;

    if (auto i = a.integerConstants.find(&e); i != a.integerConstants.end()) result = integer(type, Bits::parse(i->second));
    else if (auto floatingConstant = a.floatingConstants.find(&e); floatingConstant != a.floatingConstants.end()) {
      auto bits = std::stoull(floatingConstant->second.substr(2), nullptr, 16);
      double number; std::memcpy(&number, &bits, sizeof(number)); result = real(type, number);
    } else if (e.kind == "bool") result = integer(Type::Bool, Bits(e.token.text == "true"));
    else if (e.kind == "name") {
      if (a.staticFields.count(&e)) reject("static storage reads and writes are forbidden");

      auto value = binding(env, e.token.text);

      if (!value) reject("'" + e.token.text + "' is not a constant argument");

      result = *value;
    } else if (e.kind == "cast") result = convert(expression(*e.args[0], owner, env), e.castType);
    else if (e.kind == "compileTime") result = expression(*e.args[0], owner, env);
    else if (e.kind == "macroValue") {
      env.emplace_back();

      for (const auto &bound : e.bindings) statement(*bound, owner, env);

      result = expression(*e.args[0], owner, env);
      env.pop_back();
    } else if (a.mathCalls.count(&e)) result = math(e, owner, env);
    else if (e.kind == "call") {
      const size_t index = target(e, owner);
      validate(index);

      if (depth >= 32) reject("evaluation exceeded the 32-call depth limit");

      Environment frame(1);
      const auto &f = *functions[index];

      if (e.args.size() != f.params.size()) reject("only ordinary direct function calls are allowed");

      for (size_t arg = 0; arg < e.args.size(); ++arg)
        frame.back()[f.params[arg].name.text] = expression(*e.args[arg], owner, env);

      ++depth;
      const auto flow = sequence(f.body, index, frame);
      --depth;
      result = flow.value;
    } else if (e.kind == "conditional") {
      const bool yes = expression(*e.args[0], owner, env).truth();
      result = expression(*e.args[yes ? 1 : 2], owner, env);
    } else if (e.kind == "unary") {
      result = expression(*e.args[0], owner, env);

      if (e.token.text == "!") result = integer(Type::Bool, Bits(!result.truth()));
      else if (e.token.text == "-" && floating(result.type)) result = real(result.type, -result.real);
      else if (e.token.text == "-") result = integer(result.type, result.bits.negate());
      else if (e.token.text == "~") result = integer(result.type, result.bits.negate().add(Bits(1).negate()));
      else reject("unsupported unary operator");
    } else if (e.kind == "binary") {
      auto left = expression(*e.args[0], owner, env);

      if (e.token.text == "&&" || e.token.text == "||") {
        const bool skip = e.token.text == "&&" ? !left.truth() : left.truth();
        result = skip ? left : expression(*e.args[1], owner, env);
      } else {
        auto right = expression(*e.args[1], owner, env);

        if (e.args.size() == 3) {
          const auto tolerance = expression(*e.args[2], owner, env);
          const bool same = floatsMatch(left, right, &tolerance);
          result = integer(Type::Bool, Bits(e.token.text == "==" ? same : !same));
        } else
          result = binary(e.token.text, left, right);
      }
    } else reject("expression '" + e.kind + "' is not a constant argument");

    auto conversion = a.conversions.find(&e);
    return conversion == a.conversions.end() ? result : convert(result, conversion->second);
  }
  struct Flow { enum Kind { Next, Return, Break, Continue } kind = Next; Value value; };
  Flow sequence(const std::vector<std::unique_ptr<Stmt>> &body, size_t owner, Environment &env) {
    env.emplace_back();
    Flow result;

    for (const auto &s : body) { result = statement(*s, owner, env); if (result.kind != Flow::Next) break; }

    env.pop_back(); return result;
  }
  Flow statement(const Stmt &s, size_t owner, Environment &env) {
    tick();

    if (s.kind == "block" || s.kind == "ordered") return sequence(s.body, owner, env);
    if (s.kind == "return") return {Flow::Return, s.expr ? expression(*s.expr, owner, env) : Value{}};
    if (s.kind == "break") return {Flow::Break, {}};
    if (s.kind == "continue") return {Flow::Continue, {}};
    if (s.kind == "if") return sequence(expression(*s.expr, owner, env).truth() ? s.body : s.alternate, owner, env);

    if (s.kind == "switch") {
      env.emplace_back();
      statement(*s.initializer, owner, env);

      for (const auto &bound : s.alternate) statement(*bound, owner, env);

      const auto selected = expression(*s.target, owner, env);
      const Stmt *chosen = nullptr, *otherwise = nullptr;

      for (const auto &section : s.body) {
        if (section->isDefault) otherwise = section.get();

        for (const auto &label : section->labels)
          if (!chosen && binary("==", selected, expression(*label, owner, env)).truth()) chosen = section.get();
      }

      if (!chosen) chosen = otherwise;

      Flow result;

      if (chosen) result = sequence(chosen->body, owner, env);

      env.pop_back();
      return result.kind == Flow::Break ? Flow{} : result;
    }

    if (s.kind == "rangefor") {
      const auto first = expression(*s.expr->args[0], owner, env);
      const auto last = expression(*s.expr->args[1], owner, env);
      const bool ascending = binary("<=", first, last).truth();
      env.emplace_back(); env.back()[s.name] = first;
      Flow result;

      for (;;) {
        tick();
        result = sequence(s.body, owner, env);

        if (result.kind == Flow::Return || result.kind == Flow::Break) break;

        auto &counter = binding(env, s.name);

        if (binary(ascending ? ">=" : "<=", *counter, last).truth()) break;

        counter = binary(ascending ? "+" : "-", *counter, integer(first.type, Bits(1)));
      }

      env.pop_back(); return result.kind == Flow::Return ? result : Flow{};
    }

    if (s.kind == "while" || s.kind == "for" || s.kind == "do") {
      env.emplace_back();

      if (s.initializer) statement(*s.initializer, owner, env);

      Flow result;
      bool first = true;

      for (;;) {
        tick();

        if (s.kind != "do" || !first) if (s.expr && !expression(*s.expr, owner, env).truth()) break;

        first = false;
        result = sequence(s.body, owner, env);

        if (result.kind == Flow::Return || result.kind == Flow::Break) break;
        if (s.step) statement(*s.step, owner, env);
      }

      env.pop_back(); return result.kind == Flow::Return ? result : Flow{};
    }

    if (s.kind == "let") {
      env.back()[s.name] = s.expr ? std::optional<Value>(expression(*s.expr, owner, env)) : std::nullopt;
      return {};
    }

    if (s.kind == "expr") { expression(*s.expr, owner, env); return {}; }

    auto &slot = binding(env, s.target->token.text);

    if (s.kind == "assign") slot = expression(*s.expr, owner, env);
    else {
      if (!slot) reject("read of uninitialized local");

      const auto old = *slot;
      const auto op = s.kind == "++" ? "+" : s.kind == "--" ? "-" : s.kind.substr(0, s.kind.size() - 1);
      auto right = s.expr ? expression(*s.expr, owner, env) : convert(integer(Type::Int, Bits(1)), old.type);
      slot = convert(binary(op, old, right), old.type);
    }

    return {};
  }
public:
  Interpreter(const std::vector<const Function *> &functions, std::vector<FunctionAnalysis> &analyses)
      : functions(functions), analyses(analyses) {
    for (size_t i = 0; i < functions.size(); ++i) indices[functions[i]->name.text] = i;
  }
  Value evaluate(const Expr &e, size_t owner, Environment &env) {
    remaining = 100000; depth = 0; expressionDepth = 0;
    validationDepth = 0; validationExpressionDepth = 0; validated.clear();
    return expression(e, owner, env);
  }
};

// Walk source scopes without running the enclosing function. Only #typed
// constants enter this environment; parameters and mutable locals stay unknown.
class MarkerPass {
  std::vector<FunctionAnalysis> &analyses;
  Interpreter interpreter;
  size_t owner = 0;
  Environment env;
  void expression(const Expr &e) {
    if (e.kind == "compileTime") {
      try {
        const auto v = interpreter.evaluate(*e.args[0], owner, env);
        auto &a = analyses[owner];

        if (floating(v.type)) a.floatingConstants[&e] = encode(v.real);
        else if (v.type != Type::Void) a.integerConstants[&e] = v.bits.decimal();
      } catch (const EvaluationFailure &error) {
        fail(e.token, "compile-time call failed: " + std::string(error.what()));
      }

      return;
    }

    if (e.kind == "macroValue") {
      env.emplace_back();

      for (const auto &bound : e.bindings) statement(*bound);

      expression(*e.args[0]);
      env.pop_back();
      return;
    }

    if (e.receiver) expression(*e.receiver);
    for (const auto &child : e.args) expression(*child);

    if (e.lambda) {
      env.emplace_back();

      for (const auto &p : e.lambda->params) env.back()[p.name.text] = std::nullopt;

      list(e.lambda->body);
      env.pop_back();
    }
  }
  void statement(const Stmt &s) {
    const bool isSwitch = s.kind == "switch";
    const bool loop = s.kind == "for" || s.kind == "rangefor" || s.kind == "foreach" || isSwitch;

    if (loop) env.emplace_back();
    if (s.initializer) statement(*s.initializer);
    if (s.expr) expression(*s.expr);
    if (s.target) expression(*s.target);
    for (const auto &label : s.labels) expression(*label);
    // A Switch's macro bindings are visible to its sections.
    if (isSwitch) for (const auto &bound : s.alternate) statement(*bound);

    if (s.kind == "rangefor" || s.kind == "foreach") {
      env.back()[s.name] = std::nullopt;

      if (!s.key.empty()) env.back()[s.key] = std::nullopt;
    }

    if (s.kind == "let" || s.kind == "own") {
      std::optional<Value> value;

      if (s.constant && s.expr) {
        try { value = interpreter.evaluate(*s.expr, owner, env); }
        catch (const EvaluationFailure &) { /* A # binding may have a runtime initializer. */ }
      }

      env.back()[s.name] = value;
    }

    list(s.body);

    if (!isSwitch) list(s.alternate);
    if (s.step) statement(*s.step);
    if (loop) env.pop_back();
  }
  void list(const std::vector<std::unique_ptr<Stmt>> &body) {
    env.emplace_back();

    for (const auto &s : body) statement(*s);

    env.pop_back();
  }
public:
  MarkerPass(const std::vector<const Function *> &functions, std::vector<FunctionAnalysis> &analyses)
      : analyses(analyses), interpreter(functions, analyses) {
    for (owner = 0; owner < functions.size(); ++owner) {
      env = Environment(1);

      for (const auto &p : functions[owner]->params) env.back()[p.name.text] = std::nullopt;

      list(functions[owner]->body);
    }
  }
};
} // namespace
void evaluateCompileTimeCalls(const std::vector<const Function *> &functions,
                              std::vector<FunctionAnalysis> &analyses) {
  MarkerPass pass(functions, analyses);
}
} // namespace duck

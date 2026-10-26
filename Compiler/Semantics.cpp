#include "Pipeline.h"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>

namespace duck {
namespace {
std::string typeName(Type type) {
  switch (type) {
  case Type::Int8:
    return "int8";
  case Type::Short:
    return "short";
  case Type::Int:
    return "int";
  case Type::Long:
    return "long";
  case Type::UInt8:
    return "uint8";
  case Type::UInt16:
    return "uint16";
  case Type::UInt32:
    return "uint32";
  case Type::UInt64:
    return "uint64";
  case Type::Float:
    return "float";
  case Type::Double:
    return "double";
  case Type::Bool:
    return "bool";
  case Type::Text:
    return "literal text";
  default:
    return "void";
  }
}
struct Local {
  Type type;
  bool constant;
  bool initialized = true;
};
class Analyzer {
  const Symbols &symbols;
  using Scopes = std::vector<std::map<std::string, Local>>;
  Scopes scopes;
  struct LoopFlow {
    std::vector<Scopes> continues;
    std::vector<Scopes> breaks;
  };
  std::vector<LoopFlow> loops;
  FunctionAnalysis analysis;
  Type result;
  std::string container;

  void require(Type actual, Type expected, const Token &token) {
    if (actual != expected)
      fail(token, "type mismatch: expected " + typeName(expected) + ", got " +
                      typeName(actual));
  }
  Local &lookup(const Token &token) {
    for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
      auto local = scope->find(token.text);
      if (local != scope->end())
        return local->second;
    }
    fail(token, "unknown variable '" + token.text + "'");
  }
  void requireInitialized(const Token &token) {
    if (!lookup(token).initialized)
      fail(token,
           "variable '" + token.text + "' may be read before assignment");
  }
  // Merge only bindings visible at the join, ignoring inner scopes captured
  // on a continue edge. A variable must be initialized on every incoming edge.
  Scopes merge(Scopes visible, const std::vector<Scopes> &incoming) {
    for (size_t i = 0; i < visible.size(); ++i)
      for (auto &entry : visible[i]) {
        bool initialized = !incoming.empty();
        for (const auto &state : incoming)
          initialized = initialized && state.at(i).at(entry.first).initialized;
        entry.second.initialized = initialized;
      }
    return visible;
  }
  void bind(const Token &token, Local local) {
    if (!scopes.back().emplace(token.text, local).second)
      fail(token, "duplicate variable '" + token.text + "'");
  }
  void convert(const Expr &e, Type from, Type to) {
    if (from == to)
      return;
    if (numeric(from) && numeric(to) && commonNumeric(from, to) == to) {
      analysis.conversions[&e] = to;
      return;
    }
    require(from, to, e.token);
  }
  Type common(Type a, Type b, const Token &token) {
    const auto type = commonNumeric(a, b);
    if (type == Type::Void)
      fail(token, "mixed signed and unsigned values require an explicit cast: "
                  "no common integer type can represent both ranges");
    return type;
  }
  Type expression(const Expr &e, Type expected = Type::Void) {
    auto type = checkExpression(e, expected);
    analysis.expressionTypes.emplace(&e, type);
    if (expected != Type::Void) {
      convert(e, type, expected);
      return expected;
    }
    return type;
  }
  Type checkExpression(const Expr &e, Type expected) {
    const auto &op = e.token.text;
    if (e.kind == "cast") {
      auto source = expression(*e.args[0]);
      if ((!numeric(source) && source != Type::Bool) ||
          (!numeric(e.castType) && e.castType != Type::Bool))
        fail(e.token, "casts currently require numeric or bool types");
      return e.castType;
    }
    if (e.kind == "real") {
      Type type = expected == Type::Double ? Type::Double : Type::Float;
      std::istringstream input(op);
      input.imbue(std::locale::classic());
      double value = 0;
      if (type == Type::Float) {
        float single = 0;
        input >> single;
        value = single;
      } else
        input >> value;
      if (!input || input.peek() != std::char_traits<char>::eof() ||
          !std::isfinite(value))
        fail(e.token, "invalid or out-of-range floating literal");
      uint64_t bits = 0;
      static_assert(sizeof(value) == sizeof(bits));
      std::memcpy(&bits, &value, sizeof(bits));
      std::ostringstream encoded;
      encoded << "0x" << std::hex << std::uppercase << std::setfill('0')
              << std::setw(16) << bits;
      analysis.floatingConstants[&e] = encoded.str();
      return type;
    }
    // Parse magnitudes without passing through a signed host integer. A
    // negated literal is checked as a whole, including the signed minima.
    const bool negativeLiteral = e.kind == "unary" && op == "-" &&
                                 e.args[0]->kind == "int";
    if (e.kind == "int" || negativeLiteral) {
      const auto &digits = negativeLiteral ? e.args[0]->token.text : op;
      size_t consumed = 0;
      uint64_t value = 0;
      try {
        value = std::stoull(digits, &consumed);
      } catch (...) {
        fail(e.token, "integer literal out of range");
      }
      if (consumed != digits.size())
        fail(e.token, "invalid integer literal");
      Type type;
      if (negativeLiteral) {
        if (value > (uint64_t{1} << 63))
          fail(e.token, "integer literal out of range");
        type = value <= (uint64_t{1} << 31) ? Type::Int : Type::Long;
      } else
        type = value <= uint64_t(std::numeric_limits<int32_t>::max()) ? Type::Int
             : value <= uint64_t(std::numeric_limits<int64_t>::max()) ? Type::Long
                                                            : Type::UInt64;
      if (integral(expected) && expected != Type::Int) {
        const auto width = integerWidth(expected);
        const auto limit = unsignedInteger(expected)
            ? (width == 64 ? std::numeric_limits<uint64_t>::max()
                           : (uint64_t{1} << width) - 1)
            : (uint64_t{1} << (width - 1)) - (negativeLiteral ? 0 : 1);
        if (value > limit || (unsignedInteger(expected) && negativeLiteral && value != 0))
          fail(e.token, "integer literal out of range for " + typeName(expected));
        type = expected;
      }
      analysis.integerConstants[&e] =
          (negativeLiteral && value != 0 ? "-" : "") + std::to_string(value);
      if (negativeLiteral) {
        analysis.expressionTypes.emplace(e.args[0].get(), type);
        analysis.integerConstants[e.args[0].get()] = std::to_string(value);
      }
      return type;
    }
    if (e.kind == "bool")
      return Type::Bool;
    if (e.kind == "text")
      return Type::Text;
    if (e.kind == "name") {
      requireInitialized(e.token);
      return lookup(e.token).type;
    }
    if (e.kind == "call") {
      if (op == "Print") {
        if (e.args.size() != 1)
          fail(e.token, "Print expects one argument");
        if (expression(*e.args[0]) == Type::Void)
          fail(e.token, "cannot print void");
        return Type::Void;
      }
      auto found = symbols.end();
      // Resolve relative names from the innermost static container outward.
      auto scope = container;
      while (!scope.empty() && found == symbols.end()) {
        found = symbols.find(scope + "." + op);
        auto dot = scope.rfind('.');
        scope = dot == std::string::npos ? "" : scope.substr(0, dot);
      }
      if (found == symbols.end())
        found = symbols.find(op);
      if (found == symbols.end())
        fail(e.token, "unknown function '" + op + "'");
      analysis.callTargets[&e] = found->first;
      const auto &signature = found->second;
      if (signature.parameters.size() != e.args.size())
        fail(e.token, "wrong argument count for '" + op + "'");
      for (size_t i = 0; i < e.args.size(); ++i) {
        const auto &argument = *e.args[i];
        const auto &parameter = signature.parameters[i];
        if (parameter.reference) {
          if (argument.kind != "name")
            fail(argument.token,
                 "reference argument must be a mutable variable");
          if (lookup(argument.token).constant)
            fail(argument.token,
                 "cannot pass a constant to a mutable reference");
        }
        if (parameter.reference)
          require(expression(argument), parameter.type, argument.token);
        else
          expression(argument, parameter.type);
      }
      return signature.result;
    }
    auto first = expression(*e.args[0],
                            e.kind == "unary" && op == "-" && numeric(expected)
                                ? expected
                                : Type::Void);
    if (e.kind == "conditional") {
      require(first, Type::Bool, e.token);
      auto yes = expression(*e.args[1], expected);
      if (yes == Type::Void)
        fail(e.token, "conditional branches must produce values");
      auto no = expression(*e.args[2], expected);
      if (numeric(yes) && numeric(no)) {
        auto type = common(yes, no, e.token);
        convert(*e.args[1], yes, type);
        convert(*e.args[2], no, type);
        return type;
      }
      require(no, yes, e.token);
      return yes;
    }
    if (e.kind == "unary") {
      if (op == "!")
        require(first, Type::Bool, e.token);
      else if (!numeric(first))
        require(first, Type::Int, e.token);
      return first;
    }
    if (op == "&&" || op == "||") {
      require(first, Type::Bool, e.token);
      require(expression(*e.args[1]), Type::Bool, e.token);
      return Type::Bool;
    }
    auto second = expression(*e.args[1]);
    if (numeric(first) && numeric(second)) {
      auto type = common(first, second, e.token);
      convert(*e.args[0], first, type);
      convert(*e.args[1], second, type);
      first = type;
    } else
      require(second, first, e.token);
    if (op == "==" || op == "!=") {
      if (!numeric(first) && first != Type::Bool)
        fail(e.token, "equality supports numeric and bool values only");
      return Type::Bool;
    }
    if (!numeric(first) || (op == "%" && !integral(first)))
      require(first, Type::Int, e.token);
    return op == "<" || op == ">" || op == "<=" || op == ">=" ? Type::Bool
                                                              : first;
  }
  bool sequence(const std::vector<std::unique_ptr<Stmt>> &statements) {
    bool terminated = false;
    for (const auto &s : statements) {
      if (terminated)
        fail(s->token, "unreachable statement");
      terminated = statement(*s);
    }
    return terminated;
  }
  bool block(const std::vector<std::unique_ptr<Stmt>> &statements) {
    scopes.emplace_back();
    bool terminated = sequence(statements);
    scopes.pop_back();
    return terminated;
  }
  bool statement(const Stmt &s) {
    if (s.kind == "block")
      return block(s.body);
    if (s.kind == "break" || s.kind == "continue") {
      if (loops.empty())
        fail(s.token, s.kind + " requires an enclosing loop");
      if (s.kind == "continue")
        loops.back().continues.push_back(scopes);
      else
        loops.back().breaks.push_back(scopes);
      return true;
    }
    if (s.kind == "return") {
      require(s.expr ? expression(*s.expr, result) : Type::Void, result,
              s.token);
      return true;
    }
    if (s.kind == "if") {
      require(expression(*s.expr), Type::Bool, s.token);
      auto before = scopes;
      bool yes = block(s.body);
      auto yesState = scopes;
      scopes = before;
      bool no = block(s.alternate);
      auto noState = scopes;
      if (yes && no)
        scopes = before;
      else if (yes)
        scopes = noState;
      else if (no)
        scopes = yesState;
      else
        scopes = merge(before, {yesState, noState});
      return yes && no;
    }
    if (s.kind == "do") {
      const auto entry = scopes;
      loops.emplace_back();
      bool terminated = block(s.body);
      auto incoming = std::move(loops.back().continues);
      auto exits = std::move(loops.back().breaks);
      loops.pop_back();
      if (!terminated)
        incoming.push_back(scopes);
      scopes = incoming.empty() ? entry : merge(entry, incoming);
      require(expression(*s.expr), Type::Bool, s.token);
      if (!incoming.empty())
        exits.push_back(scopes);
      scopes = exits.empty() ? entry : merge(entry, exits);
      if (exits.empty()) {
        analysis.terminatingLoops.insert(&s);
        return true;
      }
      return false;
    }
    if (s.kind == "while" || s.kind == "for") {
      scopes.emplace_back();
      if (s.initializer)
        statement(*s.initializer);
      if (s.expr)
        require(expression(*s.expr), Type::Bool, s.token);
      auto entry = scopes;
      loops.emplace_back();
      bool terminated = block(s.body);
      auto incoming = std::move(loops.back().continues);
      loops.pop_back();
      if (!terminated)
        incoming.push_back(scopes);
      // Still type-check an unreachable step, using conservative entry facts.
      scopes = incoming.empty() ? entry : merge(entry, incoming);
      if (s.step)
        statement(*s.step);
      scopes = entry; // A precondition loop can execute zero times.
      scopes.pop_back();
      return false;
    }
    if (s.kind == "let") {
      auto type = s.expr ? expression(*s.expr, s.type) : s.type;
      if (type == Type::Void)
        fail(s.token, "locals currently support only values, not void");
      if (s.type != Type::Void)
        require(type, s.type, s.token);
      auto name = s.token;
      name.text = s.name;
      bind(name, {type, s.constant, s.expr != nullptr});
    } else if (s.kind == "expr")
      expression(*s.expr);
    else {
      auto name = s.token;
      name.text = s.name;
      auto local = lookup(name);
      if (local.constant)
        fail(s.token, "cannot modify constant '" + s.name + "'");
      if (s.kind != "assign")
        requireInitialized(name);
      if (s.kind != "assign" &&
          (!numeric(local.type) || (s.kind == "%=" && !integral(local.type))))
        require(local.type, Type::Int, s.token);
      if (s.expr)
        expression(*s.expr, local.type);
      lookup(name).initialized = true;
    }
    return false;
  }

public:
  Analyzer(const Symbols &symbols, Type result)
      : symbols(symbols), result(result) {}
  FunctionAnalysis run(const Function &function) {
    auto dot = function.name.text.rfind('.');
    if (dot != std::string::npos)
      container = function.name.text.substr(0, dot);
    scopes.emplace_back();
    for (const auto &parameter : function.params)
      bind(parameter.name, {parameter.type, false});
    if (!sequence(function.body) && result != Type::Void)
      fail(function.name, "not all paths return a value");
    return std::move(analysis);
  }
};
} // namespace
FunctionAnalysis analyzeFunction(const Function &function,
                                 const Symbols &symbols) {
  return Analyzer(symbols, function.result).run(function);
}
} // namespace duck

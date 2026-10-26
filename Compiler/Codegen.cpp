#include "Pipeline.h"
#include <cstdint>
#include <sstream>
namespace duck {
namespace {
std::string irType(Type t) {
  if (integral(t))
    return "i" + std::to_string(integerWidth(t));
  switch (t) {
  case Type::Float:
    return "float";
  case Type::Double:
    return "double";
  case Type::Bool:
    return "i1";
  case Type::Text:
    return "ptr";
  default:
    return "void";
  }
}
// Exact IEEE double powers of two delimit every supported integer range.
std::string powerOfTwo(int exponent, bool negative = false) {
  const auto bits = (uint64_t(exponent + 1023) << 52) |
                    (negative ? uint64_t{1} << 63 : 0);
  std::ostringstream encoded;
  encoded << "0x" << std::hex << bits;
  return encoded.str();
}
struct Value {
  Type type;
  std::string value;
};
struct Binding {
  Type type;
  std::string value;
  bool constant = false;
};
class Generator {
  const Symbols &functions;
  const FunctionAnalysis &analysis;
  std::string symbolPrefix;
  std::vector<std::map<std::string, Binding>> scopes;
  std::ostringstream globals, code, allocations;
  struct Loop {
    std::string exit, next;
  };
  std::vector<Loop> loops;
  std::string currentBlock = "entry";
  int next = 0, strings = 0;
  std::string fresh() { return "%v" + std::to_string(next++); }
  std::string label() { return "b" + std::to_string(next++); }
  void emit(const std::string &s) { code << "  " << s << '\n'; }
  void require(const std::string &valid) {
    auto success = label(), failure = label();
    emit("br i1 " + valid + ", label %" + success + ", label %" + failure);
    startBlock(failure);
    emit("call void @llvm.trap()");
    emit("unreachable");
    startBlock(success);
  }
  Value convert(Value value, Type target) {
    if (value.type == target)
      return value;
    if (integral(value.type) && integral(target) &&
        integerWidth(value.type) == integerWidth(target))
      return {target, value.value}; // Signedness changes interpretation only.
    auto id = fresh();
    if (floating(value.type) && integral(target)) {
      value = convert(value, Type::Double);
      auto rounded = fresh(), lower = fresh(), upper = fresh(), valid = fresh();
      emit(rounded + " = call double @llvm.floor.f64(double " + value.value +
           ")");
      const auto exponent = integerWidth(target) - (unsignedInteger(target) ? 0 : 1);
      const auto low = unsignedInteger(target) ? "0.000000e+00" : powerOfTwo(exponent, true);
      const auto high = powerOfTwo(exponent);
      emit(lower + " = fcmp oge double " + rounded + ", " + low);
      emit(upper + " = fcmp olt double " + rounded + ", " + high);
      emit(valid + " = and i1 " + lower + ", " + upper);
      require(valid);
      emit(id + (unsignedInteger(target) ? " = fptoui double " : " = fptosi double ") +
           rounded + " to " + irType(target));
      return {target, id};
    }
    if (target == Type::Bool) {
      emit(id + " = " + (floating(value.type) ? "fcmp une " : "icmp ne ") +
           irType(value.type) + " " + value.value + ", " +
           (floating(value.type) ? "0.000000e+00" : "0"));
      return {target, id};
    }
    emit(id + " = " +
         (value.type == Type::Bool ? (floating(target) ? "uitofp " : "zext ")
          : integral(value.type) && integral(target)
              ? (integerWidth(target) > integerWidth(value.type)
                     ? (unsignedInteger(value.type) ? "zext " : "sext ") : "trunc ")
          : integral(value.type)  ? (unsignedInteger(value.type) ? "uitofp " : "sitofp ")
          : target == Type::Float ? "fptrunc "
                                  : "fpext ") +
         irType(value.type) + " " + value.value + " to " + irType(target));
    return {target, id};
  }
  Value binary(const std::string &op, Value a, Value b) {
    if (integral(a.type) && (op == "/" || op == "%")) {
      // Both sdiv and srem require a nonzero divisor and must avoid MIN/-1.
      // Operands have already been evaluated once, in source order.
      auto nonzero = fresh();
      const auto type = irType(a.type);
      emit(nonzero + " = icmp ne " + type + " " + b.value + ", 0");
      auto valid = nonzero;
      if (!unsignedInteger(a.type)) {
        auto minimum = fresh(), negativeOne = fresh(), overflow = fresh(), safe = fresh();
        valid = fresh();
        emit(minimum + " = icmp eq " + type + " " + a.value + ", -" +
             std::to_string(uint64_t{1} << (integerWidth(a.type) - 1)));
        emit(negativeOne + " = icmp eq " + type + " " + b.value + ", -1");
        emit(overflow + " = and i1 " + minimum + ", " + negativeOne);
        emit(safe + " = xor i1 " + overflow + ", true");
        emit(valid + " = and i1 " + nonzero + ", " + safe);
      }
      require(valid);
    }
    auto id = fresh();
    const std::map<std::string, std::string> ints{
        {"+", "add"},      {"-", "sub"},       {"*", "mul"},
        {"/", "sdiv"},     {"%", "srem"},      {"<", "icmp slt"},
        {">", "icmp sgt"}, {"<=", "icmp sle"}, {">=", "icmp sge"},
        {"==", "icmp eq"}, {"!=", "icmp ne"}};
    const std::map<std::string, std::string> reals{
        {"+", "fadd"},      {"-", "fsub"},      {"*", "fmul"},
        {"/", "fdiv"},      {"<", "fcmp olt"},  {">", "fcmp ogt"},
        {"<=", "fcmp ole"}, {">=", "fcmp oge"}, {"==", "fcmp oeq"},
        {"!=", "fcmp une"}};
    auto instruction = (floating(a.type) ? reals : ints).at(op);
    if (unsignedInteger(a.type)) {
      if (op == "/") instruction = "udiv";
      else if (op == "%") instruction = "urem";
      else if (op == "<") instruction = "icmp ult";
      else if (op == ">") instruction = "icmp ugt";
      else if (op == "<=") instruction = "icmp ule";
      else if (op == ">=") instruction = "icmp uge";
    }
    emit(id + " = " + instruction + " " +
         irType(a.type) + " " + a.value + ", " + b.value);
    return {op == "<" || op == ">" || op == "<=" || op == ">=" || op == "==" ||
                    op == "!="
                ? Type::Bool
                : a.type,
            id};
  }
  void startBlock(const std::string &name) {
    code << name << ":\n";
    currentBlock = name;
  }
  Binding variable(const Token &t) {
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
      auto found = it->find(t.text);
      if (found != it->end())
        return found->second;
    }
    throw std::logic_error(
        "lowering requires a successfully analyzed function");
  }
  void bind(const Token &t, Binding binding) {

    scopes.back()[t.text] = binding;
  }
  void declare(const Token &t, Value value, bool constant = false) {
    auto address = fresh();
    allocations << "  " << address << " = alloca " << irType(value.type)
                << '\n';
    if (!value.value.empty())
      emit("store " + irType(value.type) + " " + value.value + ", ptr " +
           address);
    bind(t, {value.type, address, constant});
  }
  std::string textConstant(const std::string &value) {
    std::string name = "@str." + symbolPrefix + "." + std::to_string(strings++),
                escaped;
    const char *hex = "0123456789ABCDEF";
    for (unsigned char c : value) {
      escaped += '\\';
      escaped += hex[c >> 4];
      escaped += hex[c & 15];
    }
    globals << name << " = private unnamed_addr constant [" << value.size() + 1
            << " x i8] c\"" << escaped << "\\00\"\n";
    return name;
  }
  Value expression(const Expr &e) {
    // The analysis artifact is required, including in release builds.
    auto expected = analysis.expressionTypes.at(&e);
    auto value = lowerExpression(e);
    if (value.type != expected)
      throw std::logic_error("lowering disagrees with semantic analysis");
    auto conversion = analysis.conversions.find(&e);
    return conversion == analysis.conversions.end()
               ? value
               : convert(value, conversion->second);
  }
  Value lowerExpression(const Expr &e) {
    const auto &op = e.token.text;
    auto integer = analysis.integerConstants.find(&e);
    if (integer != analysis.integerConstants.end())
      return {analysis.expressionTypes.at(&e), integer->second};
    if (e.kind == "cast")
      return convert(expression(*e.args[0]), e.castType);
    if (e.kind == "real")
      return {analysis.expressionTypes.at(&e),
              analysis.floatingConstants.at(&e)};
    if (e.kind == "bool")
      return {Type::Bool, op == "true" ? "1" : "0"};
    if (e.kind == "text")
      return {Type::Text, textConstant(op)};
    if (e.kind == "name") {
      auto v = variable(e.token);
      auto id = fresh();
      emit(id + " = load " + irType(v.type) + ", ptr " + v.value);
      return {v.type, id};
    }
    if (e.kind == "call") {
      if (op == "Print") {

        auto v = expression(*e.args[0]);

        if (v.type == Type::Bool) {
          auto id = fresh();
          emit(id + " = zext i1 " + v.value + " to i32");
          v = {Type::Int, id};
        }
        if (v.type == Type::Float)
          v = convert(v, Type::Double);
        // All 8/16-bit values fit C's int varargs promotion.
        if (integral(v.type) && integerWidth(v.type) < 32)
          v = convert(v, Type::Int);
        auto fmt = textConstant(v.type == Type::Text     ? "%s\n"
                                : v.type == Type::Double ? "%g\n"
                                : v.type == Type::Long   ? "%lld\n"
                                : v.type == Type::UInt64 ? "%llu\n"
                                : v.type == Type::UInt32 ? "%u\n"
                                                         : "%d\n");
        emit("call i32 (ptr, ...) @printf(ptr " + fmt + ", " + irType(v.type) +
             " " + v.value + ")");
        return {Type::Void, ""};
      }
      const auto &target = analysis.callTargets.at(&e);
      auto found = functions.find(target);

      const auto &f = found->second;

      std::string args;
      for (size_t i = 0; i < e.args.size(); ++i) {
        const auto &parameter = f.parameters[i];
        const auto &argument = *e.args[i];
        if (i)
          args += ", ";
        if (parameter.reference) {

          auto binding = variable(argument.token);

          args += "ptr " + binding.value;
        } else {
          auto v = expression(argument);
          args += irType(v.type) + " " + v.value;
        }
      }
      auto id = f.result == Type::Void ? "" : fresh();
      emit((id.empty() ? "" : id + " = ") + "call " + irType(f.result) +
           " @duck." + target + "(" + args + ")");
      return {f.result, id};
    }
    auto a = expression(*e.args[0]);
    auto id = fresh();
    if (e.kind == "conditional") {

      auto yes = label(), no = label(), end = label();
      emit("br i1 " + a.value + ", label %" + yes + ", label %" + no);
      startBlock(yes);
      auto b = expression(*e.args[1]);
      auto yesEnd = currentBlock;

      emit("br label %" + end);
      startBlock(no);
      auto c = expression(*e.args[2]);
      auto noEnd = currentBlock;
      emit("br label %" + end);
      startBlock(end);
      emit(id + " = phi " + irType(b.type) + " [ " + b.value + ", %" + yesEnd +
           " ], [ " + c.value + ", %" + noEnd + " ]");
      return {b.type, id};
    }
    if (op == "&&" || op == "||") {

      auto leftEnd = currentBlock, right = label(), end = label();
      emit("br i1 " + a.value + ", label %" + (op == "&&" ? right : end) +
           ", label %" + (op == "&&" ? end : right));
      startBlock(right);
      auto b = expression(*e.args[1]);
      auto rightEnd = currentBlock;
      emit("br label %" + end);
      startBlock(end);
      emit(id + " = phi i1 [ " + (op == "&&" ? "0" : "1") + ", %" + leftEnd +
           " ], [ " + b.value + ", %" + rightEnd + " ]");
      return {Type::Bool, id};
    }
    if (e.kind == "unary") {
      if (floating(a.type)) {
        emit(id + " = fneg " + irType(a.type) + " " + a.value);
        return {a.type, id};
      }
      emit(id + (op == "!" ? " = xor i1 " + a.value + ", true"
                           : " = sub " + irType(a.type) + " 0, " + a.value));
      return {a.type, id};
    }
    auto b = expression(*e.args[1]);
    return binary(op, a, b);
  }
  bool block(const std::vector<std::unique_ptr<Stmt>> &body) {
    scopes.emplace_back();
    bool terminated = false;
    for (const auto &s : body) {

      terminated = statement(*s);
    }
    scopes.pop_back();
    return terminated;
  }
  bool statement(const Stmt &s) {
    if (s.kind == "block")
      return block(s.body);
    if (s.kind == "break" || s.kind == "continue") {

      emit("br label %" +
           (s.kind == "break" ? loops.back().exit : loops.back().next));
      return true;
    }
    if (s.kind == "return") {
      Value v = s.expr ? expression(*s.expr) : Value{Type::Void, ""};
      emit("ret " + irType(v.type) +
           (v.type == Type::Void ? "" : " " + v.value));
      return true;
    }
    if (s.kind == "if") {
      auto v = expression(*s.expr);
      auto yes = label(), no = label(), end = label();
      emit("br i1 " + v.value + ", label %" + yes + ", label %" + no);
      startBlock(yes);
      bool a = block(s.body);
      if (!a)
        emit("br label %" + end);
      startBlock(no);
      bool b = block(s.alternate);
      if (!b)
        emit("br label %" + end);
      if (a && b)
        return true;
      startBlock(end);
      return false;
    }
    if (s.kind == "do") {
      auto body = label(), check = label(), end = label();
      emit("br label %" + body);
      loops.push_back({end, check});
      startBlock(body);
      if (!block(s.body))
        emit("br label %" + check);
      loops.pop_back();
      startBlock(check);
      auto condition = expression(*s.expr);
      emit("br i1 " + condition.value + ", label %" + body + ", label %" + end);
      startBlock(end);
      if (analysis.terminatingLoops.count(&s)) {
        emit("unreachable");
        return true;
      }
      return false;
    }
    if (s.kind == "while" || s.kind == "for") {
      scopes.emplace_back();
      if (s.initializer)
        statement(*s.initializer);
      auto check = label(), body = label(), end = label(), step = label();
      emit("br label %" + check);
      startBlock(check);
      auto v = s.expr ? expression(*s.expr) : Value{Type::Bool, "1"};
      emit("br i1 " + v.value + ", label %" + body + ", label %" + end);
      loops.push_back({end, s.kind == "for" ? step : check});
      startBlock(body);
      if (!block(s.body))
        emit("br label %" + loops.back().next);
      loops.pop_back();
      if (s.kind == "for") {
        startBlock(step);
        if (s.step)
          statement(*s.step);
        emit("br label %" + check);
      }
      startBlock(end);
      scopes.pop_back();
      return false;
    }
    if (s.kind == "let") {
      auto v = s.expr ? expression(*s.expr) : Value{s.type, ""};

      auto name = s.token;
      name.text = s.name;
      declare(name, v, s.constant);
    } else if (s.kind == "assign" || s.kind == "++" || s.kind == "--" ||
               s.kind == "+=" || s.kind == "-=" || s.kind == "*=" ||
               s.kind == "/=" || s.kind == "%=") {
      auto name = s.token;
      name.text = s.name;
      auto address = variable(name);
      Value v;

      if (s.kind == "assign")
        v = expression(*s.expr);
      else {
        auto old = fresh();
        emit(old + " = load " + irType(address.type) + ", ptr " +
             address.value);
        auto operand = s.expr ? expression(*s.expr)
                              : convert({Type::Int, "1"}, address.type);
        v = binary(s.kind.substr(0, 1), {address.type, old}, operand);
      }
      emit("store " + irType(v.type) + " " + v.value + ", ptr " +
           address.value);
    } else
      expression(*s.expr);
    return false;
  }

public:
  Generator(const Symbols &symbols, const FunctionAnalysis &analysis,
            const std::string &name)
      : functions(symbols), analysis(analysis), symbolPrefix(name) {}
  FunctionIR generate(const Function &f) {
    scopes.emplace_back();
    std::ostringstream output;
    output << "define " << irType(f.result) << " @duck." << f.name.text << "(";
    for (size_t i = 0; i < f.params.size(); ++i) {
      if (i)
        output << ", ";
      const auto &parameter = f.params[i];
      auto arg = "%arg" + std::to_string(i);
      output << (parameter.reference ? "ptr" : irType(parameter.type)) << " "
             << arg;
      if (parameter.reference)
        bind(parameter.name, {parameter.type, arg});
      else
        declare(parameter.name, {parameter.type, arg});
    }
    bool terminated = false;
    for (const auto &s : f.body)
      terminated = statement(*s);
    if (!terminated)
      emit("ret void");
    output << ") {\nentry:\n" << allocations.str() << code.str() << "}\n\n";
    return {globals.str(), output.str()};
  }
};
} // namespace
FunctionIR lowerFunction(const Function &function, const Symbols &symbols,
                         const FunctionAnalysis &analysis) {
  return Generator(symbols, analysis, function.name.text).generate(function);
}
std::string emitDeclarations(const Symbols &symbols) {
  std::string declarations;
  for (const auto &item : symbols) {
    const auto &signature = item.second;
    declarations +=
        "declare " + irType(signature.result) + " @duck." + item.first + "(";
    for (size_t i = 0; i < signature.parameters.size(); ++i) {
      if (i)
        declarations += ", ";
      const auto &p = signature.parameters[i];
      declarations += p.reference ? "ptr" : irType(p.type);
    }
    declarations += ")\n";
  }
  return declarations;
}
} // namespace duck

#include "Pipeline.h"
#include "Unicode.h"
#include <cstdint>
#include <sstream>
namespace duck {
namespace {
std::string irType(Type t) {
  if (integral(t))
    return "i" + std::to_string(integerWidth(t));
  if (t.depth) return "ptr";
  if (recordValue(t)) return "%struct." + t.record;
  switch (t.base) {
  case Type::Float:
    return "float";
  case Type::Double:
    return "double";
  case Type::Bool:
    return "i1";
  case Type::String:
  case Type::Name:
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
  bool owned = false;
  bool scopedBorrow = false;
};
class Generator {
  const Symbols &functions;
  const FunctionAnalysis &analysis;
  std::string symbolPrefix;
  std::vector<std::map<std::string, Binding>> scopes;
  std::ostringstream globals, code, allocations;
  struct Loop {
    std::string exit, next;
    size_t scopeDepth;
  };
  std::vector<Loop> loops;
  std::string currentBlock = "entry";
  int next = 0, strings = 0;
  bool usesTextRuntime = false, usesHeapRuntime = false;
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
  static std::string textWidth(Type type) { return type == Type::String ? "2" : "1"; }
  void retain(Value value) {
    if (!managedText(value.type)) return;
    usesTextRuntime = true;
    emit("call void @duck.text.retain(ptr " + value.value + ")");
  }
  void release(Value value) {
    if (!managedText(value.type)) return;
    usesTextRuntime = true;
    emit("call void @duck.text.release(ptr " + value.value + ")");
  }
  static std::string ownerFlag(Type type) { return type.role == Type::Owner ? "true" : "false"; }
  std::string moveOwner(const Token &source) {
    auto slot = variable(source).value;
    auto word = fresh();
    emit(word + " = load ptr, ptr " + slot);
    emit("store ptr null, ptr " + slot);
    return word;
  }
  std::string aliasWord(const Expr &source) {
    if (source.kind == "null") return "null";
    auto binding = variable(source.token);
    auto word = fresh();
    emit(word + " = call ptr @duck.heap$alias(ptr " + binding.value + ", i1 " + ownerFlag(binding.type) + ")");
    return word;
  }
  void releaseBinding(const Binding &binding) {
    if (binding.scopedBorrow) return;
    if (binding.type.role != Type::Raw) {
      emit("call void @duck.heap$drop(ptr " + binding.value + ", i1 " + ownerFlag(binding.type) + ")");
      return;
    }
    if (!managedText(binding.type)) return;
    auto old = fresh();
    emit(old + " = load ptr, ptr " + binding.value);
    release({binding.type, old});
  }
  void cleanupScopes(size_t first) {
    for (size_t i = scopes.size(); i > first; --i)
      for (const auto &[name, binding] : scopes[i - 1]) {
        (void)name;
        if (binding.owned) releaseBinding(binding);
      }
  }
  Value convert(Value value, Type target) {
    if (value.type == target)
      return value;
    if (value.type.depth && target.depth) {
      if (value.type.role != Type::Raw && target.role == Type::Raw) {
        auto valid = fresh();
        emit(valid + " = icmp ne ptr " + value.value + ", null");
        require(valid);
      }
      return {target, value.value};
    }
    if (value.type.depth || target.depth) {
      auto id = fresh();
      emit(id + (value.type.depth ? " = ptrtoint " : " = inttoptr ") +
           irType(value.type) + " " + value.value + " to " + irType(target));
      return {target, id};
    }
    if (managedText(value.type) && managedText(target)) {
      usesTextRuntime = true;
      auto converted = fresh();
      emit(converted + " = call ptr @duck.text.convert(ptr " + value.value +
           ", i64 " + textWidth(value.type) + ", i64 " + textWidth(target) + ")");
      release(value);
      return {target, converted};
    }
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
    if (managedText(a.type)) {
      usesTextRuntime = true;
      auto id = fresh();
      bool concat = op == "+";
      emit(id + " = call " + (concat ? "ptr @duck.text.concat" : "i1 @duck.text.equal") +
           "(ptr " + a.value + ", ptr " + b.value + ", i64 " + textWidth(a.type) + ")");
      release(a);
      release(b);
      if (op == "!=") {
        auto inverse = fresh();
        emit(inverse + " = xor i1 " + id + ", true");
        id = inverse;
      }
      return {concat ? a.type : Type::Bool, id};
    }
    if (op == "<<" || op == ">>") {
      // Check at the original count width before any truncation. Comparing
      // as unsigned rejects negative signed counts as well.
      auto valid = fresh();
      emit(valid + " = icmp ult " + irType(b.type) + " " + b.value + ", " +
           std::to_string(integerWidth(a.type)));
      require(valid);
      b = convert(b, a.type);
    }
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
        {"==", "icmp eq"}, {"!=", "icmp ne"},
        {"&", "and"}, {"|", "or"}, {"^", "xor"},
        {"<<", "shl"}, {">>", "ashr"}};
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
      else if (op == ">>") instruction = "lshr";
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
  void declare(const Token &t, Value value, bool constant = false, bool scopedBorrow = false) {
    auto address = fresh();
    allocations << "  " << address << " = alloca " << irType(value.type)
                << '\n';
    if (recordValue(value.type) && value.value.empty()) value.value = "zeroinitializer";
    if (managedText(value.type) && value.value.empty()) value.value = "null";
    if (!value.value.empty())
      emit("store " + irType(value.type) + " " + value.value + ", ptr " +
           address);
    bind(t, {value.type, address, constant, true, scopedBorrow});
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
  Value managedLiteral(const Expr &e, Type type) {
    std::vector<uint32_t> units;
    if (type == Type::Name) {
      for (unsigned char byte : e.token.text) units.push_back(byte);
    } else {
      for (auto cp : decodeUTF8(e.token)) {
        if (cp < 0x10000) units.push_back(cp);
        else {
          cp -= 0x10000;
          units.push_back(0xD800 | (cp >> 10));
          units.push_back(0xDC00 | (cp & 1023));
        }
      }
    }
    auto symbol = "@str." + symbolPrefix + "." + std::to_string(strings++);
    auto unit = type == Type::String ? "i16" : "i8";
    auto array = "[" + std::to_string(units.size() + 1) + " x " + unit + "]";
    globals << symbol << " = private unnamed_addr constant { i64, i64, " << array
            << " } { i64 -1, i64 " << units.size() << ", " << array << " [";
    for (auto value : units) globals << unit << " " << value << ", ";
    globals << unit << " 0] }, align 8\n";
    return {type, symbol};
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
  Value dereference(const Expr &e) {
    auto v = expression(e);
    if (v.type.role != Type::Raw) {
      auto valid = fresh();
      emit(valid + " = icmp ne ptr " + v.value + ", null");
      require(valid);
    }
    return v;
  }
  Binding location(const Expr &e) {
    if (e.kind == "name") return variable(e.token);
    auto type = analysis.expressionTypes.at(&e);
    if (e.kind == "unary") return {type, dereference(*e.args[0]).value};
    const auto &base = *e.args[0];
    auto record = analysis.expressionTypes.at(&base);
    std::string address;
    if (!record.depth) address = location(base).value;
    else {
      address = dereference(base).value;
      record = record.pointee();
    }
    auto id = fresh();
    emit(id + " = getelementptr " + irType(record) + ", ptr " + address +
         ", i32 0, i32 " + std::to_string(analysis.fieldIndices.at(&e)));
    return {type, id};
  }
  Value lowerExpression(const Expr &e) {
    const auto &op = e.token.text;
    auto integer = analysis.integerConstants.find(&e);
    if (integer != analysis.integerConstants.end())
      return {analysis.expressionTypes.at(&e), integer->second};
    if (e.kind == "initializer") {
      auto type = analysis.expressionTypes.at(&e);
      std::string value = "zeroinitializer";
      for (const auto &field : e.args) {
        auto v = expression(*field->args.front());
        auto aggregate = fresh();
        emit(aggregate + " = insertvalue " + irType(type) + " " + value + ", " + irType(v.type) +
             " " + v.value + ", " + std::to_string(analysis.fieldIndices.at(field.get())));
        value = aggregate;
      }
      return {type, value};
    }
    if (e.kind == "cast")
      return convert(expression(*e.args[0]), e.castType);
    if (e.kind == "real")
      return {analysis.expressionTypes.at(&e),
              analysis.floatingConstants.at(&e)};
    if (e.kind == "null")
      return {analysis.expressionTypes.at(&e), "null"};
    if (e.kind == "bool")
      return {Type::Bool, op == "true" ? "1" : "0"};
    if (e.kind == "text") {
      auto type = analysis.expressionTypes.at(&e);
      if (managedText(type)) return managedLiteral(e, type);
      return {Type::Text, textConstant(op)};
    }
    if (e.kind == "unary" && op == "&") {
      auto target = analysis.callTargets.find(&e);
      if (target != analysis.callTargets.end()) {
        const auto &native = functions.at(target->second).nativeSymbol;
        return {Type::Pointer, native.empty() ? "@duck." + target->second : "@" + native};
      }
      return {analysis.expressionTypes.at(&e), location(*e.args[0]).value};
    }
    if (e.addressable()) {
      auto v = location(e);
      auto id = fresh();
      emit(id + " = load " + irType(v.type) + ", ptr " + v.value);
      if (v.type.role != Type::Raw && !v.scopedBorrow) {
        auto payload = fresh();
        emit(payload + " = call ptr @duck.heap$payload(ptr " + id + ", i1 " + ownerFlag(v.type) + ")");
        return {v.type, payload};
      }
      retain({v.type, id});
      return {v.type, id};
    }
    if (e.kind == "call") {
      if (op == "Length" || op == "CodeUnitAt") {
        usesTextRuntime = true;
        auto text = expression(*e.args[0]);
        auto id = fresh();
        if (op == "Length")
          emit(id + " = call i64 @duck.text.length(ptr " + text.value + ")");
        else {
          auto index = expression(*e.args[1]);
          emit(id + " = call i32 @duck.text.unit(ptr " + text.value + ", i64 " +
               index.value + ", i64 " + textWidth(text.type) + ")");
        }
        release(text);
        return {op == "Length" ? Type::Long : Type::UInt32, id};
      }
      if (op == "Print") {

        auto v = expression(*e.args[0]);

        if (managedText(v.type)) {
          usesTextRuntime = true;
          emit("call void @duck.text.print(ptr " + v.value + ", i64 " + textWidth(v.type) + ")");
          release(v);
          return {Type::Void, ""};
        }
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
      if (!analysis.callTargets.count(&e))
        return {analysis.expressionTypes.at(&e), "zeroinitializer"};
      const auto &target = analysis.callTargets.at(&e);
      auto found = functions.find(target);

      const auto &f = found->second;

      std::string args;
      if (f.method) {
        std::string address;
        if (!e.receiver) {
          address = "%arg0";
        } else if (analysis.expressionTypes.at(e.receiver.get()).depth)
          address = dereference(*e.receiver).value;
        else if (e.receiver->addressable())
          address = location(*e.receiver).value;
        else {
          auto temporary = expression(*e.receiver);
          address = fresh();
          allocations << "  " << address << " = alloca " << irType(temporary.type) << '\n';
          emit("store " + irType(temporary.type) + " " + temporary.value + ", ptr " + address);
        }
        args = "ptr " + address;
      }
      std::vector<Value> arguments;
      for (size_t i = 0; i < e.args.size(); ++i) {
        const auto &parameter = f.parameters[i + (f.method ? 1 : 0)];
        const auto &argument = *e.args[i];
        if (!args.empty())
          args += ", ";
        if (parameter.reference) {

          auto binding = location(argument);

          args += "ptr " + binding.value;
        } else {
          auto v = expression(argument);
          arguments.push_back(v);
          if (!f.nativeSymbol.empty() && managedText(v.type)) {
            auto data = fresh();
            emit(data + " = getelementptr i8, ptr " + v.value + ", i64 16");
            v.value = data; // Read-only native borrow; owner stays alive through the call.
          }
          args += irType(v.type) + " " + v.value;
        }
      }
      auto id = f.result == Type::Void ? "" : fresh();
      emit((id.empty() ? "" : id + " = ") + "call " + irType(f.result) +
           " @" + (f.nativeSymbol.empty() ? "duck." + target : f.nativeSymbol) + "(" + args + ")");
      for (auto argument : arguments) release(argument);
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
      emit(id + (op == "~" ? " = xor " + irType(a.type) + " " + a.value + ", -1"
                 : op == "!" ? " = xor i1 " + a.value + ", true"
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
    if (!terminated) cleanupScopes(scopes.size() - 1);
    scopes.pop_back();
    return terminated;
  }
  bool statement(const Stmt &s) {
    if (s.kind == "block")
      return block(s.body);
    if (s.kind == "break" || s.kind == "continue") {
      cleanupScopes(loops.back().scopeDepth);
      emit("br label %" +
           (s.kind == "break" ? loops.back().exit : loops.back().next));
      return true;
    }
    if (s.kind == "returnOwner") {
      auto word = moveOwner(s.expr->token);
      cleanupScopes(0);
      emit("ret ptr " + word);
      return true;
    }
    if (s.kind == "return") {
      Value v = s.expr ? expression(*s.expr) : Value{Type::Void, ""};
      cleanupScopes(0);
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
      loops.push_back({end, check, scopes.size()});
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
      loops.push_back({end, s.kind == "for" ? step : check, scopes.size()});
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
      cleanupScopes(scopes.size() - 1);
      scopes.pop_back();
      return false;
    }
    if (s.kind == "own" || s.kind == "let") {
      const auto type = analysis.localTypes.at(&s);
      if (type.role != Type::Raw) {
        usesHeapRuntime = true;
        std::string word;
        bool scoped = analysis.scopedBorrows.count(&s) != 0;
        if (scoped) word = expression(*s.expr).value;
        else if (type.role == Type::Alias) word = aliasWord(*s.expr);
        else if (s.expr && analysis.expressionTypes.at(s.expr.get()).role == Type::Owner) {
          word = s.expr->kind == "call" ? expression(*s.expr).value : moveOwner(s.expr->token);
        } else {
          auto payload = irType(type.pointee());
          auto value = s.expr ? expression(*s.expr).value : "zeroinitializer";
          word = fresh();
          emit(word + " = call ptr @duck.heap$alloc(i64 ptrtoint (ptr getelementptr (" +
               payload + ", ptr null, i32 1) to i64))");
          emit("store " + payload + " " + value + ", ptr " + word);
        }
        auto name = s.token;
        name.text = s.name;
        declare(name, {type, word}, s.constant, scoped);
        return false;
      }
      auto v = s.expr ? expression(*s.expr) : Value{s.type, ""};

      auto name = s.token;
      name.text = s.name;
      declare(name, v, s.constant);
    } else if (s.kind == "assign" || s.kind == "++" || s.kind == "--" ||
               s.kind == "+=" || s.kind == "-=" || s.kind == "*=" ||
               s.kind == "/=" || s.kind == "%=" || s.kind == "&=" ||
               s.kind == "|=" || s.kind == "^=" ||
               s.kind == "<<=" || s.kind == ">>=") {
      auto address = location(*s.target);
      if (address.type.role != Type::Raw) {
        auto word = aliasWord(*s.expr); // Retain first, including on self-assignment.
        releaseBinding(address);
        emit("store ptr " + word + ", ptr " + address.value);
        return false;
      }
      Value v;

      if (s.kind == "assign")
        v = expression(*s.expr);
      else {
        auto old = fresh();
        emit(old + " = load " + irType(address.type) + ", ptr " +
             address.value);
        retain({address.type, old});
        auto operand = s.expr ? expression(*s.expr)
                              : convert({Type::Int, "1"}, address.type);
        v = binary(s.kind.substr(0, s.kind.size() - 1),
                   {address.type, old}, operand);
      }
      releaseBinding(address);
      emit("store " + irType(v.type) + " " + v.value + ", ptr " +
           address.value);
    } else
      release(expression(*s.expr));
    return false;
  }

public:
  Generator(const Symbols &symbols, const FunctionAnalysis &analysis,
            const std::string &name)
      : functions(symbols), analysis(analysis), symbolPrefix(name) {}
  FunctionIR generate(const Function &f) {
    scopes.emplace_back();
    if (f.method) {
      const auto &self = f.params.front().type;
      const auto &fields = functions.records.at(self.record).fields;
      for (size_t i = 0; i < fields.size(); ++i) {
        auto address = fresh();
        emit(address + " = getelementptr " + irType(self) + ", ptr %arg0, i32 0, i32 " + std::to_string(i));
        bind(fields[i].name, {fields[i].type, address});
      }
      scopes.emplace_back();
    }
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
      else {
        retain({parameter.type, arg});
        declare(parameter.name, {parameter.type, arg});
      }
    }
    bool terminated = false;
    for (const auto &s : f.body)
      terminated = statement(*s);
    if (!terminated) {
      cleanupScopes(0);
      emit("ret void");
    }
    output << ") {\nentry:\n" << allocations.str() << code.str() << "}\n\n";
    return {globals.str(), output.str(), usesTextRuntime, usesHeapRuntime};
  }
};
} // namespace
FunctionIR lowerFunction(const Function &function, const Symbols &symbols,
                         const FunctionAnalysis &analysis) {
  if (!function.nativeSymbol.empty()) return {};
  return Generator(symbols, analysis, function.name.text).generate(function);
}
std::string emitRecords(const Symbols &symbols) {
  std::string ir;
  for (const auto &[name, record] : symbols.records) {
    ir += "%struct." + name + " = type { ";
    if (record.fields.empty()) ir += "i8"; // Give empty instances an addressable byte.
    for (size_t i = 0; i < record.fields.size(); ++i) {
      if (i) ir += ", ";
      ir += irType(record.fields[i].type);
    }
    ir += " }\n";
  }
  return ir;
}
std::string emitDeclarations(const Symbols &symbols) {
  std::string declarations;
  std::set<std::string> emitted;
  for (const auto &item : symbols) {
    const auto &signature = item.second;
    auto symbol = signature.nativeSymbol.empty() ? "duck." + item.first : signature.nativeSymbol;
    if (!emitted.insert(symbol).second) continue;
    declarations += "declare " + irType(signature.result) + " @" + symbol + "(";
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

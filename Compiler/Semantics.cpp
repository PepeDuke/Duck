#include "Pipeline.h"
#include "WideInteger.h"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>

namespace duck {
std::string typeName(Type type) {
  if (type.depth) {
    auto prefix = type.role == Type::Owner ? "owning " : type.role == Type::Alias ? "tracked " : "";
    return prefix + typeName(type.pointee()) + "*";
  }
  if (recordValue(type) || enumValue(type)) return type.record;
  if (customInteger(type))
    return (type.base == Type::UIntN ? "u" : "i") + std::to_string(type.bits / 8);
  switch (type.base) {
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
  case Type::String: return "string";
  case Type::Name: return "name";
  case Type::Char: return "char";
  case Type::Date: return "date";
  case Type::Time: return "time";
  case Type::DateTime: return "datetime";
  case Type::Big: return "big";
  case Type::UBig: return "ubig";
  case Type::Tiny: return "tiny";
  case Type::UTiny: return "utiny";
  case Type::Text:
    return "cstring";
  default:
    return "void";
  }
}
namespace {
bool leapYear(long long year) {
  return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}
long long daysInMonth(long long year, long long month) {
  if (month == 2) return leapYear(year) ? 29 : 28;
  return month == 4 || month == 6 || month == 9 || month == 11 ? 30 : 31;
}
// Mirrors the runtime checks in CalendarRuntime.cpp for constructors whose
// fields are all integer literals, so invalid constants never compile.
void validateCalendar(Type type, const std::vector<long long> &v, const Token &token) {
  const auto kind = typeName(type);
  auto range = [&](long long value, long long low, long long high, const std::string &field) {
    if (value < low || value > high)
      fail(token, "invalid " + kind + ": " + field + " " + std::to_string(value) +
                      " is outside " + std::to_string(low) + ".." + std::to_string(high));
  };
  if (type == Type::Char) {
    range(v[0], 0, 0x10FFFF, "code point");
    if (v[0] >= 0xD800 && v[0] <= 0xDFFF)
      fail(token, "invalid char: surrogate code point " + std::to_string(v[0]) +
                      " is not a Unicode scalar value");
    return;
  }
  size_t time = 0;
  if (type == Type::Date || type == Type::DateTime) {
    range(v[0], 1, 9999, "year");
    range(v[1], 1, 12, "month");
    const auto limit = daysInMonth(v[0], v[1]);
    if (v[2] < 1 || v[2] > limit)
      fail(token, "invalid " + kind + ": day " + std::to_string(v[2]) + " is outside 1.." +
                      std::to_string(limit) + " for month " + std::to_string(v[1]) +
                      " of year " + std::to_string(v[0]));
    time = 3;
  }
  if (type == Type::Time || type == Type::DateTime) {
    range(v[time], 0, 23, "hour");
    range(v[time + 1], 0, 59, "minute");
    range(v[time + 2], 0, 59, "second");
    if (v.size() > time + 3) range(v[time + 3], 0, 999999999, "nanosecond");
  }
}
// Rounds a decimal literal (digits * 10^exponent) to tiny's 69 significant
// digits, ties to even, exactly like the runtime. Returns the signed
// coefficient and exponent as LLVM integer constants.
std::pair<std::string, std::string> decimalLiteral(std::string digits, long long exponent,
                                                   bool negative, Type type, const Token &token) {
  const auto first = digits.find_first_not_of('0');
  if (first == std::string::npos) return {"0", "0"};
  digits = digits.substr(first);
  if (digits.size() > 69) {
    const auto dropped = digits.substr(69);
    digits.resize(69);
    exponent += (long long)dropped.size();
    const bool rest = dropped.find_first_not_of('0', 1) != std::string::npos;
    const bool odd = (digits.back() - '0') % 2 != 0;
    if (dropped[0] > '5' || (dropped[0] == '5' && (rest || odd))) {
      int i = 68;
      while (i >= 0 && digits[size_t(i)] == '9') digits[size_t(i--)] = '0';
      if (i < 0) {
        digits = "1" + std::string(68, '0');
        ++exponent;
      } else
        ++digits[size_t(i)];
    }
  }
  while (digits.size() > 1 && digits.back() == '0') {
    digits.pop_back();
    ++exponent;
  }
  const long long adjusted = exponent + (long long)digits.size() - 1;
  if (adjusted < -999999999 || adjusted > 999999999)
    fail(token, "decimal literal out of range for " + typeName(type) +
                    ": its leading digit must be within 10^-999999999..10^999999999");
  return {(negative ? "-" : "") + digits, std::to_string(exponent)};
}
struct Local {
  Type type;
  bool constant;
  bool initialized = true;
  bool receiver = false;
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
  const Expr *ownerDestination = nullptr;
  std::string container;
  bool method = false;
  bool staticInitializer = false; // Analyzing a static field's constant initializer.

  void require(Type actual, Type expected, const Token &token) {
    if (actual != expected)
      fail(token, "type mismatch: expected " + typeName(expected) + ", got " +
                      typeName(actual));
  }
  Local *findLocal(const Token &token) {
    for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
      auto local = scope->find(token.text);
      if (local != scope->end())
        return &local->second;
    }
    return nullptr;
  }
  Local &lookup(const Token &token) {
    if (auto local = findLocal(token)) return *local;
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
  // An enum operand decays to its underlying value; Print alone keeps the name.
  Type decay(const Expr &e, Type type) {
    if (!enumValue(type)) return type;
    const auto underlying = symbols.enums.at(type.record).type;
    analysis.conversions[&e] = underlying;
    return underlying;
  }
  // Day.Monday, unless Day is a local whose field is being read.
  const EnumInfo *enumOf(const Expr &e) {
    if (e.kind != "." || e.args[0]->kind != "name" || findLocal(e.args[0]->token)) return nullptr;
    auto found = symbols.enums.find(e.args[0]->token.text);
    return found == symbols.enums.end() ? nullptr : &found->second;
  }
  void convert(const Expr &e, Type from, Type to) {
    if (from == to)
      return;
    if (enumValue(from)) {
      const auto underlying = symbols.enums.at(from.record).type;
      if (underlying == to || (numeric(underlying) && numeric(to) && commonNumeric(underlying, to) == to) ||
          (arbitraryNumber(to) && arbitraryCommon(underlying, to) == to)) {
        analysis.conversions[&e] = to;
        return;
      }
    }
    if ((from.role == Type::Raw && from.depth && to == Type::Pointer) ||
        (numeric(from) && numeric(to) && commonNumeric(from, to) == to) ||
        (arbitraryNumber(to) && arbitraryCommon(from, to) == to)) {
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
  // A protected member is visible inside its declaring container, the
  // static containers nested in it, and containers that derive from it.
  void requireAccess(bool isProtected, const std::string &declaring,
                     const std::string &member, const Token &token) const {
    if (!isProtected || container == declaring || container.rfind(declaring + ".", 0) == 0 ||
        symbols.derivesFrom(container, declaring))
      return;
    fail(token, "'" + member + "' is a protected member of '" + declaring +
                    "'; access it through a public member instead");
  }
  void requireAccess(Symbols::const_iterator function, const Token &token) const {
    const auto &name = function->first;
    const auto dot = name.rfind('.');
    if (dot != std::string::npos)
      requireAccess(function->second.protectedAccess, name.substr(0, dot), memberName(name), token);
  }
  Symbols::const_iterator resolve(const std::string &op) const {
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
    return found;
  }
  // The qualified static field a name or dotted path denotes, resolved from
  // the innermost static container outward, or empty. Locals shadow statics.
  std::string staticPath(const Expr &e) {
    std::string path;
    const Expr *root = &e;
    while (root->kind == ".") {
      path = "." + root->token.text + path;
      root = root->args[0].get();
    }
    if (root->kind != "name" || findLocal(root->token)) return "";
    path = root->token.text + path;
    for (auto scope = container;; ) {
      const auto candidate = scope.empty() ? path : scope + "." + path;
      if (symbols.statics.count(candidate)) return candidate;
      if (scope.empty()) return "";
      const auto dot = scope.rfind('.');
      scope = dot == std::string::npos ? "" : scope.substr(0, dot);
    }
  }
  // True when e, or a dotted prefix of it, is static storage.
  bool staticStorage(const Expr &e) {
    for (const Expr *part = &e;; part = part->args[0].get()) {
      if (!staticPath(*part).empty()) return true;
      if (part->kind != ".") return false;
    }
  }
  // An addressable expression denotes existing storage. Dot inherits the
  // mutability of its base; dereferencing borrows the pointed-to storage.
  Local location(const Expr &e, bool read = true) {
    Local local{Type::Void, false};
    if (const auto qualified = staticPath(e); !qualified.empty()) {
      if (staticInitializer)
        fail(e.token, "field initializers cannot read static fields; use a constant");
      const auto &info = symbols.statics.at(qualified);
      const auto dot = qualified.rfind('.');
      requireAccess(info.protectedAccess, qualified.substr(0, dot), qualified.substr(dot + 1), e.token);
      analysis.staticFields[&e] = qualified;
      local = {info.type, info.constant};
    } else if (e.kind == "name") {
      local = lookup(e.token);
      if (local.receiver) analysis.receiverStorage.insert(&e);
      if (read) requireInitialized(e.token);
    } else if (e.kind == "unary" && e.token.text == "*") {
      auto ptr = expression(*e.args[0]);
      if (!ptr.depth || ptr == Type::Pointer) fail(e.token, "dereference requires a typed pointer");
      local.type = ptr.pointee();
    } else if (e.kind == ".") {
      if (enumOf(e))
        fail(e.token, "enum member '" + e.args[0]->token.text + "." + e.token.text +
                          "' is a constant");
      const auto &base = *e.args[0];
      if (base.addressable()) local = location(base);
      else {
        local.type = expression(base);
        if (recordValue(local.type)) fail(e.token, "field access requires addressable container storage");
      }
      if (local.type.depth) {
        local.type = local.type.pointee();
        local.constant = false; // A constant pointer can point to mutable storage.
      }
      if (!recordValue(local.type)) fail(e.token, "field access requires a container or a pointer to a container");
      MemberPath path;
      if (!symbols.findField(local.type.record, e.token.text, path))
        fail(e.token, "unknown container field '" + e.token.text + "'");
      requireAccess(path.field->protectedAccess, path.declaring, e.token.text, e.token);
      analysis.fieldIndices[&e] = path.indices;
      local.type = path.field->type;
    } else fail(e.token, "operand must be a mutable variable, field, or dereference");
    analysis.expressionTypes[&e] = local.type;
    return local;
  }
  bool heapStorage(const Expr &e) const {
    if (analysis.receiverStorage.count(&e)) return true;
    if (analysis.expressionTypes.at(&e).role != Type::Raw) return true;
    if (e.kind == "." || (e.kind == "unary" && e.token.text == "*"))
      return heapStorage(*e.args[0]);
    return false;
  }
  Type checkExpression(const Expr &e, Type expected) {
    const auto &op = e.token.text;
    if (e.kind == "initializer") {
      if (!recordValue(expected)) fail(e.token, "named initialization requires a container value type");
      std::set<std::string> assigned;
      for (const auto &field : e.args) {
        MemberPath path;
        if (!symbols.findField(expected.record, field->token.text, path))
          fail(field->token, "unknown container field");
        requireAccess(path.field->protectedAccess, path.declaring, field->token.text, field->token);
        if (path.field->parentView)
          fail(field->token, "initialize inherited fields by name, not through parent '" +
                                 field->token.text + "'");
        if (!assigned.insert(field->token.text).second) fail(field->token, "duplicate field initializer");
        analysis.fieldIndices[field.get()] = path.indices;
        expression(*field->args.front(), path.field->type);
      }
      return expected;
    }
    if (e.kind == "char")
      return Type::Char;
    if (e.kind == "typeMember") return typeLimit(e);
    if (e.kind == "construct") {
      const auto type = e.castType;
      const auto count = e.args.size();
      if (type == Type::DateTime && count == 2) {
        expression(*e.args[0], Type::Date);
        expression(*e.args[1], Type::Time);
        return type;
      }
      const bool valid = type == Type::Char ? count == 1
                       : type == Type::Date ? count == 3
                       : type == Type::Time ? count == 3 || count == 4
                                            : count == 6 || count == 7;
      if (!valid)
        fail(e.token, "wrong argument count for " + typeName(type) + "(" +
                          (type == Type::Char ? "code point"
                           : type == Type::Date ? "year, month, day"
                           : type == Type::Time ? "hour, minute, second[, nanosecond]"
                                                : "date, time) or datetime(year, month, day, "
                                                  "hour, minute, second[, nanosecond]") + ")");
      // Fields are integers evaluated left to right, then validated together.
      std::vector<long long> values;
      bool constant = true;
      for (const auto &argument : e.args) {
        expression(*argument, Type::Long);
        auto literal = analysis.integerConstants.find(argument.get());
        constant = constant && literal != analysis.integerConstants.end();
        if (constant) values.push_back(std::stoll(literal->second));
      }
      if (constant) validateCalendar(type, values, e.token);
      return type;
    }
    if (e.kind == "cast") {
      if (e.castType.depth && e.castType.base == Type::Record && symbols.records.count(e.castType.record) &&
          symbols.managedRecord(e.castType.record))
        fail(e.token, "cannot cast to a pointer to a container with string, name, or big fields");
      auto source = decay(*e.args[0], expression(*e.args[0]));
      if (source.role != Type::Raw) fail(e.token, "cannot cast a tracked heap reference");
      symbols.validateType(e.castType, e.token);
      if ((source.depth && e.castType.depth) ||
          (source.depth && integerWidth(e.castType) == 64) ||
          (integerWidth(source) == 64 && e.castType.depth)) return e.castType;
      if (managedText(source) && managedText(e.castType)) return e.castType;
      if (source == Type::Char && (integral(e.castType) || managedText(e.castType)))
        return e.castType;
      if (calendarValue(e.castType))
        fail(e.token, "use " + typeName(e.castType) + "(...) to construct a validated " +
                          typeName(e.castType));
      if (calendarValue(source))
        fail(e.token, "only char has explicit conversions: to an integer scalar value or "
                      "to string/name");
      if (arbitraryNumber(source) || arbitraryNumber(e.castType)) {
        const auto &target = e.castType;
        auto hostInteger = [](const Type &t) { return integral(t) && integerWidth(t) <= 64; };
        if ((bigInteger(target) && (bigInteger(source) || hostInteger(source))) ||
            (bigInteger(source) && hostInteger(target)) ||
            (decimalValue(target) && (decimalValue(source) || hostInteger(source))))
          return target;
        fail(e.token, "big/ubig convert only to and from each other and integers up to 64 bits; "
                      "tiny/utiny accept tiny/utiny and integers up to 64 bits");
      }
      if ((!numeric(source) && source != Type::Bool) ||
          (!numeric(e.castType) && e.castType != Type::Bool))
        fail(e.token, "casts currently require numeric/bool types or string/name encoding conversions");
      return e.castType;
    }
    if (e.kind == "real" && decimalValue(expected)) {
      // Exact decimal parsing: 0.1 is one tenth, not the nearest binary float.
      const auto marker = op.find_first_of("eE");
      const auto mantissa = op.substr(0, marker);
      long long exponent = 0;
      if (marker != std::string::npos) {
        try {
          exponent = std::stoll(op.substr(marker + 1));
        } catch (...) {
          fail(e.token, "decimal literal out of range for " + typeName(expected));
        }
        if (exponent < -4000000000LL || exponent > 4000000000LL)
          fail(e.token, "decimal literal out of range for " + typeName(expected));
      }
      const auto point = mantissa.find('.');
      auto digits = mantissa;
      if (point != std::string::npos) {
        digits.erase(point, 1);
        exponent -= (long long)(mantissa.size() - point - 1);
      }
      analysis.decimalConstants[&e] = decimalLiteral(digits, exponent, false, expected, e.token);
      return expected;
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
      const auto base = integerLiteralBase(digits);
      const auto magnitude = base == 10 ? digits : digits.substr(2);
      if (magnitude.empty())
        fail(e.token, "integer literal requires digits after its prefix");
      // Validate before conversion so malformed huge literals do not hide
      // invalid digits behind an overflow diagnostic.
      for (char digit : magnitude) {
        int value = digit >= '0' && digit <= '9' ? digit - '0'
                  : digit >= 'a' && digit <= 'f' ? digit - 'a' + 10
                  : digit >= 'A' && digit <= 'F' ? digit - 'A' + 10 : -1;
        if (value < 0 || value >= base)
          fail(e.token, "invalid integer literal");
      }
      // Magnitudes are arbitrary precision so wide iN/uN destinations can
      // accept literals beyond uint64. Bounds compare significant bit counts.
      const auto value = Magnitude::parse(magnitude, base);
      const auto bits = int(value.bits());
      // Signed range of a width: up to 2^(w-1)-1, or exactly 2^(w-1) when negated.
      auto fitsSigned = [&](int width) {
        return bits <= width - 1 || (negativeLiteral && bits == width && value.powerOfTwo());
      };
      Type type = negativeLiteral ? (fitsSigned(32) ? Type::Int : fitsSigned(64) ? Type::Long : Type::Void)
                : bits <= 31 ? Type::Int : bits <= 63 ? Type::Long : bits <= 64 ? Type::UInt64
                                                                     : Type::Void;
      if ((expected == Type::UBig || expected == Type::UTiny) && negativeLiteral && bits)
        fail(e.token, "integer literal out of range for " + typeName(expected));
      if (decimalValue(expected)) {
        analysis.decimalConstants[&e] =
            decimalLiteral(value.decimal(), 0, negativeLiteral && bits, expected, e.token);
        if (negativeLiteral) analysis.expressionTypes.emplace(e.args[0].get(), expected);
        return expected;
      }
      if (bigInteger(expected))
        type = expected; // Any magnitude; big literals become constant storage.
      else if (integral(expected) && expected != Type::Int) {
        const bool fits = unsignedInteger(expected) ? (negativeLiteral ? bits == 0 : bits <= integerWidth(expected))
                                                    : fitsSigned(integerWidth(expected));
        if (!fits)
          fail(e.token, "integer literal out of range for " + typeName(expected));
        type = expected;
      }
      if (type == Type::Void)
        fail(e.token, "integer literal out of range; a literal beyond the 64-bit types "
                      "needs a wider iN or uN destination or operand");
      const auto decimal = value.decimal();
      analysis.integerConstants[&e] = (negativeLiteral && bits ? "-" : "") + decimal;
      if (negativeLiteral) {
        analysis.expressionTypes.emplace(e.args[0].get(), type);
        analysis.integerConstants[e.args[0].get()] = decimal;
      }
      return type;
    }
    if (e.kind == "null")
      return expected.depth ? expected : Type::Pointer;
    if (e.kind == "bool")
      return Type::Bool;
    if (e.kind == "text")
      return textType(expected) ? expected : Type::String;
    if (auto info = enumOf(e)) {
      const auto index = info->find(e.token.text);
      if (index == info->members.size())
        fail(e.token, "unknown member '" + e.token.text + "' in enum '" + e.args[0]->token.text + "'");
      analysis.enumMembers[&e] = index;
      Type type = Type::Enum;
      type.record = e.args[0]->token.text;
      return type;
    }
    if (e.addressable())
      return location(e).type;
    if (e.kind == "unary" && op == "&") {
      const auto &arg = *e.args[0];
      const Expr *root = &arg;
      std::string suffix;
      while (root->kind == ".") {
        suffix = "." + root->token.text + suffix;
        root = root->args[0].get();
      }
      if (root->kind == "name") {
        bool local = false;
        for (const auto &scope : scopes) local |= scope.count(root->token.text) != 0;
        auto function = resolve(root->token.text + suffix);
        if (!local && function != symbols.end()) {
          const auto &sig = function->second;
          if (sig.method) fail(arg.token, "instance method requires a receiver");
          requireAccess(function, arg.token);
          if (sig.result.role != Type::Raw || (sig.result != Type::Void && !nativeScalar(sig.result)))
            fail(arg.token, "unsupported callback return ABI");
          for (const auto &p : sig.parameters)
            if (p.reference || !nativeScalar(p.type)) fail(arg.token, "unsupported callback parameter ABI");
          analysis.callTargets[&e] = function->first;
          return Type::Pointer;
        }
      }
      auto local = location(arg);
      if (heapStorage(arg)) fail(arg.token, "cannot expose the address of tracked heap storage");
      if (local.constant || managedValue(local.type))
        fail(arg.token, "cannot take the address of constant or managed text storage");
      return local.type.pointerTo();
    }
    if (e.kind == "call") {
      if (op == "Length" || op == "CodeUnitAt") {
        if (e.args.size() != (op == "Length" ? 1u : 2u))
          fail(e.token, "wrong argument count for '" + op + "'");
        auto text = expression(*e.args[0]);
        if (!managedText(text)) fail(e.token, op + " requires string or name");
        if (op == "CodeUnitAt") expression(*e.args[1], Type::Long);
        return op == "Length" ? Type::Long : Type::UInt32;
      }
      if (op == "Print") {
        if (e.args.size() != 1)
          fail(e.token, "Print expects one argument");
        auto printed = expression(*e.args[0]);
        if (printed == Type::Void)
          fail(e.token, "cannot print void");
        if (recordValue(printed)) fail(e.token, "cannot print a container");
        if (printed.depth)
          fail(e.token, "cannot print an opaque pointer; compare it with null instead");
        return Type::Void;
      }
      auto found = resolve(op);
      // Inside an instance method an unqualified name finds the container's
      // own or inherited methods before static or global functions.
      if (method && !e.receiver && op.find('.') == std::string::npos) {
        MemberPath path;
        auto member = symbols.findMethod(container, op, path);
        if (member != symbols.end()) {
          found = member;
          if (!path.indices.empty()) analysis.receiverPaths[&e] = {container, path.indices};
        }
      }
      bool instance = false;
      if (e.receiver) {
        const Expr *root = e.receiver.get();
        while (root->kind == ".") root = root->args[0].get();
        instance = root->kind != "name" || findLocal(root->token) || staticStorage(*e.receiver);
        if (instance) {
          const auto &base = *e.receiver;
          Type t;
          if (base.addressable()) {
            auto local = location(base);
            t = local.type;
            if (!t.depth && local.constant) fail(base.token, "cannot call a mutable method on a constant");
          } else t = expression(base);
          if (t.depth) t = t.pointee();
          if (!recordValue(t)) fail(base.token, "method receiver requires a container");
          MemberPath path;
          found = symbols.findMethod(t.record, op.substr(op.rfind('.') + 1), path);
          if (found == symbols.end()) fail(e.token, "unknown instance method '" + op + "'");
          if (!path.indices.empty()) analysis.receiverPaths[&e] = {t.record, path.indices};
        }
      }
      if (found == symbols.end() && !e.receiver && !symbols.records.count(op)) {
        Type builtin;
        if (calendarCall(e, builtin)) return builtin;
      }
      if (found == symbols.end() && !e.receiver && symbols.records.count(op)) {
        if (!e.args.empty()) fail(e.token, "default container construction takes no arguments");
        Type t = Type::Record;
        t.record = op;
        return t;
      }
      if (found == symbols.end()) {
        // Meter.Read() or a bare Read() in a static body naming an instance method.
        const auto dot = op.rfind('.');
        const auto owner = dot == std::string::npos ? container : op.substr(0, dot);
        if (!instance && symbols.records.count(owner) &&
            symbols.count(methodKey(owner, op.substr(dot == std::string::npos ? 0 : dot + 1))))
          fail(e.token, "'" + op + "' is an instance method; call it on a '" + owner + "' object");
        fail(e.token, "unknown function '" + op + "'");
      }
      requireAccess(found, e.token);
      analysis.callTargets[&e] = found->first;
      const auto &signature = found->second;
      if (signature.result.role == Type::Owner && ownerDestination != &e)
        fail(e.token, "receive an owning function result with ::");
      if (signature.method && !instance && (!method || e.receiver))
        fail(e.token, "instance method requires a receiver");
      const size_t offset = signature.method ? 1 : 0;
      if (signature.parameters.size() != e.args.size() + offset)
        fail(e.token, "wrong argument count for '" + op + "'");
      for (size_t i = 0; i < e.args.size(); ++i) {
        const auto &argument = *e.args[i];
        const auto &parameter = signature.parameters[i + offset];
        if (parameter.reference) {
          auto local = location(argument);
          if (heapStorage(argument)) fail(argument.token, "cannot expose tracked heap storage by reference");
          if (local.constant) fail(argument.token, "cannot pass a constant to a mutable reference");
          require(local.type, parameter.type, argument.token);
        } else if (argument.kind == "name" && lookup(argument.token).type.role != Type::Raw) {
          auto tracked = expression(argument);
          tracked.role = Type::Raw;
          if (!signature.nonescapingParameters.count(i + offset) || tracked != parameter.type)
            fail(argument.token, "tracked pointer requires a proven nonescaping parameter of matching type");
          analysis.conversions[&argument] = parameter.type;
        } else
          expression(argument, parameter.type);
      }
      return signature.result;
    }
    auto first = expression(*e.args[0],
                            (e.kind == "unary" && op == "-" &&
                             (numeric(expected) || arbitraryNumber(expected))) ||
                            (e.kind == "binary" && op == "+" && managedText(expected)) ||
                            leftLiteralAdopts(e, expected)
                                ? expected
                                : Type::Void);
    // Both conditional branches keep their enum type.
    if (e.kind != "conditional") first = decay(*e.args[0], first);
    if (e.kind == "conditional") {
      require(first, Type::Bool, e.token);
      auto yes = expression(*e.args[1], expected);
      if (yes.role != Type::Raw) fail(e.token, "tracked aliases require a named source");
      if (yes == Type::Void)
        fail(e.token, "conditional branches must produce values");
      auto hint = expected;
      if (hint == Type::Void && textType(yes) && e.args[1]->kind != "text") hint = yes;
      auto no = expression(*e.args[2], hint);
      if (no.role != Type::Raw) fail(e.token, "tracked aliases require a named source");
      if (expected == Type::Void && e.args[1]->kind == "text" && textType(no)) {
        yes = no;
        analysis.expressionTypes[e.args[1].get()] = no;
      }
      if (arbitraryNumber(yes) || arbitraryNumber(no)) {
        const auto type = arbitraryCommon(yes, no);
        if (type == Type::Void) require(no, yes, e.token);
        convert(*e.args[1], yes, type);
        convert(*e.args[2], no, type);
        return type;
      }
      if (numeric(yes) && numeric(no)) {
        auto type = common(yes, no, e.token);
        convert(*e.args[1], yes, type);
        convert(*e.args[2], no, type);
        return type;
      }
      if (yes.depth && no.depth && (e.args[1]->kind == "null" || e.args[2]->kind == "null")) {
        Type t = e.args[1]->kind == "null" ? no : yes;
        if (e.args[1]->kind == "null") analysis.expressionTypes[e.args[1].get()] = t;
        if (e.args[2]->kind == "null") analysis.expressionTypes[e.args[2].get()] = t;
        return t;
      }
      require(no, yes, e.token);
      return yes;
    }
    if (e.kind == "unary" && arbitraryNumber(first)) {
      if (op != "-") fail(e.token, typeName(first) + " supports only arithmetic negation");
      if (first == Type::UBig || first == Type::UTiny)
        fail(e.token, "cannot negate unsigned " + typeName(first));
      return first;
    }
    if (e.kind == "unary") {
      if (op == "!")
        require(first, Type::Bool, e.token);
      else if (op == "~" && !integral(first))
        fail(e.token, "bitwise operators require integer operands");
      else if (!numeric(first))
        require(first, Type::Int, e.token);
      return first;
    }
    if (op == "&&" || op == "||") {
      require(first, Type::Bool, e.token);
      require(decay(*e.args[1], expression(*e.args[1])), Type::Bool, e.token);
      return Type::Bool;
    }
    Type hint = Type::Void;
    if (op == "+" && managedText(expected)) hint = expected;
    // Only beyond 64 bits can a literal need its operand's width to be representable.
    else if (customInteger(first) && integerWidth(first) > 64 && op != "<<" && op != ">>" &&
             (e.args[1]->kind == "int" ||
              (e.args[1]->kind == "unary" && e.args[1]->token.text == "-" &&
               e.args[1]->args[0]->kind == "int")))
      hint = first;
    else if (arbitraryNumber(first) && op != "<<" && op != ">>" &&
             (e.args[1]->kind == "int" || e.args[1]->kind == "real" ||
              (e.args[1]->kind == "unary" && e.args[1]->token.text == "-" &&
               (e.args[1]->args[0]->kind == "int" || e.args[1]->args[0]->kind == "real"))))
      hint = first; // Literals take big/tiny precision from the left operand.
    else if (textType(first) &&
             (e.args[0]->kind != "text" || e.args[1]->kind == "text")) hint = first;
    auto second = decay(*e.args[1], expression(*e.args[1], hint));
    // Literals adopt the other operand's text type; named values never change encoding implicitly.
    if (e.args[0]->kind == "text" && textType(second) && textType(first)) {
      first = second;
      analysis.expressionTypes[e.args[0].get()] = second;
    }
    if ((op == "==" || op == "!=") && first.depth && second.depth &&
        (first.pointee() == second.pointee() || first == Type::Pointer || second == Type::Pointer))
      return Type::Bool;
    if (managedText(first) && managedText(second)) {
      require(second, first, e.token);
      if (op == "+") return first;
      if (op == "==" || op == "!=") return Type::Bool;
      fail(e.token, "text supports only concatenation and equality");
    }
    if (arbitraryNumber(first) || arbitraryNumber(second)) {
      const auto type = arbitraryCommon(first, second);
      if (type == Type::Void)
        fail(e.token, "big/ubig and tiny/utiny combine only with their own kind and integers "
                      "up to 64 bits; got " + typeName(first) + " and " + typeName(second));
      const bool comparison = op == "==" || op == "!=" || op == "<" || op == ">" ||
                              op == "<=" || op == ">=";
      if (!comparison && op != "+" && op != "-" && op != "*" && op != "/" &&
          !(op == "%" && bigInteger(type)))
        fail(e.token, typeName(type) + " supports + - * /" +
                          (bigInteger(type) ? " %" : "") + " and comparisons");
      convert(*e.args[0], first, type);
      convert(*e.args[1], second, type);
      return comparison ? Type::Bool : type;
    }
    if (calendarValue(first) || calendarValue(second)) {
      if (op != "==" && op != "!=" && op != "<" && op != ">" && op != "<=" && op != ">=")
        fail(e.token, typeName(calendarValue(first) ? first : second) +
                          " values support only comparison operators");
      require(second, first, e.token);
      return Type::Bool;
    }
    if (op == "&" || op == "|" || op == "^" || op == "<<" || op == ">>") {
      if (!integral(first) || !integral(second))
        fail(e.token, "bitwise operators require integer operands");
      // Shift counts retain their own type until the backend checks bounds.
      if (op == "<<" || op == ">>")
        return first;
    }
    if (numeric(first) && numeric(second)) {
      auto type = common(first, second, e.token);
      convert(*e.args[0], first, type);
      convert(*e.args[1], second, type);
      first = type;
    } else
      require(second, first, e.token);
    if (op == "==" || op == "!=") {
      if (!numeric(first) && first != Type::Bool && first != Type::Pointer)
        fail(e.token, "equality supports numeric, bool, and opaque pointer values only");
      return Type::Bool;
    }
    if (!numeric(first) || (op == "%" && !integral(first)))
      require(first, Type::Int, e.token);
    return op == "<" || op == ">" || op == "<=" || op == ">=" ? Type::Bool
                                                              : first;
  }
  // T.Min and T.Max for integer and floating types, as typed constants. For
  // float and double, Min is the lowest finite value, -Max.
  Type typeLimit(const Expr &e) {
    const auto type = e.castType;
    const auto &member = e.token.text;
    if (!integral(type) && !floating(type))
      fail(e.token, "type '" + typeName(type) + "' has no Min or Max" +
                        (arbitraryNumber(type) ? "; its range is unbounded" : ""));
    if (member != "Min" && member != "Max")
      fail(e.token, "type '" + typeName(type) + "' has no member '" + member +
                        "'; use Min or Max");
    if (floating(type)) {
      double value = type == Type::Float ? double(std::numeric_limits<float>::max())
                                         : std::numeric_limits<double>::max();
      if (member == "Min") value = -value;
      uint64_t bits = 0;
      std::memcpy(&bits, &value, sizeof(bits));
      std::ostringstream encoded;
      encoded << "0x" << std::hex << std::uppercase << std::setfill('0') << std::setw(16) << bits;
      analysis.floatingConstants[&e] = encoded.str();
      return type;
    }
    const auto width = unsigned(integerWidth(type));
    const bool isUnsigned = unsignedInteger(type);
    if (member == "Min") {
      analysis.integerConstants[&e] =
          isUnsigned ? "0" : "-" + Magnitude::powerOfTwo(width - 1).decimal();
      return type;
    }
    // All ones in the value bits: width bits unsigned, width - 1 signed.
    const auto bits = isUnsigned ? width : width - 1;
    Magnitude maximum;
    maximum.limbs.assign((bits + 31) / 32, 0xFFFFFFFFu);
    if (bits % 32) maximum.limbs.back() = (uint32_t{1} << (bits % 32)) - 1;
    analysis.integerConstants[&e] = maximum.decimal();
    return type;
  }
  // A left literal operand of big/tiny arithmetic takes the destination type
  // where it could not convert implicitly anyway: integer literals beyond 64
  // bits into big, and real literals into tiny. Integer literals into tiny keep
  // integer arithmetic, so tiny x = 1 / 3 is 0, as for double.
  static bool leftLiteralAdopts(const Expr &e, Type expected) {
    const auto &op = e.token.text;
    if (e.kind != "binary" || e.args.empty() ||
        (op != "+" && op != "-" && op != "*" && op != "/" && op != "%"))
      return false;
    const Expr *literal = e.args[0].get();
    if (literal->kind == "unary" && literal->token.text == "-") literal = literal->args[0].get();
    return (bigInteger(expected) && literal->kind == "int") ||
           (decimalValue(expected) && literal->kind == "real");
  }
  // Calendar accessors are built-ins only when no user function or container
  // has the same name, so existing programs keep their own Day or Second.
  bool calendarCall(const Expr &e, Type &type) {
    const auto &op = e.token.text;
    const bool datePart = op == "Year" || op == "Month" || op == "Day" || op == "DayOfWeek";
    const bool timePart = op == "Hour" || op == "Minute" || op == "Second" || op == "Nanosecond";
    if (!datePart && !timePart && op != "DateOf" && op != "TimeOf" && op != "AddDays" &&
        op != "DaysBetween")
      return false;
    const size_t count = op == "AddDays" || op == "DaysBetween" ? 2 : 1;
    if (e.args.size() != count) fail(e.token, "wrong argument count for '" + op + "'");
    analysis.calendarCalls.insert(&e);
    const auto value = expression(*e.args[0]);
    auto accept = [&](bool valid, const std::string &expected) {
      if (!valid)
        fail(e.args[0]->token, op + " requires " + expected + ", got " + typeName(value));
    };
    if (datePart) {
      accept(value == Type::Date || value == Type::DateTime, "a date or datetime");
      type = Type::Int;
    } else if (timePart) {
      accept(value == Type::Time || value == Type::DateTime, "a time or datetime");
      type = Type::Int;
    } else if (op == "DateOf" || op == "TimeOf") {
      accept(value == Type::DateTime, "a datetime");
      type = op == "DateOf" ? Type::Date : Type::Time;
    } else if (op == "AddDays") {
      accept(value == Type::Date || value == Type::DateTime, "a date or datetime");
      expression(*e.args[1], Type::Long);
      type = value;
    } else {
      accept(value == Type::Date, "a date");
      expression(*e.args[1], Type::Date);
      type = Type::Long;
    }
    return true;
  }
  bool sequence(const std::vector<std::unique_ptr<Stmt>> &statements) {
    bool terminated = false;
    for (const auto &s : statements) {
      if (terminated)
        fail(s->token, "unreachable statement");
      terminated = statement(*s);
    }
    // A local alias may use the payload directly when neither binding can
    // escape or be cleared/moved for the rest of this lexical scope.
    for (size_t i = 0; i < statements.size(); ++i) {
      const auto &s = *statements[i];
      if (s.kind != "let" || analysis.localTypes.at(&s).role != Type::Alias ||
          analysis.expressionTypes.at(s.expr.get()).role != Type::Owner) continue;
      bool scoped = true;
      for (size_t j = i + 1; scoped && j < statements.size(); ++j)
        scoped = borrowsOnly(*statements[j], s.name) &&
                 borrowsOnly(*statements[j], s.expr->token.text);
      if (scoped) analysis.scopedBorrows.insert(&s);
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
    if (s.kind == "returnOwner") {
      if (!s.expr || s.expr->kind != "name" || lookup(s.expr->token).constant ||
          expression(*s.expr) != result)
        fail(s.token, "return :: requires a mutable owning local of the declared type");
      return true;
    }
    if (s.kind == "return") {
      if (result.role == Type::Owner) fail(s.token, "owning function requires return :: on every return");
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
    if (s.kind == "let" || s.kind == "own") {
      symbols.validateType(s.type, s.token);
      // Inspect the initializer before choosing between raw storage and an alias.
      Type hint = s.type;
      if (s.expr && s.type.depth && s.expr->kind != "null") {
        const bool literal = s.expr->kind == "int" || s.expr->kind == "real" ||
            (s.expr->kind == "unary" && s.expr->token.text == "-" && s.expr->args[0]->kind == "int");
        hint = (literal && numeric(s.type.pointee())) || s.expr->kind == "initializer"
                   ? s.type.pointee() : Type(Type::Void);
      }
      if (s.kind == "own") ownerDestination = s.expr.get();
      auto type = s.expr ? expression(*s.expr, hint) : s.type;
      ownerDestination = nullptr;
      if (s.kind == "let" && s.type.depth && heapValue(s.type.pointee()) && heapValue(type)) {
        convert(*s.expr, type, s.type.pointee());
        type = s.type;
        type.role = Type::Owner;
      } else if (s.kind == "own") {
        if (s.expr && type.depth) {
          if (type.role != Type::Owner || (s.expr->kind != "call" &&
              (s.expr->kind != "name" || lookup(s.expr->token).constant)))
            fail(s.token, "ownership transfer requires a mutable owning source");
        } else if (s.expr) {
          if (!heapValue(type))
            fail(s.token, "heap allocation requires a scalar or container value");
          type = type.pointerTo();
        }
        type.role = Type::Owner;
      } else if (type.role != Type::Raw) {
        if (s.expr->kind != "name") fail(s.token, "tracked aliases require a named source");
        type.role = Type::Alias;
      } else if (s.expr && s.type.depth) {
        convert(*s.expr, type, s.type);
        type = s.type;
      }
      if (type == Type::Void)
        fail(s.token, "locals currently support only values, not void");
      if (s.type != Type::Void) {
        auto declared = s.type;
        declared.role = type.role;
        require(type, declared, s.token);
      }
      if (type.role == Type::Owner && type.depth == 1 && symbols.managed(type.pointee()))
        fail(s.token, "heap containers with string, name, or big fields are not supported yet");
      analysis.localTypes[&s] = type;
      auto name = s.token;
      name.text = s.name;
      bind(name, {type, s.constant, s.kind == "own" || s.expr != nullptr || recordValue(type)});
    } else if (s.kind == "expr")
      expression(*s.expr);
    else {
      auto local = location(*s.target, s.kind != "assign");
      if (local.constant)
        fail(s.token, "cannot modify constant '" + s.target->token.text + "'");
      if (local.type.role != Type::Raw) {
        if (s.kind != "assign" || (s.expr->kind != "null" &&
            (local.type.role != Type::Alias || s.expr->kind != "name")))
          fail(s.token, "tracked binding requires null or an alias assignment");
        auto source = expression(*s.expr);
        if (s.expr->kind != "null" && (source.role == Type::Raw ||
            source.pointee() != local.type.pointee()))
          fail(s.token, "incompatible tracked alias source");
        return false;
      }
      const bool bitwise = s.kind == "&=" || s.kind == "|=" || s.kind == "^=" ||
                           s.kind == "<<=" || s.kind == ">>=";
      if (bitwise && !integral(local.type))
        fail(s.token, "bitwise operators require integer operands");
      const bool arbitraryArithmetic =
          arbitraryNumber(local.type) &&
          (s.kind == "+=" || s.kind == "-=" || s.kind == "*=" || s.kind == "/=" ||
           s.kind == "++" || s.kind == "--" || (s.kind == "%=" && bigInteger(local.type)));
      if (s.kind != "assign" && !(s.kind == "+=" && managedText(local.type)) && !arbitraryArithmetic &&
          (!numeric(local.type) || (s.kind == "%=" && !integral(local.type))))
        require(local.type, Type::Int, s.token);
      if (s.expr) {
        if (s.kind == "<<=" || s.kind == ">>=") {
          if (!integral(expression(*s.expr)))
            fail(s.expr->token, "bitwise operators require integer operands");
        } else
          expression(*s.expr, local.type);
      }
      if (s.target->kind == "name" && !analysis.staticFields.count(s.target.get()))
        lookup(s.target->token).initialized = true;
    }
    return false;
  }

public:
  Analyzer(const Symbols &symbols, Type result)
      : symbols(symbols), result(result) {}
  // The LLVM constant for a static field initializer. Only literals, type
  // limits, and enum members of the field's type are compile-time constants.
  std::string constant(const std::string &qualified, const Expr &e, Type type) {
    container = qualified.substr(0, qualified.rfind('.'));
    staticInitializer = true;
    scopes.emplace_back();
    if (bigInteger(type))
      fail(e.token, "big fields cannot have an initializer yet; they start at zero");
    if (recordValue(type))
      fail(e.token, "container-typed fields start from that container's defaults; "
                    "initializers are not supported yet");
    expression(e, type);
    // A string or name literal; lowering emits it as immortal constant storage.
    if (e.kind == "text" && managedText(type)) return "";
    const bool converted = analysis.conversions.count(&e) != 0;
    auto encode = [](double value) {
      uint64_t bits = 0;
      std::memcpy(&bits, &value, sizeof(bits));
      std::ostringstream encoded;
      encoded << "0x" << std::hex << std::uppercase << std::setfill('0') << std::setw(16) << bits;
      return encoded.str();
    };
    if (e.kind == "bool") return e.token.text;
    if (e.kind == "null") return "null";
    if (auto member = analysis.enumMembers.find(&e); member != analysis.enumMembers.end() && !converted)
      return std::to_string(member->second);
    if (auto integer = analysis.integerConstants.find(&e); integer != analysis.integerConstants.end()) {
      if (!converted || integral(type)) return integer->second;
      if (floating(type)) {
        const auto value = std::stold(integer->second);
        return encode(type == Type::Float ? double(float(value)) : double(value));
      }
    }
    if (auto real = analysis.floatingConstants.find(&e); real != analysis.floatingConstants.end() && !converted)
      return real->second;
    // A negated floating literal: flip the sign bit of the encoded value.
    if (e.kind == "unary" && e.token.text == "-" && !converted) {
      auto real = analysis.floatingConstants.find(e.args[0].get());
      if (real != analysis.floatingConstants.end()) {
        auto bits = std::stoull(real->second.substr(2), nullptr, 16) ^ (uint64_t(1) << 63);
        double value = 0;
        std::memcpy(&value, &bits, sizeof(bits));
        return encode(value);
      }
    }
    fail(e.token, "field initializer must be a literal, type limit, or enum member "
                  "of the field's type");
  }
  FunctionAnalysis run(const Function &function) {
    auto dot = function.name.text.rfind('.');
    if (dot != std::string::npos)
      container = function.name.text.substr(0, dot);
    scopes.emplace_back();
    method = function.method;
    if (method) {
      for (const auto &path : symbols.visibleFields(container))
        bind(path.field->name, {path.field->type, false, true, true});
      scopes.emplace_back();
    }
    symbols.validateType(result, function.name);
    for (const auto &parameter : function.params) {
      symbols.validateType(parameter.type, parameter.name);
      bind(parameter.name, {parameter.type, false, true, method && &parameter == &function.params.front()});
    }
    if (!function.nativeSymbol.empty())
      return std::move(analysis);
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
void resolveFieldDefaults(Symbols &symbols) {
  for (auto &[name, record] : symbols.records)
    for (auto &field : record.fields)
      if (field.initializer)
        field.initialValue = Analyzer(symbols, Type::Void)
                                 .constant(name + "." + field.name.text, *field.initializer, field.type);
}
void resolveStaticInitializers(Symbols &symbols) {
  for (auto &[name, info] : symbols.statics) {
    if (info.imported) continue;
    info.value = info.initializer ? Analyzer(symbols, Type::Void).constant(name, *info.initializer, info.type)
                                  : "zeroinitializer";
  }
}
} // namespace duck

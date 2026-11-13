#include <functional>
#include "Literals.h"
#include "Pipeline.h"
#include "WideInteger.h"
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <locale>
#include <optional>
#include <regex>
#include <sstream>

namespace duck {
namespace {
// Signed decimal text + 1, for implicit values of any integer width.
std::string successor(std::string value) {
  const bool negative = value[0] == '-';
  std::string digits = negative ? value.substr(1) : value;
  size_t i = digits.size();

  if (negative) {
    while (digits[i - 1] == '0') digits[--i] = '9';

    --digits[i - 1];
    const auto first = digits.find_first_not_of('0');
    digits = first == std::string::npos ? "0" : digits.substr(first);
    return digits == "0" ? digits : "-" + digits;
  }

  while (i > 0 && digits[i - 1] == '9') digits[--i] = '0';
  if (i == 0) return "1" + digits;

  ++digits[i - 1];
  return digits;
}
bool fitsInteger(const std::string &value, const Type &type) {
  const bool negative = value[0] == '-';
  const auto magnitude = Magnitude::parse(negative ? value.substr(1) : value, 10);
  const int bits = int(magnitude.bits()), width = integerWidth(type);

  if (unsignedInteger(type)) return !negative && bits <= width;

  return bits <= width - 1 || (negative && bits == width && magnitude.powerOfTwo());
}
// A constant from an enum value expression. Integers are evaluated in 512
// bits, wider than any underlying type, and range-checked at the end.
struct EnumConstant {
  enum Kind { Integer, Real, Text } kind = Integer;
  Bits bits;
  double real = 0;
  std::string text;
};
bool negativeBits(const Bits &b) { return b.bit(511); }
std::string bitsDecimal(const Bits &b) { return negativeBits(b) ? "-" + b.negate().decimal() : b.decimal(); }
double bitsReal(const Bits &b) { return std::stod(bitsDecimal(b)); }
const char *constantKind(const EnumConstant &c) {
  return c.kind == EnumConstant::Integer ? "an integer" : c.kind == EnumConstant::Real ? "a decimal number" : "text";
}
// A literal operand: an integer, a decimal number, or a string. Empty for
// any other expression.
std::optional<EnumConstant> literalConstant(const Expr &e) {
  EnumConstant c;

  if (e.kind == "int") {
    const auto &text = e.token.text;
    const int base = integerLiteralBase(text);
    const auto digits = base == 10 ? text : text.substr(2);

    if (digits.empty()) fail(e.token, "integer literal requires digits after its prefix");

    for (char d : digits)
      if (const int digit = digitValue(d); digit < 0 || digit >= base) fail(e.token, "invalid integer literal");

    const auto magnitude = Magnitude::parse(digits, base);

    if (magnitude.bits() > 511) fail(e.token, "integer literal is too large for an enum value");

    c.bits = Bits::parse(magnitude.decimal());
    return c;
  }

  if (e.kind == "real") {
    c.kind = EnumConstant::Real;

    if (!parseFloatingLiteral(e.token.text, false, c.real))
      fail(e.token, "invalid or out-of-range floating literal");

    return c;
  }

  if (e.kind == "text" && !e.token.character) {
    c.kind = EnumConstant::Text;
    c.text = e.token.text;
    return c;
  }

  return std::nullopt;
}
EnumConstant unaryConstant(const Token &op, EnumConstant c) {
  if (op.text == "-" && c.kind == EnumConstant::Integer) c.bits = c.bits.negate();
  else if (op.text == "-" && c.kind == EnumConstant::Real) c.real = -c.real;
  else if (op.text == "~" && c.kind == EnumConstant::Integer) {
    for (auto &w : c.bits.words) w = ~w;
  } else
    fail(op, "operator '" + op.text + "' does not apply to " + constantKind(c) + " in an enum value");

  return c;
}
Bits absolute(const Bits &v) { return negativeBits(v) ? v.negate() : v; }
unsigned bitLength(const Bits &v) {
  for (unsigned i = 512; i > 0; --i)
    if (v.bit(i - 1)) return i;

  return 0;
}
EnumConstant binaryConstant(const Token &token, EnumConstant a, EnumConstant b) {
  const auto &op = token.text;

  if (a.kind == EnumConstant::Text || b.kind == EnumConstant::Text) {
    if (op != "+" || a.kind != b.kind)
      fail(token, "enum text values can only be joined with '+' to other text");

    a.text += b.text;
    return a;
  }

  if (a.kind == EnumConstant::Real || b.kind == EnumConstant::Real) {
    const double x = a.kind == EnumConstant::Real ? a.real : bitsReal(a.bits);
    const double y = b.kind == EnumConstant::Real ? b.real : bitsReal(b.bits);
    EnumConstant r;
    r.kind = EnumConstant::Real;

    if (op == "+") r.real = x + y;
    else if (op == "-") r.real = x - y;
    else if (op == "*") r.real = x * y;
    else if (op == "/") r.real = x / y;
    else fail(token, "operator '" + op + "' does not apply to decimal numbers in an enum value");

    if (!std::isfinite(r.real)) fail(token, "enum value is not a finite number");

    return r;
  }

  // Signed 512-bit arithmetic. Division truncates toward zero, as at run time.
  const bool na = negativeBits(a.bits), nb = negativeBits(b.bits);
  EnumConstant r;

  if (op == "+") r.bits = a.bits.add(b.bits);
  else if (op == "-") r.bits = a.bits.add(b.bits.negate());
  else if (op == "*") {
    // Operands stay far below 2^256 in practice; larger ones would wrap.
    if (bitLength(absolute(a.bits)) + bitLength(absolute(b.bits)) > 510) fail(token, "enum value overflows");

    r.bits = a.bits.multiply(b.bits);
  } else if (op == "/" || op == "%") {
    if (b.bits.zero()) fail(token, "division by zero in an enum value");

    auto [q, rem] = absolute(a.bits).divide(absolute(b.bits), 512);
    r.bits = op == "/" ? (na != nb ? q.negate() : q) : (na ? rem.negate() : rem);
  } else if (op == "<<" || op == ">>") {
    if (nb || b.bits.compare(Bits(511)) > 0) fail(token, "enum value shift count out of range");

    r.bits = a.bits.shift(b.bits.words[0], 512, op == ">>", na);
  } else if (op == "&" || op == "|" || op == "^") {
    for (size_t i = 0; i < r.bits.words.size(); ++i)
      r.bits.words[i] = op == "&" ? a.bits.words[i] & b.bits.words[i]
                        : op == "|" ? a.bits.words[i] | b.bits.words[i]
                                    : a.bits.words[i] ^ b.bits.words[i];
  } else
    fail(token, "operator '" + op + "' is not allowed in an enum value");

  return r;
}
// The text form EnumInfo stores for a constant of the enum's type, or a
// diagnostic at token when the constant has the wrong kind or range.
std::string storedValue(const EnumConstant &c, const Type &type, const Token &token, const std::string &member) {
  const auto subject = member.empty() ? std::string("the value") : "enum member '" + member + "'";

  if (integral(type)) {
    if (c.kind != EnumConstant::Integer)
      fail(token, subject + " requires an integer value, not " + constantKind(c));

    const auto value = bitsDecimal(c.bits);

    if (!fitsInteger(value, type)) fail(token, "enum value " + value + " is out of range for " + typeName(type));

    return value;
  }

  if (floating(type)) {
    if (c.kind == EnumConstant::Text) fail(token, subject + " requires a decimal number value, not text");

    double value = c.kind == EnumConstant::Real ? c.real : bitsReal(c.bits);

    if (type == Type::Float) value = double(float(value));
    if (!std::isfinite(value)) fail(token, "enum value is out of range for " + typeName(type));

    return floatingConstantText(value);
  }

  if (c.kind != EnumConstant::Text) fail(token, subject + " requires a string value, not " + constantKind(c));

  return c.text;
}
EnumConstant constantOf(const std::string &stored, const Type &type) {
  EnumConstant c;

  if (integral(type)) c.bits = Bits::parse(stored);
  else if (floating(type)) {
    c.kind = EnumConstant::Real;
    const uint64_t raw = std::stoull(stored.substr(2), nullptr, 16);
    std::memcpy(&c.real, &raw, sizeof(raw));
  } else {
    c.kind = EnumConstant::Text;
    c.text = stored;
  }

  return c;
}
// Assigns every member's value. A value may be a constant expression of
// literals, unary - and ~, arithmetic, shifts, bitwise operators, and members
// of this or another enum (`Monday`, `Day.Monday`, `Outer.Day.Monday`).
// Members are evaluated on demand, so a member may refer to a later one;
// a cycle is an error.
class EnumResolver {
public:
  struct Pending {
    EnumInfo info;
    std::vector<const EnumMember *> members;
    std::vector<int> state; // 0 unvisited, 1 in progress, 2 done.
  };
  std::map<std::string, Pending> enums;

  std::string memberValue(const std::string &name, size_t index) {
    auto &pending = enums.at(name);
    const auto &member = *pending.members[index];
    const auto &label = member.name.text;

    if (pending.state[index] == 2) return pending.info.values[index];
    if (pending.state[index] == 1)
      fail(member.name, "enum member '" + name + "." + label + "' depends on its own value");

    pending.state[index] = 1;
    const auto type = pending.info.type;
    std::string value;

    if (member.value) value = storedValue(evaluate(*member.value, name), type, member.value->token, label);
    else if (managedText(type)) value = label;
    else if (integral(type)) {
      // Integers count up by 1 and floating values by 0.5, both from 0 or
      // from the previous member.
      value = index ? successor(memberValue(name, index - 1)) : "0";

      if (!fitsInteger(value, type))
        fail(member.name, "implicit value " + value + " of '" + label + "' is out of range for " + typeName(type));
    } else {
      double real = index ? constantOf(memberValue(name, index - 1), type).real + 0.5 : 0;

      if (type == Type::Float) real = double(float(real));
      if (!std::isfinite(real)) fail(member.name, "implicit value of '" + label + "' is out of range");

      value = floatingConstantText(real);
    }

    // `enums` is not resized during evaluation, so the reference is stable.
    pending.info.values[index] = value;
    pending.state[index] = 2;
    return value;
  }

private:
  // The enum a dotted path names, seen from inside enum `from`: from its
  // enclosing static containers outward, then at file scope.
  std::string qualify(const std::string &path, const std::string &from) const {
    for (auto scope = from;;) {
      const auto dot = scope.rfind('.');
      scope = dot == std::string::npos ? "" : scope.substr(0, dot);
      const auto candidate = scope.empty() ? path : scope + "." + path;

      if (enums.count(candidate)) return candidate;
      if (scope.empty()) return "";
    }
  }
  EnumConstant member(const std::string &name, const Token &token) {
    const auto &info = enums.at(name).info;
    const auto index = info.find(token.text);

    if (index == info.members.size()) {
      std::vector<std::string> names;

      for (const auto &m : info.members) names.push_back(m.text);

      fail(token, "unknown member '" + token.text + "' in enum '" + name + "'" + didYouMean(token.text, names));
    }

    return constantOf(memberValue(name, index), info.type);
  }
  EnumConstant evaluate(const Expr &e, const std::string &from) {
    if (auto literal = literalConstant(e)) return *literal;

    if (e.kind == "name") {
      const auto &info = enums.at(from).info;

      if (info.find(e.token.text) == info.members.size())
        fail(e.token, "unknown name '" + e.token.text + "' in an enum value; it can use literals and members of '" +
                          from + "' or other enums");

      return member(from, e.token);
    }

    if (e.kind == ".") {
      std::string path;
      const Expr *root = e.args[0].get();

      while (root->kind == ".") {
        path = "." + root->token.text + path;
        root = root->args[0].get();
      }

      if (root->kind == "name") {
        const auto name = qualify(root->token.text + path, from);

        if (!name.empty()) return member(name, e.token);
      }

      fail(e.token, "'" + e.token.text + "' is not an enum member; an enum value can use literals and enum members");
    }

    if (e.kind == "unary") return unaryConstant(e.token, evaluate(*e.args[0], from));
    if (e.kind == "binary") return binaryConstant(e.token, evaluate(*e.args[0], from), evaluate(*e.args[1], from));

    fail(e.token, "an enum value must be a constant expression of literals and enum members");
  }
};
} // namespace
std::optional<size_t> enumLiteralMember(const EnumInfo &info, const Expr &e) {
  auto c = literalConstant(e);

  if (!c && e.kind == "unary" && e.token.text == "-")
    if ((c = literalConstant(*e.args[0]))) c = unaryConstant(e.token, *c);

  if (!c) return std::nullopt;

  const auto value = storedValue(*c, info.type, e.token, "");
  const auto found = std::find(info.values.begin(), info.values.end(), value);

  if (found == info.values.end())
    fail(e.token, "no member of enum '" + info.declaration.text + "' has this value");

  return size_t(found - info.values.begin());
}
std::map<std::string, EnumInfo> resolveEnums(const std::vector<EnumFragment> &fragments) {
  EnumResolver resolver;
  std::map<std::string, std::vector<const EnumFragment *>> groups;
  std::vector<std::string> order;

  for (const auto &fragment : fragments) {
    auto &group = groups[fragment.name.text];

    if (group.empty()) order.push_back(fragment.name.text);

    group.push_back(&fragment);
  }

  for (const auto &name : order) {
    const auto &group = groups.at(name);
    // The underlying type may appear on any fragment; all stated types agree.
    const EnumFragment *typed = nullptr;

    for (const auto *fragment : group) {
      if (fragment->type == Type::Void) continue;

      if (!typed) typed = fragment;
      else if (fragment->type != typed->type)
        fail(fragment->name, "enum '" + name + "' is declared as " + typeName(typed->type) +
                                 " in an earlier fragment, not " + typeName(fragment->type));
    }

    if (!typed)
      fail(group.front()->name, "enum '" + name + "' requires an underlying type, as in " + name +
                                    " : int #@{ ... }");

    const auto type = typed->type;

    if (!integral(type) && !floating(type) && !managedText(type))
      fail(typed->name, "enum underlying type must be an integer, float, double, string, or name; got " +
                            typeName(type));

    auto &pending = resolver.enums[name];
    pending.info = EnumInfo{group.front()->name, type, {}, {}};

    for (const auto *fragment : group) {
      pending.info.attribute = pending.info.attribute || fragment->attribute;

      for (const auto &member : fragment->members) {
        if (pending.info.find(member.name.text) != pending.info.members.size())
          fail(member.name, "duplicate member '" + member.name.text + "' in enum '" + name + "'");

        pending.info.members.push_back(member.name);
        pending.members.push_back(&member);
      }
    }

    pending.info.values.resize(pending.members.size());
    pending.state.assign(pending.members.size(), 0);
  }

  // Values in input order, so diagnostics come out in source order.
  for (const auto &name : order)
    for (size_t i = 0; i < resolver.enums.at(name).members.size(); ++i) resolver.memberValue(name, i);

  std::map<std::string, EnumInfo> enums;

  for (auto &[name, pending] : resolver.enums) enums.emplace(name, std::move(pending.info));

  return enums;
}
namespace {
// Conservative proof for a direct borrow: every use of the parameter must
// access its pointee. Unknown calls and address-taking can export that storage.
bool borrowsOnly(const Expr &e, const std::string &name, bool pointee = false,
                 bool mayEscape = false) {
  if (e.kind == "name" && e.token.text == name) return pointee && !mayEscape;

  const bool indirect = e.kind == "." || (e.kind == "unary" && e.token.text == "*");
  const bool nullCheck = e.kind == "binary" && (e.token.text == "==" || e.token.text == "!=") &&
                         (e.args[0]->kind == "null" || e.args[1]->kind == "null");

  // A lambda body is not inspected, so it may capture or keep anything; the
  // same goes for a macro value's argument bindings.
  if (e.kind == "lambda" || e.kind == "macroValue" || e.kind == "duckCall" || e.kind == "moveCheck") return false;

  mayEscape = mayEscape || (e.kind == "unary" && e.token.text == "&") ||
              (e.kind == "call" && e.token.text != "Print");

  if (e.receiver && !borrowsOnly(*e.receiver, name, false, true)) return false;

  // Used as a condition, the pointer itself is only compared with null.
  const bool tests = (e.kind == "unary" && e.token.text == "!") ||
                     (e.kind == "binary" && (e.token.text == "&&" || e.token.text == "||"));

  for (size_t i = 0; i < e.args.size(); ++i) {
    const bool selects = e.kind == "conditional" && i == 0;

    if (!borrowsOnly(*e.args[i], name, indirect || nullCheck || tests || selects, mayEscape)) return false;
  }

  return true;
}
} // namespace
bool borrowsOnly(const Stmt &s, const std::string &name) {
  // `if p`, `while p`, and their relatives only test the pointer.
  const bool nullTest = (s.kind == "if" || s.kind == "while" || s.kind == "do" || s.kind == "for") && s.expr &&
                        s.expr->kind == "name" && s.expr->token.text == name;

  if ((s.expr && !nullTest && !borrowsOnly(*s.expr, name)) || (s.target && !borrowsOnly(*s.target, name)) ||
      (s.initializer && !borrowsOnly(*s.initializer, name)) ||
      (s.step && !borrowsOnly(*s.step, name))) return false;

  for (const auto &child : s.body) if (!borrowsOnly(*child, name)) return false;
  for (const auto &child : s.alternate) if (!borrowsOnly(*child, name)) return false;

  return true;
}
// Orders declarations by the input order of their files, then by position
// within a file.
class InputOrder {
  std::map<std::string, size_t> files;

  void add(const std::string &source) { files.emplace(source, files.size()); }

public:
  explicit InputOrder(const Program &program) {
    for (const auto &source : program.sources) add(source);

    for (const auto &fragment : program.records) add(fragment.name.source);

    for (const auto &f : program) add(f.name.source);
  }

  bool operator()(const Token &a, const Token &b) const {
    const auto fa = files.at(a.source), fb = files.at(b.source);

    if (fa != fb) return fa < fb;

    return a.line != b.line ? a.line < b.line : a.column < b.column;
  }
};

static void mergeRecordFragments(Symbols &symbols, const Program &program) {
  for (const auto &fragment : program.records) {
    auto [found, first] = symbols.records.emplace(fragment.name.text, fragment);

    if (first) continue;

    auto &merged = found->second;
    auto list = [](const Record &record) {
      std::string text;

      for (const auto &parent : record.parents) text += (text.empty() ? "" : ", ") + parent.text;

      return text.empty() ? std::string("no parents") : "parents '" + text + "'";
    };

    if (list(fragment) != list(merged))
      fail(fragment.name, "container '" + fragment.name.text + "' is declared with " + list(merged) +
                              " at " + where(merged.name) + "; every declaration must repeat the same parent list");

    for (const auto &field : fragment.fields) {
      for (const auto &existing : merged.fields)
        if (existing.name.text == field.name.text)
          fail(field.name, "'" + field.name.text + "' is already declared in container '" +
                               fragment.name.text + "' at " + where(existing.name));

      merged.fields.push_back(field);
    }

    if (fragment.markupClosed) {
      merged.markupClosed = true;
      merged.markupChildren = fragment.markupChildren;
    }
  }
}
static void addMarkupFields(Symbols &symbols, const Program &program) {
  for (const auto &host : program.markupHosts) {
    const auto found = symbols.records.find(host.text);

    if (found == symbols.records.end()) continue;

    auto &record = found->second;
    record.markupClosed = false;

    if (hasField(record, "$children")) continue;

    Token name = host;
    name.text = "$children";
    record.fields.push_back({name, markupType()});
  }

  {
    std::map<std::string, Program::MarkupValue> implied;

    for (const auto &value : program.markupValues) {
      const auto found = symbols.records.find(value.tag.text);

      if (found == symbols.records.end()) continue;

      auto &record = found->second;

      if (const auto known = implied.find(value.tag.text); known != implied.end()) {
        if (known->second.type != value.type)
          fail(value.tag, "'" + value.tag.text + "' holds a " + typeName(known->second.type) + " value at " +
                              where(known->second.tag) + " and a " + typeName(value.type) +
                              " value here; declare its value field with one type");

        continue;
      }

      if (hasField(record, "value")) continue;

      Token name = value.tag;
      name.text = "value";
      record.fields.push_back({name, value.type});
      implied.emplace(value.tag.text, value);
    }
  }
}
static void validateNativeDeclaration(const Function &f, const std::map<std::string, EnumInfo> &enums) {
  // An enum crosses the boundary as its underlying value, so it needs one
  // that native code can take: a 32/64-bit integer, float, or double.
  const auto native = [&](const Type &t) {
    const auto found = enumValue(t) ? enums.find(t.record) : enums.end();
    return found != enums.end() ? nativeScalar(found->second.type) : nativeScalar(t);
  };

  if (f.name.text == "Main")
    fail(f.name, "Main cannot be a native declaration");

  if (!std::regex_match(f.nativeSymbol, std::regex("[A-Za-z_][A-Za-z0-9_]*")) ||
      f.nativeSymbol == "main" || f.nativeSymbol == "wmain" || f.nativeSymbol == "printf" ||
      f.nativeSymbol == "malloc" || f.nativeSymbol == "free" || f.nativeSymbol == "putchar")
    fail(f.name, "invalid or reserved native symbol '" + f.nativeSymbol + "'");

  if (f.result != Type::Void && !native(f.result))
    fail(f.name, "native return requires void, a pointer, float, double, a 32/64-bit integer, "
                 "or an enum of one of those");

  for (const auto &p : f.params) {
    if (p.type == Type::Bool ||
        (!p.reference && !textType(p.type) && !native(p.type)) ||
        // References follow the C layout of numeric values up to 64 bits,
        // pointers, and containers; other Duck types have no native ABI.
        (p.reference && !((numeric(p.type) && integerWidth(p.type) <= 64) ||
                          p.type.depth || recordValue(p.type))))
      fail(p.name, "unsupported native parameter ABI; use 32/64-bit values, enums of them, numeric references, "
                   "pointers, pointer references, or text by value");
  }
}
static void rejectMethodClash(const Symbols &symbols, const Function &f, const InputOrder &before) {
  const auto record = f.name.text.substr(0, f.name.text.rfind('.'));
  const auto member = memberName(f.name.text);

  for (const auto &field : symbols.records.at(record).fields)
    if (field.name.text == member) {
      // Report the clash at whichever declaration comes later in input order.
      const bool fieldLater = before(f.name, field.name);
      const auto &later = fieldLater ? field.name : f.name;
      const auto &earlier = fieldLater ? f.name : field.name;
      fail(later, "'" + member + "' is already declared in container '" + record + "' at " +
                      where(earlier));
    }

  auto existing = symbols.find(f.name.text);

  if (existing != symbols.end())
    fail(f.name, (member[0] != '$' ? "'" + member + "'"
                  : member == "$destroy" ? std::string("a destructor")
                  : member.rfind("$part", 0) == 0
                      ? std::string("a partial constructor with these parameter types")
                      : std::string("a constructor with these parameter types")) +
                     " is already declared in container '" + record + "' at " +
                     where(existing->second.declaration));
}
static void recordNonescapingParameters(Symbols &symbols, const Function &f) {
  auto &safe = symbols.at(f.name.text).nonescapingParameters;

  for (size_t i = 0; i < f.params.size(); ++i) {
    const auto &p = f.params[i];

    if (!p.type.depth || p.reference) continue;

    bool borrow = true;

    for (const auto &stmt : f.body) borrow = borrow && borrowsOnly(*stmt, p.name.text);
    if (borrow) safe.insert(i);
  }
}
static void declareStatics(Symbols &symbols, const Program &program) {
  for (const auto &field : program.statics) {
    const auto &name = field.name.text;
    // Instance fields and methods are a separate namespace from statics.
    bool clash = symbols.count(name) || symbols.records.count(name);

    for (const auto &[function, signature] : symbols) {
      (void)signature;
      clash = clash || function.rfind(name + ".", 0) == 0;
    }

    StaticInfo info{field.name, field.type, field.constant, field.protectedAccess, field.initializer.get()};
    info.order = symbols.statics.size();
    info.usings = field.usings;
    info.scope = field.scope;
    info.scopedType = field.scopedType;

    if (clash || !symbols.statics.emplace(name, info).second)
      fail(field.name, "duplicate static field or member '" + name + "'");

    const auto &t = field.type;

    // Array elements are checked once every container is known.
    if (field.scope.empty() && !(numeric(t) || t == Type::Bool || enumValue(t) || t == Type::Pointer || recordValue(t) ||
          managedValue(t) || arrayValue(t)))
      fail(field.name, "static fields of type " + typeName(t) +
                           " are not supported yet; use a number, bool, enum, string, name, big, "
                           "void*, container, or array");
  }
}
// vecN and friends are ordinary containers from here on, so copies, fields,
// references, arrays and heap storage need no special handling. A matrix's
// columns have no name: only indexing reaches them.
static void addNativeMathTypes(Symbols &symbols) {
  static const char *const stems[] = {"vec", "ivec", "dvec", "mat", "dmat"};
  static const char *const axes[] = {"X", "Y", "Z", "W"};
  static const char *const angles[] = {"Pitch", "Yaw", "Roll"};
  std::vector<std::string> names{"quat", "rot"};

  for (const std::string stem : stems)
    for (char size = '2'; size <= '4'; ++size) names.push_back(stem + size);

  for (const auto &name : names) {
    const auto shape = nativeMath(name);

    if (!shape) continue;

    Record record;
    record.name.text = name;

    for (unsigned i = 0; i < std::max(shape.columns, shape.matrix() ? 0u : shape.rows); ++i) {
      Parameter field;
      field.name.text = shape.matrix() ? "$" + std::to_string(i) : std::string(shape.euler ? angles[i] : axes[i]);
      field.type = shape.element();

      // An unassigned quat means no rotation, not the invalid all-zero value.
      if (shape.quaternion && i == 3) {
        field.initializer = std::make_shared<Expr>();
        field.initializer->kind = "real";
        field.initializer->token.text = "1.0";
        field.initialValue = "1.0";
      }

      record.fields.push_back(field);
    }

    symbols.records.emplace(name, record);
  }
}
// Whether a qualified name sits inside a namespace called Math.
static bool insideMath(const std::string &qualified) {
  size_t start = 0;

  for (auto dot = qualified.find('.'); dot != std::string::npos; start = dot + 1, dot = qualified.find('.', start))
    if (qualified.compare(start, dot - start, "Math") == 0) return true;

  return false;
}
// Math is the compiler's namespace, so `Math.X` always means the built-in.
static void rejectMathDeclarations(const Symbols &symbols, const Program &program) {
  const auto reject = [](const Token &at) {
    fail(at, "'Math' is a compiler namespace and cannot be declared; choose another container name");
  };

  for (const auto &f : program)
    if (insideMath(f.name.text)) reject(f.name);

  for (const auto &[name, record] : symbols.records)
    if (insideMath(name + ".")) reject(record.name);

  for (const auto &[name, info] : symbols.enums)
    if (insideMath(name + ".")) reject(info.declaration);

  for (const auto &[name, info] : symbols.statics)
    if (insideMath(name)) reject(info.declaration);
}
static void validateEntryPoint(const Symbols &symbols, bool requireEntry, const Program &program) {
  auto main = symbols.find("Main");

  if (requireEntry && main == symbols.end()) {
    std::vector<std::string> names;

    for (const auto &[name, signature] : symbols) {
      (void)signature;
      names.push_back(name);
    }

    const auto close = closestName("Main", names);
    fail({"", 1, 1, false, program.sources.empty() ? "" : program.sources.front()},
         "program requires Main(); " +
             (close.empty() ? "add Main() { ... }, or compile with --library" : "found '" + close + "', but names are case-sensitive and the entry point must be exactly Main"));
  }

  if (main != symbols.end()) {
    // The parser marks `string [] args` as a reference: see entryParameter.
    const auto &parameters = main->second.parameters;
    const bool arguments = parameters.size() == 1 && parameters[0].reference &&
                           dynamicArray(parameters[0].type) &&
                           *parameters[0].type.element == Type::String;

    if (main->second.result != Type::Void || !(parameters.empty() || arguments))
      fail(main->second.declaration, "bootstrap entry point must be Main() or "
                                     "Main(string [] args) with no return type");
  }
}
Symbols collectSymbols(const Program &program, bool requireEntry) {
  Symbols symbols;
  const InputOrder before(program);
  // Declarations of one container merge in input order, so every module and
  // file order sees the same field layout. Each must repeat the parent list.
  mergeRecordFragments(symbols, program);
  addNativeMathTypes(symbols);
  // Markup decides what its containers store: children under a tag, and a
  // `value` field typed by the literal written there.
  addMarkupFields(symbols, program);
  symbols.enums = resolveEnums(program.enums);

  for (const auto &[name, info] : symbols.enums)
    if (symbols.records.count(name))
      fail(info.declaration, "duplicate type name '" + name + "'");

  for (const auto &f : program) {
    if (!f.nativeSymbol.empty()) validateNativeDeclaration(f, symbols.enums);
    if (f.method) rejectMethodClash(symbols, f, before);

    if (f.name.text == "Print" || f.name.text == "Length" || f.name.text == "CodeUnitAt" || f.name.text == "Exec" ||
        !symbols.emplace(f.name.text, Signature{f.result, f.params, f.nativeSymbol, f.name, {}, f.method, f.protectedAccess, f.inferred, f.tags}).second)
      fail(f.name, "duplicate or reserved function '" + f.name.text + "'" +
                       (symbols.count(f.name.text) ? "; it is already declared at " + where(symbols.at(f.name.text).declaration) : ""));

    if (f.nativeSymbol.empty()) recordNonescapingParameters(symbols, f);
  }

  declareStatics(symbols, program);
  rejectMathDeclarations(symbols, program);

  // A type and a function or static field must not share a qualified name,
  // or `Name.Space()` and `Name.Space` would be ambiguous.
  for (const auto &[name, record] : symbols.records) {
    auto function = symbols.find(name);
    auto field = symbols.statics.find(name);

    if (function != symbols.end() || field != symbols.statics.end())
      fail(function != symbols.end() ? function->second.declaration : field->second.declaration,
           "'" + name + "' is already declared as a container at " + where(record.name));
  }

  validateEntryPoint(symbols, requireEntry, program);
  validateNativeSymbols(symbols);
  return symbols;
}
std::string sourceKey(const std::string &path) {
  namespace fs = std::filesystem;
  std::error_code error;
  const auto absolute = fs::absolute(fs::path(path), error);
  // Resolves ".." and, for files that exist, their on-disk spelling.
  const auto resolved = fs::weakly_canonical(absolute, error);
  return (error ? absolute.lexically_normal() : resolved).generic_u8string();
}
const std::string *Symbols::execTarget(const Expr &call) const {
  namespace fs = std::filesystem;

  if (call.args.size() != 1) fail(call.token, "Exec expects one source file path");

  const auto &argument = *call.args[0];

  if (argument.kind != "text" || argument.token.character)
    fail(argument.token, "Exec requires a string literal path; computed paths are not supported yet");

  const auto &written = argument.token.text;
  // Relative to the file making the call, so moving the project keeps working.
  const auto path = fs::path(call.token.source).parent_path() / fs::u8path(written);
  const auto key = sourceKey(path.string());

  if (importedSources.count(key))
    fail(argument.token, "Exec cannot run '" + written + "': it belongs to another module, and "
                         "Exec reaches only the calling module's source files");

  if (!sources.count(key))
    fail(argument.token, "Exec cannot find '" + written + "' (" + key + ") among this module's source "
                         "files; paths are relative to the calling file, and the file must be compiled with it");

  const auto script = scripts.find(key);
  return script == scripts.end() ? nullptr : &script->second;
}
bool Symbols::findField(const std::string &record, const std::string &name,
                        MemberPath &path) const {
  const auto &fields = records.at(record).fields;

  for (size_t i = 0; i < fields.size(); ++i)
    if (fields[i].name.text == name) {
      path = {{i}, record, &fields[i]};
      return true;
    }

  for (size_t i = 0; i < fields.size(); ++i)
    if (fields[i].parentView && findField(fields[i].type.record, name, path)) {
      path.indices.insert(path.indices.begin(), i);
      return true;
    }

  return false;
}
Symbols::const_iterator Symbols::findMethod(const std::string &record, const std::string &name,
                                            MemberPath &path) const {
  auto found = find(methodKey(record, name));

  if (found != end()) {
    path = {{}, record, nullptr};
    return found;
  }

  const auto &fields = records.at(record).fields;

  for (size_t i = 0; i < fields.size(); ++i) {
    if (!fields[i].parentView) continue;

    auto inherited = findMethod(fields[i].type.record, name, path);

    if (inherited != end()) {
      path.indices.insert(path.indices.begin(), i);
      return inherited;
    }
  }

  return end();
}
bool Symbols::managedRecord(const std::string &record) const {
  for (const auto &field : records.at(record).fields)
    if (managed(field.type) || field.type.role == Type::Owner) return true;

  return false;
}
bool Symbols::canOwn(const std::string &holder, const std::string &held) const {
  std::set<std::string> seen;
  std::function<bool(const std::string &)> visit = [&](const std::string &current) {
    if (current == held) return true;
    if (!seen.insert(current).second) return false;

    for (const auto &field : records.at(current).fields) {
      Type inner = field.type;

      while (arrayValue(inner)) inner = *inner.element;
      if (inner.role == Type::Owner) inner = inner.pointee();
      if (recordValue(inner) && visit(inner.record)) return true;
    }

    return false;
  };
  return visit(holder);
}
std::vector<std::string> Symbols::constructors(const std::string &record, bool partial) const {
  std::vector<std::string> keys;
  const auto prefix = methodKey(record, partial ? "$part" : "$new");

  for (auto it = lower_bound(prefix); it != end() && it->first.compare(0, prefix.size(), prefix) == 0; ++it)
    keys.push_back(it->first);

  return keys;
}
namespace {
void collectParts(const Symbols &symbols, const std::string &record, const std::vector<size_t> &path,
                  std::vector<ConstructionPart> &parts) {
  const auto &fields = symbols.records.at(record).fields;

  for (size_t i = 0; i < fields.size(); ++i)
    if (fields[i].parentView) {
      auto nested = path;
      nested.push_back(i);
      collectParts(symbols, fields[i].type.record, nested, parts);
    }

  for (bool partial : {true, false}) {
    const auto overloads = symbols.constructors(record, partial);

    if (overloads.empty()) continue;

    const auto automatic = constructorKey(record, {}, partial);
    parts.push_back({path, record, partial, overloads, symbols.count(automatic) ? automatic : ""});
  }
}
} // namespace
std::vector<ConstructionPart> Symbols::parts(const std::string &record) const {
  std::vector<ConstructionPart> result;
  collectParts(*this, record, {}, result);
  return result;
}
bool Symbols::zeroDefault(const Type &type) const {
  if (fixedArray(type)) return zeroDefault(*type.element);

  if (!recordValue(type))
    return !type.depth && (numeric(type) || type == Type::Bool || type == Type::Char || enumValue(type) ||
                           decimalValue(type) || calendarValue(type));

  if (!constructors(type.record).empty() || !constructors(type.record, true).empty() || destructible(type.record))
    return false;

  for (const auto &field : records.at(type.record).fields)
    if (field.initializer || !zeroDefault(field.type)) return false;

  return true;
}
bool Symbols::destructible(const std::string &record) const {
  if (count(destructorKey(record))) return true;

  for (const auto &field : records.at(record).fields)
    if (field.type.role == Type::Owner || collectionValue(field.type) || fileValue(field.type) ||
        dynamicArray(field.type) ||
        (fixedArray(field.type) && needsCleanup(field.type)) ||
        (recordValue(field.type) && destructible(field.type.record)))
      return true;

  return false;
}
// Constructors run during construction, so a container with any, in itself,
// a nested container, or a parent, is built at run time.
bool Symbols::runtimeDefaults(const std::string &record) const {
  if (!constructors(record).empty() || !constructors(record, true).empty()) return true;

  for (const auto &field : records.at(record).fields)
    if (field.runtimeDefault ||
        (recordValue(field.type) && !field.initializer && runtimeDefaults(field.type.record)))
      return true;

  return false;
}
bool Symbols::derivesFrom(const std::string &record, const std::string &ancestor) const {
  auto found = records.find(record);

  if (found == records.end()) return false;

  for (const auto &parent : found->second.parents)
    if (parent.text == ancestor || derivesFrom(parent.text, ancestor)) return true;

  return false;
}
size_t Symbols::parentPath(const std::string &record, const std::string &base,
                           std::vector<size_t> &path) const {
  size_t count = 0;
  const auto &fields = records.at(record).fields;

  for (size_t i = 0; i < fields.size() && fields[i].parentView; ++i) {
    std::vector<size_t> below;
    size_t found = fields[i].type.record == base ? 1 : 0;
    found += parentPath(fields[i].type.record, base, below);

    if (found && !count) {
      path = {i};

      if (fields[i].type.record != base) path.insert(path.end(), below.begin(), below.end());
    }

    count += found;
  }

  return count;
}
std::string Symbols::recordAt(std::string record, const std::vector<size_t> &path) const {
  for (auto index : path) record = records.at(record).fields[index].type.record;

  return record;
}
std::vector<std::string> Symbols::dispatchNames(const std::string &record) const {
  std::set<std::string> names;
  std::function<void(const std::string &)> collect = [&](const std::string &current) {
    const auto prefix = methodKey(current, "");

    for (auto it = lower_bound(prefix); it != end() && it->first.compare(0, prefix.size(), prefix) == 0; ++it)
      if (dispatched(it->first, it->second.method)) names.insert(memberName(it->first));

    for (const auto &field : records.at(current).fields)
      if (field.parentView) collect(field.type.record);
  };
  collect(record);
  return {names.begin(), names.end()};
}
DispatchTable Symbols::overrider(const std::string &record, const std::vector<size_t> &path,
                                 const std::string &name) const {
  auto current = record;

  for (size_t length = 0;; ++length) {
    const auto key = methodKey(current, name);

    if (auto found = find(key); found != end() && found->second.method)
      return {key, {path.begin(), path.begin() + length}};

    if (length == path.size()) break;

    current = records.at(current).fields[path[length]].type.record;
  }

  MemberPath member;
  const auto found = findMethod(current, name, member);
  auto full = path;
  full.insert(full.end(), member.indices.begin(), member.indices.end());
  return {found->first, full};
}
namespace {
void collectVisible(const Symbols &symbols, const std::string &record, std::vector<size_t> prefix,
                    std::set<std::string> &seen, std::vector<MemberPath> &result) {
  const auto &fields = symbols.records.at(record).fields;

  for (size_t i = 0; i < fields.size(); ++i)
    if (!fields[i].parentView && seen.insert(fields[i].name.text).second) {
      auto indices = prefix;
      indices.push_back(i);
      result.push_back({indices, record, &fields[i]});
    }

  for (size_t i = 0; i < fields.size(); ++i)
    if (fields[i].parentView) {
      auto indices = prefix;
      indices.push_back(i);
      collectVisible(symbols, fields[i].type.record, indices, seen, result);
    }
}
void checkParents(const Symbols &symbols, const std::string &name,
                  std::set<std::string> &active, std::set<std::string> &complete) {
  if (complete.count(name)) return;

  active.insert(name);

  for (const auto &parent : symbols.records.at(name).parents) {
    if (!symbols.records.count(parent.text))
      fail(parent, "unknown parent container '" + parent.text + "'");

    if (active.count(parent.text))
      fail(parent, "inheritance cycle through '" + parent.text + "'");

    checkParents(symbols, parent.text, active, complete);
  }

  active.erase(name);
  complete.insert(name);
}
} // namespace
std::vector<TypeMember> Symbols::members(const std::string &record) const {
  std::vector<TypeMember> result;

  for (const auto &path : visibleFields(record)) {
    TypeMember member;
    member.name = path.field->name.text;
    member.declaring = path.declaring;
    member.isProtected = path.field->protectedAccess;
    member.path = path.indices;
    member.tags = path.field->tags;
    member.type = path.field->type;
    result.push_back(std::move(member));
  }

  for (const auto &name : dispatchNames(record)) {
    const auto [key, receiver] = overrider(record, {}, name);
    const auto &signature = at(key);
    TypeMember member;
    member.name = name;
    member.key = key;
    member.declaring = key.substr(0, key.rfind('.'));
    member.isProtected = signature.protectedAccess;
    member.path = receiver;
    member.tags = signature.tags;
    member.signature = &signature;
    result.push_back(std::move(member));
  }

  return result;
}
std::vector<MemberPath> Symbols::visibleFields(const std::string &record) const {
  std::vector<MemberPath> result;
  std::set<std::string> seen;
  collectVisible(*this, record, {}, seen, result);
  return result;
}
void resolveInheritance(Symbols &symbols) {
  std::set<std::string> active, complete;

  for (const auto &[name, record] : symbols.records) {
    std::set<std::string> listed;

    for (const auto &parent : record.parents)
      if (!listed.insert(parent.text).second)
        fail(parent, "duplicate parent '" + parent.text + "'");

    checkParents(symbols, name, active, complete);
  }

  // Each parent's state leads the layout, in priority order, so a pointer
  // to the derived value reaches its first parent without adjustment.
  for (auto &[name, record] : symbols.records) {
    if (record.parents.empty() || (!record.fields.empty() && record.fields.front().parentView))
      continue;

    std::vector<Parameter> views;

    for (const auto &parent : record.parents) {
      Type type = Type::Record;
      type.record = parent.text;
      views.push_back({parent, type, false, true, true});
    }

    record.fields.insert(record.fields.begin(), views.begin(), views.end());
  }

  // A field may not redeclare an inherited member, and a method may only
  // redeclare inherited methods, overriding each of them; it must keep their
  // parameter types, references, result, and access.
  auto requireNew = [&](const Record &record, const std::string &member, const Token &token, bool field) {
    for (const auto &parent : record.parents) {
      if (parent.text == member)
        fail(token, "member '" + member + "' has the name of parent container '" + member + "'");

      MemberPath path;

      if (symbols.findField(parent.text, member, path))
        fail(token, "'" + member + "' is already inherited from '" + path.declaring +
                        "' as a field; fields cannot be overridden");

      if (field && symbols.findMethod(parent.text, member, path) != symbols.end())
        fail(token, "'" + member + "' is already inherited from '" + path.declaring +
                        "' as a method; only a method can override it");

      if (field && (symbols.findMethod(parent.text, "get-" + member, path) != symbols.end() ||
                    symbols.findMethod(parent.text, "set-" + member, path) != symbols.end()))
        fail(token, "'" + member + "' is already inherited from '" + path.declaring +
                        "' as a property; only a property can override it");
    }
  };

  for (const auto &[key, signature] : symbols)
    if (dispatched(key, signature.method)) {
      const auto &owner = symbols.records.at(key.substr(0, key.rfind('.')));
      const auto property = propertyName(key);
      requireNew(owner, property.empty() ? memberName(key) : property, signature.declaration, false);
    }

  for (const auto &[name, record] : symbols.records)
    for (const auto &field : record.fields)
      if (!field.parentView) requireNew(record, field.name.text, field.name, true);
}
void resolveAttributes(Symbols &symbols) {
  auto resolve = [&](Tag &tag) {
    const auto &text = tag.token.text;
    const auto dot = text.rfind('.');

    if (dot != std::string::npos) {
      const auto owner = symbols.enums.find(text.substr(0, dot));

      if (owner == symbols.enums.end()) fail(tag.token, "unknown enum in attribute '" + text + "'");

      if (!owner->second.attribute)
        fail(tag.token, "enum '" + owner->first + "' is not marked [args], so it cannot be used as an attribute");

      tag.type = owner->first;
      tag.member = owner->second.find(text.substr(dot + 1));

      if (tag.member == owner->second.members.size()) fail(tag.token, "unknown attribute '" + text + "'");

      return;
    }

    bool unmarked = false;

    for (const auto &[name, info] : symbols.enums) {
      const auto member = info.find(text);

      if (member == info.members.size()) continue;

      if (!info.attribute) {
        unmarked = true;
        continue;
      }

      if (!tag.type.empty())
        fail(tag.token, "attribute '" + text + "' is ambiguous between enums '" + tag.type + "' and '" + name +
                            "'; qualify it, as in [" + name + "." + text + "]");

      tag.type = name;
      tag.member = member;
    }

    if (tag.type.empty())
      fail(tag.token, unmarked ? "'" + text + "' belongs to an enum not marked [args]; write [args] before that enum"
                               : "unknown attribute '" + text + "'; attributes are members of enums marked [args]");
  };

  for (auto &[name, record] : symbols.records)
    for (auto &field : record.fields)
      for (auto &tag : field.tags) resolve(tag);

  for (auto &[name, signature] : symbols)
    for (auto &tag : signature.tags) resolve(tag);
}
void validateOverrides(const Symbols &symbols) {
  auto describe = [](const std::string &owner, const std::string &key, const Signature &signature) {
    const auto property = propertyName(key);
    std::string text = (signature.protectedAccess ? "-" : "") + owner;

    if (!property.empty()) {
      const bool getter = memberName(key)[0] == 'g';
      const auto type = getter ? signature.result : signature.parameters.back().type;
      return text + property + " (" + (getter ? "getter" : "setter") + " of " + typeName(type) + ")";
    }

    text += memberName(key) + "(";

    for (size_t i = 1; i < signature.parameters.size(); ++i)
      text += (i > 1 ? ", " : "") + typeName(signature.parameters[i].type) +
              (signature.parameters[i].reference ? "&" : "");

    return text + ")" + (signature.result == Type::Void ? "" : " -> " + typeName(signature.result));
  };
  auto same = [](const Signature &a, const Signature &b) {
    if (a.result != b.result || a.lends != b.lends || a.protectedAccess != b.protectedAccess ||
        a.parameters.size() != b.parameters.size())
      return false;

    for (size_t i = 1; i < a.parameters.size(); ++i)
      if (a.parameters[i].type != b.parameters[i].type || a.parameters[i].reference != b.parameters[i].reference)
        return false;

    return true;
  };

  for (const auto &[key, signature] : symbols) {
    // Constructors and destructors belong to their own container only.
    if (!dispatched(key, signature.method)) continue;

    const auto &owner = symbols.records.at(key.substr(0, key.rfind('.')));
    const auto member = memberName(key);
    std::set<std::string> visited;
    std::function<void(const std::string &)> check = [&](const std::string &ancestor) {
      if (!visited.insert(ancestor).second) return;

      const auto inherited = symbols.find(methodKey(ancestor, member));

      if (inherited != symbols.end() && inherited->second.method && !same(signature, inherited->second))
        fail(signature.declaration, (propertyName(key).empty() ? "'" + member + "'" : "property '" + propertyName(key) + "'") +
                                        " overrides " +
                                        describe(ancestor + ".", inherited->first, inherited->second) +
                                        " but is declared as " + describe("", key, signature) +
                                        "; an override must keep the " +
                                        (propertyName(key).empty() ? "parameter types, result, and access"
                                                                   : "property type and access"));

      for (const auto &parent : symbols.records.at(ancestor).parents) check(parent.text);
    };

    for (const auto &parent : owner.parents) check(parent.text);
  }
}
namespace {
void validateLayout(const Symbols &symbols, const Record &record,
                    std::set<std::string> &active, std::set<std::string> &complete) {
  if (complete.count(record.name.text)) return;

  active.insert(record.name.text);

  for (const auto &field : record.fields) {
    symbols.validateType(field.type, field.name);
    // A fixed array stores its elements inline, like a nested container.
    const auto inline_ = fixedArray(field.type) ? *field.type.element : field.type;

    if (recordValue(inline_)) {
      if (active.count(inline_.record)) fail(field.name, "recursive container field requires a pointer");

      validateLayout(symbols, symbols.records.at(inline_.record), active, complete);
    }
  }

  active.erase(record.name.text);
  complete.insert(record.name.text);
}
} // namespace
void validateLayouts(const Symbols &symbols) {
  std::set<std::string> active, complete;

  for (const auto &[name, record] : symbols.records)
    validateLayout(symbols, record, active, complete);

  // Managed storage has a compiler-private layout and reference counts that
  // native code would bypass, so it never crosses a native boundary.
  for (const auto &[name, signature] : symbols) {
    (void)name;

    if (signature.nativeSymbol.empty()) continue;

    for (const auto &p : signature.parameters)
      if (recordValue(p.type) && symbols.managedRecord(p.type.record))
        fail(p.name, "native parameters cannot reference a container with string, name, or big fields");
  }
}
void validateNativeSymbols(const Symbols &symbols) {
  std::map<std::string, const Signature *> native;

  for (const auto &[name, signature] : symbols) {
    (void)name;

    if (signature.nativeSymbol.empty()) continue;

    auto [found, inserted] = native.emplace(signature.nativeSymbol, &signature);

    if (inserted) continue;

    const auto &other = *found->second;
    bool same = signature.result == other.result &&
                signature.parameters.size() == other.parameters.size();

    for (size_t i = 0; same && i < signature.parameters.size(); ++i)
      same = signature.parameters[i].type == other.parameters[i].type &&
             signature.parameters[i].reference == other.parameters[i].reference;

    if (!same)
      fail(signature.declaration, "conflicting native declarations for '" + signature.nativeSymbol + "'");
  }
}
void validateLifetimes(const Symbols &symbols) {
  // An owning field built with its container runs that construction again for
  // every object it creates, so such fields cannot lead back to the container.
  std::set<std::string> building, built;
  std::function<void(const std::string &)> visit = [&](const std::string &name) {
    if (built.count(name)) return;

    building.insert(name);

    for (const auto &field : symbols.records.at(name).fields) {
      Type next = field.type;

      while (fixedArray(next)) next = *next.element;

      const bool eager = field.type.role == Type::Owner && field.initializer && field.initializer->kind == "call" &&
                         recordValue(field.initializer->castType);

      if (eager) next = field.initializer->castType;
      if (!recordValue(next) || !symbols.records.count(next.record)) continue;

      if (eager && building.count(next.record))
        fail(field.name, "owning field '" + field.name.text + "' would build '" + next.record +
                             "' without end; declare it empty, as in " + next.record + "* " + field.name.text +
                             " :: null");

      visit(next.record);
    }

    building.erase(name);
    built.insert(name);
  };

  for (const auto &[name, record] : symbols.records) {
    (void)record;
    visit(name);
  }

  for (const auto &[name, record] : symbols.records)
    for (const auto &field : record.fields) {
      // A field without a default is built from the zero-argument overload
      // of every part. Parents are parts of the container's own construction.
      if (!recordValue(field.type) || field.initializer || field.parentView) continue;

      for (const auto &part : symbols.parts(field.type.record))
        if (part.automatic.empty())
          fail(field.name, "field '" + field.name.text + "' needs a default: '" + field.type.record +
                               "' cannot be constructed without arguments; " + part.text() +
                               " has no zero-argument overload");
    }

  for (const auto &[name, signature] : symbols) {
    (void)name;

    for (const auto &parameter : signature.parameters)
      if (!parameter.reference && collectionValue(parameter.type))
        fail(parameter.name, "parameter '" + parameter.name.text + "' must take " + typeName(parameter.type) +
                                 " by reference (&): collections are move-only");
      else if (!parameter.reference && fileValue(parameter.type))
        fail(parameter.name, "parameter '" + parameter.name.text + "' must take its file by reference (file&): "
                                 "a file is move-only");
      else if (!parameter.reference && arrayValue(parameter.type) && symbols.moveOnly(parameter.type))
        fail(parameter.name, "parameter '" + parameter.name.text + "' must take " + typeName(parameter.type) +
                                 " by reference (&): " +
                                 (dynamicArray(parameter.type) ? "dynamic arrays are move-only"
                                                               : "its elements cannot be copied implicitly"));

    for (const auto &parameter : signature.parameters)
      if (!parameter.reference && recordValue(parameter.type) &&
          symbols.records.count(parameter.type.record) && symbols.destructible(parameter.type.record))
        fail(parameter.name, "parameter '" + parameter.name.text + "' must take '" + parameter.type.record +
                                 "' by reference (&): it has a destructor, so it cannot be copied");
  }

  for (const auto &[name, info] : symbols.statics)
    if (recordValue(info.type) && !info.initializer)
      for (const auto &part : symbols.parts(info.type.record))
        if (part.automatic.empty())
          fail(info.declaration, "static field '" + name + "' needs an initializer: '" + info.type.record +
                                     "' cannot be constructed without arguments; " + part.text() +
                                     " has no zero-argument overload");

  // A fixed array field starts zeroed inside its container's constant
  // default, so its elements must be valid as zero bits.
  // A null func or alias is valid as zero bits, and its container releases it.
  auto holdsFunc = [](Type type) {
    while (fixedArray(type)) type = *type.element;

    return funcValue(type) || trackedAlias(type);
  };

  for (const auto &[name, record] : symbols.records)
    for (const auto &field : record.fields)
      if (fixedArray(field.type) && !holdsFunc(field.type) && !symbols.zeroDefault(*field.type.element))
        fail(field.name, "field '" + field.name.text + "' cannot hold " + typeName(field.type) +
                             " yet: its elements need defaults or cleanup; use a dynamic array (" +
                             typeName(*field.type.element) + " [])");
      else if (dynamicArray(field.type) && arrayValue(*field.type.element))
        fail(field.name, "field '" + field.name.text + "' cannot hold " + typeName(field.type) +
                             " yet: a container field holds nested arrays only with fixed, plain "
                             "dimensions, as in int [3][4]");

  // A static array starts as zero bits and is never destroyed, so its
  // elements must be valid as zero bits and own nothing to release.
  for (const auto &[name, info] : symbols.statics) {
    if (!arrayValue(info.type) || !info.scope.empty()) continue;

    auto element = *info.type.element;

    while (fixedArray(element)) element = *element.element;

    const bool plain = element == Type::Pointer || (!arrayValue(element) && symbols.zeroDefault(element));

    if (dynamicArray(info.type) && plain && !arrayValue(*info.type.element) && !symbols.needsCleanup(element)) continue;
    if (fixedArray(info.type) && plain) continue;

    fail(info.declaration, "static field '" + name + "' cannot hold " + typeName(info.type) +
                               " yet: a static array's elements must start as zero and own nothing, since static "
                               "storage is never destroyed; use numbers, bool, char, enums, void*, or plain "
                               "containers");
  }

  for (const auto &[name, info] : symbols.statics)
    if (collectionValue(info.type))
      fail(info.declaration, "static field '" + name + "' cannot hold a collection yet: static storage is "
                                 "never destroyed, so its elements would never be released");

  for (const auto &[name, info] : symbols.statics)
    if (recordValue(info.type) && symbols.destructible(info.type.record))
      fail(info.declaration, "static field '" + name + "' cannot hold '" + info.type.record +
                                 "': it has a destructor, and static storage is never destroyed");
}
} // namespace duck

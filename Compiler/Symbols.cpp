#include <functional>
#include "Pipeline.h"
#include "WideInteger.h"
#include <cmath>
#include <cstring>
#include <iomanip>
#include <locale>
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
// An explicit value is a literal of the enum's kind, optionally negated.
std::string integerValue(const Expr &e, const Type &type, const std::string &member) {
  const bool negative = e.kind == "unary" && e.token.text == "-";
  const auto &literal = negative ? *e.args[0] : e;
  if (literal.kind != "int")
    fail(e.token, "enum member '" + member + "' requires an integer literal value");
  const auto &text = literal.token.text;
  const int base = integerLiteralBase(text);
  const auto digits = base == 10 ? text : text.substr(2);
  if (digits.empty()) fail(e.token, "integer literal requires digits after its prefix");
  for (char c : digits) {
    const int digit = c >= '0' && c <= '9' ? c - '0'
                    : c >= 'a' && c <= 'f' ? c - 'a' + 10
                    : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    if (digit < 0 || digit >= base) fail(e.token, "invalid integer literal");
  }
  const auto decimal = Magnitude::parse(digits, base).decimal();
  const auto value = negative && decimal != "0" ? "-" + decimal : decimal;
  if (!fitsInteger(value, type))
    fail(e.token, "enum value out of range for " + typeName(type));
  return value;
}
double floatingValue(const Expr &e, const Type &type, const std::string &member) {
  const bool negative = e.kind == "unary" && e.token.text == "-";
  const auto &literal = negative ? *e.args[0] : e;
  if ((literal.kind != "real" && literal.kind != "int") ||
      integerLiteralBase(literal.token.text) != 10)
    fail(e.token, "enum member '" + member + "' requires a decimal number literal value");
  std::istringstream input(literal.token.text);
  input.imbue(std::locale::classic());
  double value = 0;
  if (type == Type::Float) {
    float single = 0;
    input >> single;
    value = single;
  } else
    input >> value;
  if (!input || input.peek() != std::char_traits<char>::eof() || !std::isfinite(value))
    fail(e.token, "invalid or out-of-range floating literal");
  return negative ? -value : value;
}
std::string floatingConstant(double value) {
  uint64_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  std::ostringstream encoded;
  encoded << "0x" << std::hex << std::uppercase << std::setfill('0') << std::setw(16) << bits;
  return encoded.str();
}
} // namespace
std::map<std::string, EnumInfo> resolveEnums(const std::vector<EnumFragment> &fragments) {
  std::map<std::string, EnumInfo> enums;
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
    EnumInfo info{group.front()->name, type, {}, {}};
    // Integers count up by 1 and floating values by 0.5, both from 0 or from
    // the previous member. Text defaults to the member name.
    std::string integer = "-1";
    double real = -0.5;
    for (const auto *fragment : group)
      for (const auto &member : fragment->members) {
        const auto &label = member.name.text;
        if (info.find(label) != info.members.size())
          fail(member.name, "duplicate member '" + label + "' in enum '" + name + "'");
        std::string value;
        if (integral(type)) {
          if (member.value) integer = integerValue(*member.value, type, label);
          else {
            integer = successor(integer);
            if (!fitsInteger(integer, type))
              fail(member.name, "implicit value " + integer + " of '" + label +
                                    "' is out of range for " + typeName(type));
          }
          value = integer;
        } else if (floating(type)) {
          if (member.value) real = floatingValue(*member.value, type, label);
          else {
            real += 0.5;
            if (type == Type::Float) real = double(float(real));
            if (!std::isfinite(real))
              fail(member.name, "implicit value of '" + label + "' is out of range");
          }
          value = floatingConstant(real);
        } else if (member.value) {
          if (member.value->kind != "text" || member.value->token.character)
            fail(member.value->token, "enum member '" + label + "' requires a string literal value");
          value = member.value->token.text;
        } else
          value = label;
        info.members.push_back(member.name);
        info.values.push_back(value);
      }
    enums.emplace(name, std::move(info));
  }
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
  mayEscape = mayEscape || (e.kind == "unary" && e.token.text == "&") ||
              (e.kind == "call" && e.token.text != "Print");
  if (e.receiver && !borrowsOnly(*e.receiver, name, false, true)) return false;
  for (const auto &child : e.args)
    if (!borrowsOnly(*child, name, indirect || nullCheck, mayEscape)) return false;
  return true;
}
} // namespace
bool borrowsOnly(const Stmt &s, const std::string &name) {
  if ((s.expr && !borrowsOnly(*s.expr, name)) || (s.target && !borrowsOnly(*s.target, name)) ||
      (s.initializer && !borrowsOnly(*s.initializer, name)) ||
      (s.step && !borrowsOnly(*s.step, name))) return false;
  for (const auto &child : s.body) if (!borrowsOnly(*child, name)) return false;
  for (const auto &child : s.alternate) if (!borrowsOnly(*child, name)) return false;
  return true;
}
Symbols collectSymbols(const Program &program, bool requireEntry) {
  Symbols symbols;
  // Input order of source files, then position within a file.
  std::map<std::string, size_t> files;
  auto seen = [&](const Token &t) { files.emplace(t.source, files.size()); };
  for (const auto &source : program.sources) files.emplace(source, files.size());
  for (const auto &fragment : program.records) seen(fragment.name);
  for (const auto &f : program) seen(f.name);
  auto before = [&](const Token &a, const Token &b) {
    const auto fa = files.at(a.source), fb = files.at(b.source);
    if (fa != fb) return fa < fb;
    return a.line != b.line ? a.line < b.line : a.column < b.column;
  };
  // Declarations of one container merge in input order, so every module and
  // file order sees the same field layout. Each must repeat the parent list.
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
  }
  symbols.enums = resolveEnums(program.enums);
  for (const auto &[name, info] : symbols.enums)
    if (symbols.records.count(name))
      fail(info.declaration, "duplicate type name '" + name + "'");
  for (const auto &f : program) {
    if (!f.nativeSymbol.empty()) {
      if (f.name.text == "Main")
        fail(f.name, "Main cannot be a native declaration");
      if (!std::regex_match(f.nativeSymbol, std::regex("[A-Za-z_][A-Za-z0-9_]*")) ||
          f.nativeSymbol == "main" || f.nativeSymbol == "printf" ||
          f.nativeSymbol == "malloc" || f.nativeSymbol == "free" || f.nativeSymbol == "putchar")
        fail(f.name, "invalid or reserved native symbol '" + f.nativeSymbol + "'");
      if (f.result != Type::Void && !nativeScalar(f.result))
        fail(f.name, "native return requires void, a pointer, float, double, or a 32/64-bit integer");
      for (const auto &p : f.params) {
        if (p.type == Type::Bool ||
            (!p.reference && !textType(p.type) && !nativeScalar(p.type)) ||
            // References follow the C layout of numeric values up to 64 bits,
            // pointers, and containers; other Duck types have no native ABI.
            (p.reference && !((numeric(p.type) && integerWidth(p.type) <= 64) ||
                              p.type.depth || recordValue(p.type))))
          fail(p.name, "unsupported native parameter ABI; use 32/64-bit values, numeric references, pointers, pointer references, or text by value");
      }
    }
    if (f.method) {
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
    if (f.name.text == "Print" || f.name.text == "Length" || f.name.text == "CodeUnitAt" ||
        !symbols.emplace(f.name.text, Signature{f.result, f.params, f.nativeSymbol, f.name, {}, f.method, f.protectedAccess, f.inferred}).second)
      fail(f.name, "duplicate or reserved function '" + f.name.text + "'");
    if (f.nativeSymbol.empty()) {
      auto &safe = symbols.at(f.name.text).nonescapingParameters;
      for (size_t i = 0; i < f.params.size(); ++i) {
        const auto &p = f.params[i];
        if (!p.type.depth || p.reference) continue;
        bool borrow = true;
        for (const auto &stmt : f.body) borrow = borrow && borrowsOnly(*stmt, p.name.text);
        if (borrow) safe.insert(i);
      }
    }
  }
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
    if (clash || !symbols.statics.emplace(name, info).second)
      fail(field.name, "duplicate static field or member '" + name + "'");
    const auto &t = field.type;
    if (!(numeric(t) || t == Type::Bool || enumValue(t) || t == Type::Pointer || recordValue(t) ||
          managedValue(t)))
      fail(field.name, "static fields of type " + typeName(t) +
                           " are not supported yet; use a number, bool, enum, string, name, big, "
                           "void*, or container");
  }
  // A type and a function or static field must not share a qualified name,
  // or `Name.Space()` and `Name.Space` would be ambiguous.
  for (const auto &[name, record] : symbols.records) {
    auto function = symbols.find(name);
    auto field = symbols.statics.find(name);
    if (function != symbols.end() || field != symbols.statics.end())
      fail(function != symbols.end() ? function->second.declaration : field->second.declaration,
           "'" + name + "' is already declared as a container at " + where(record.name));
  }
  auto main = symbols.find("Main");
  if (requireEntry && main == symbols.end())
    fail({"", 1, 1}, "program requires Main()");
  if (main != symbols.end() &&
      (main->second.result != Type::Void || !main->second.parameters.empty())) {
    for (const auto &f : program)
      if (f.name.text == "Main")
        fail(f.name, "bootstrap entry point must be Main() with no return type "
                     "or parameters");
  }
  validateNativeSymbols(symbols);
  return symbols;
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
    if (managed(field.type)) return true;
  return false;
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
    if (collectionValue(field.type) || dynamicArray(field.type) ||
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
    if (a.result != b.result || a.protectedAccess != b.protectedAccess ||
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
  for (const auto &[name, record] : symbols.records)
    for (const auto &field : record.fields)
      if (fixedArray(field.type) && !symbols.zeroDefault(*field.type.element))
        fail(field.name, "field '" + field.name.text + "' cannot hold " + typeName(field.type) +
                             " yet: its elements need defaults or cleanup; use a dynamic array (" +
                             typeName(*field.type.element) + " [])");
      else if (dynamicArray(field.type) && arrayValue(*field.type.element))
        fail(field.name, "field '" + field.name.text + "' cannot hold " + typeName(field.type) +
                             " yet: a container field holds nested arrays only with fixed, plain "
                             "dimensions, as in int [3][4]");
  for (const auto &[name, info] : symbols.statics)
    if (arrayValue(info.type))
      fail(info.declaration, "static field '" + name + "' cannot hold an array yet");
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

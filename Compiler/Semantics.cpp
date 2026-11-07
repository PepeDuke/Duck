#include "Pipeline.h"
#include "WideInteger.h"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <limits>
#include <locale>
#include <algorithm>
#include <functional>
#include <optional>
#include <sstream>

namespace duck {
std::string typeName(Type type) {
  if (arrayValue(type))
    return typeName(*type.element) + " [" + (type.extent ? std::to_string(type.extent) : "") + "]";
  if (collectionValue(type)) {
    if (type.base == Type::Queue) return typeName(*type.element) + " queue";
    if (type.base == Type::Stack) return typeName(*type.element) + " stack";
    return typeName(*type.key) + ", " + typeName(*type.element) + " pair";
  }
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
  bool borrowed = false; // A reference parameter: the caller owns the storage.
  // Construction parts a container local may (on some path) or must (on
  // every path) still be missing, by ConstructionPart::id.
  std::set<std::string> mayMiss, mustMiss;
  Token declaration;
};
class Analyzer {
  const Symbols &symbols;
  using Scopes = std::vector<std::map<std::string, Local>>;
  Scopes scopes;
  struct LoopFlow {
    std::vector<Scopes> continues;
    std::vector<Scopes> breaks;
    size_t depth = 0; // Scopes at or past this index belong to the loop body.
  };
  std::vector<LoopFlow> loops;
  FunctionAnalysis analysis;
  Type result;
  const Expr *ownerDestination = nullptr;
  std::string container;
  Usings usings; // Directives visible where the analyzed code was written.
  bool method = false;
  // The construction a local declaration may leave incomplete, and what it left.
  const Expr *incompleteAllowed = nullptr;
  std::set<std::string> incompleteMissing;
  // The local a `return` statement may move instead of copying.
  const Expr *movable = nullptr;
  // Set in a generated static $eval: the static it initializes, and the
  // assignment target that may write it.
  std::string initializing;
  const Expr *initializationTarget = nullptr;
  bool initializerScope = false; // Analyzing a default or static initializer.
  // Inferring an untyped getter's type: its return sets the result.
  bool inferring = false;

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
    // Initializers are analyzed without fields in scope.
    MemberPath path;
    if (initializerScope && symbols.records.count(container) &&
        symbols.findField(container, token.text, path))
      fail(token, "initializers cannot read fields; '" + token.text + "' is a field of '" + container + "'");
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
        if (incoming.empty()) continue;
        std::set<std::string> may, must = incoming.front().at(i).at(entry.first).mustMiss;
        for (const auto &state : incoming) {
          const auto &other = state.at(i).at(entry.first);
          may.insert(other.mayMiss.begin(), other.mayMiss.end());
          std::set<std::string> both;
          for (const auto &id : must)
            if (other.mustMiss.count(id)) both.insert(id);
          must = both;
        }
        entry.second.mayMiss = may;
        entry.second.mustMiss = must;
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
    requireComplete(e);
    auto type = checkExpression(e, expected);
    analysis.expressionTypes.emplace(&e, type);
    // Reading stored container state as a value copies it. A container
    // with a destructor is destroyed exactly once, so it is move-only.
    if (recordValue(type) && e.addressable() && !analysis.properties.count(&e) && &e != movable &&
        symbols.destructible(type.record))
      fail(e.token, "cannot copy '" + type.record + "': " +
                        (moveOnlyByCollection(type.record) ? "it holds a collection, so it is move-only"
                                                           : "it has a destructor") +
                        "; pass it by reference (&), or return a local to move it");
    if (collectionValue(type) && e.addressable() && !analysis.properties.count(&e) && &e != movable)
      fail(e.token, "cannot copy " + typeName(type) + " '" + e.token.text +
                        "': collections are move-only; pass it by reference (&), or return a local to move it");
    if (arrayValue(type) && symbols.moveOnly(type) && e.addressable() && !analysis.properties.count(&e) &&
        &e != movable)
      fail(e.token, "cannot copy " + typeName(type) + " '" + e.token.text + "': " +
                        (dynamicArray(type) ? "dynamic arrays are move-only"
                                            : "its elements cannot be copied implicitly, so it is move-only") +
                        "; pass it by reference (&), or return a local to move it");
    if (expected != Type::Void) {
      convert(e, type, expected);
      return expected;
    }
    return type;
  }
  // A container that is move-only only because collections live in it.
  bool moveOnlyByCollection(const std::string &record) const {
    if (symbols.count(destructorKey(record))) return false;
    for (const auto &field : symbols.records.at(record).fields)
      if (recordValue(field.type) && symbols.destructible(field.type.record) &&
          !moveOnlyByCollection(field.type.record))
        return false;
    return true;
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
      requireAccess(function->second.protectedAccess, name.substr(0, dot),
                    propertyName(name).empty() ? memberName(name) : propertyName(name), token);
  }
  std::string constructorText(const std::string &key) const {
    const auto &params = symbols.at(key).parameters;
    const bool partial = memberName(key).rfind("$part", 0) == 0;
    std::string text = (partial ? "-" : "") + key.substr(0, key.rfind('.')) + "(";
    for (size_t i = 1; i < params.size(); ++i)
      text += (i > 1 ? ", " : "") + typeName(params[i].type) + (params[i].reference ? "&" : "");
    return text + ")";
  }
  std::string partsText(const std::string &record, const std::set<std::string> &ids) const {
    std::string text;
    for (const auto &part : symbols.parts(record))
      if (ids.count(part.id())) text += (text.empty() ? "" : ", ") + part.text();
    return text;
  }
  // Splits a call's arguments across construction parts in order. Each part
  // takes the arguments of one of its overloads (none for its automatic
  // overload), or, when it has no automatic overload, may be left missing.
  // The split leaving the fewest parts missing wins, then one whose
  // overloads match the argument types exactly; others are ambiguous.
  std::vector<PartCall> plan(const Expr &e, const std::string &record,
                             const std::vector<ConstructionPart> &parts, std::set<std::string> &missing) {
    const auto savedAnalysis = analysis;
    const auto savedScopes = scopes;
    auto restore = [&] {
      analysis = savedAnalysis;
      scopes = savedScopes;
    };
    // Argument types without a target type. Addressable arguments are read
    // as storage, so reference parameters and move-only values compare too.
    std::vector<Type> natural;
    std::vector<bool> addressable;
    for (const auto &argument : e.args) {
      addressable.push_back(storage(*argument));
      natural.push_back(addressable.back() ? location(*argument).type : expression(*argument));
    }
    restore();
    std::map<std::pair<std::string, size_t>, std::pair<bool, bool>> checked;
    auto check = [&](const std::string &key, size_t first) {
      auto found = checked.find({key, first});
      if (found != checked.end()) return found->second;
      const auto &params = symbols.at(key).parameters;
      bool exact = true, viable = true;
      for (size_t i = 1; i < params.size(); ++i)
        exact = exact && natural[first + i - 1] == params[i].type &&
                (!params[i].reference || addressable[first + i - 1]);
      try {
        arguments(e, symbols.at(key), 1, first, params.size() - 1);
      } catch (const std::runtime_error &) {
        viable = false;
      }
      restore();
      return checked[{key, first}] = {viable, exact};
    };
    struct Split {
      std::vector<PartCall> calls;
      std::set<std::string> missing;
      bool exact = true;
    };
    std::vector<Split> splits;
    Split current;
    std::function<void(size_t, size_t)> search = [&](size_t index, size_t position) {
      if (index == parts.size()) {
        if (position == e.args.size()) splits.push_back(current);
        return;
      }
      const auto &part = parts[index];
      for (const auto &key : part.overloads) {
        const size_t count = symbols.at(key).parameters.size() - 1;
        if (position + count > e.args.size()) continue;
        const auto [viable, exact] = count ? check(key, position) : std::pair<bool, bool>{true, true};
        if (!viable) continue;
        const auto saved = current;
        current.calls.push_back({part.path, key, position, count});
        current.exact = current.exact && exact;
        search(index + 1, position + count);
        current = saved;
      }
      if (part.automatic.empty()) {
        const auto saved = current;
        current.missing.insert(part.id());
        search(index + 1, position);
        current = saved;
      }
    };
    search(0, 0);
    std::string given;
    for (size_t i = 0; i < natural.size(); ++i) given += (i ? ", " : "") + typeName(natural[i]);
    if (splits.empty()) {
      std::string list;
      for (const auto &part : parts)
        for (const auto &key : part.overloads) list += (list.empty() ? "" : ", ") + constructorText(key);
      fail(e.token, "no constructor of '" + record + "' accepts (" + given + "); candidates: " + list);
    }
    size_t fewest = splits.front().missing.size();
    for (const auto &split : splits) fewest = std::min(fewest, split.missing.size());
    std::vector<Split> best, exact;
    for (const auto &split : splits)
      if (split.missing.size() == fewest) {
        best.push_back(split);
        if (split.exact) exact.push_back(split);
      }
    if (best.size() > 1 && exact.size() == 1) best = exact;
    auto describe = [&](const Split &split) {
      std::string text;
      for (const auto &call : split.calls)
        if (call.count) text += (text.empty() ? "" : " + ") + constructorText(call.key);
      return text.empty() ? std::string("no arguments") : text;
    };
    if (best.size() > 1)
      fail(e.token, "constructor call " + record + "(" + given + ") is ambiguous between " +
                        describe(best[0]) + " and " + describe(best[1]));
    for (const auto &call : best.front().calls)
      if (call.count) arguments(e, symbols.at(call.key), 1, call.first, call.count);
    missing = best.front().missing;
    return best.front().calls;
  }
  // Construction of `record` from a call's arguments. Only a declared local
  // may be left incomplete; it is completed later with `local(...)`.
  Type construction(const Expr &e, const std::string &record) {
    Type type = Type::Record;
    type.record = record;
    const auto parts = symbols.parts(record);
    if (parts.empty()) {
      if (!e.args.empty())
        fail(e.token, "'" + record + "' declares no constructors; default container construction takes no arguments");
      analysis.constructions[&e] = {};
      return type;
    }
    std::set<std::string> missing;
    analysis.constructions[&e] = plan(e, record, parts, missing);
    if (&e == incompleteAllowed) incompleteMissing = missing;
    else if (!missing.empty()) incomplete(e.token, record, missing);
    return type;
  }
  [[noreturn]] void incomplete(const Token &token, const std::string &record,
                               const std::set<std::string> &missing) const {
    fail(token, "this construction of '" + record + "' leaves " + partsText(record, missing) +
                    " missing; pass arguments for every part, or declare a local (" + record +
                    " x(...)) and complete it later with x(...)");
  }
  // The parts a default construction leaves missing: those without an
  // automatic overload.
  std::set<std::string> defaultMissing(const std::string &record) const {
    std::set<std::string> missing;
    for (const auto &part : symbols.parts(record))
      if (part.automatic.empty()) missing.insert(part.id());
    return missing;
  }
  // A local that may still miss construction parts cannot be used as a
  // whole: no method calls, references, copies, moves, or assignment.
  void requireComplete(const Expr &e) const {
    if (e.kind != "name") return;
    for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
      auto local = scope->find(e.token.text);
      if (local == scope->end()) continue;
      if (!local->second.mayMiss.empty())
        fail(e.token, "'" + e.token.text + "' may be incomplete: " +
                          partsText(local->second.type.record, local->second.mayMiss) +
                          " may still be missing; complete it with " + e.token.text + "(...) first");
      return;
    }
  }
  // Every local in scopes[first..] must be complete where they end.
  void requireCompleteScopes(size_t first) const {
    for (size_t i = first; i < scopes.size(); ++i)
      for (const auto &[name, local] : scopes[i])
        if (!local.mayMiss.empty())
          fail(local.declaration, "'" + name + "' may be incomplete when its scope ends: " +
                                      partsText(local.type.record, local.mayMiss) +
                                      " may still be missing; complete it with " + name +
                                      "(...) on every path");
  }
  // `g(w)` supplies parts the local g is missing on every path.
  Type completion(const Expr &e) {
    size_t depth = scopes.size();
    while (depth > 0 && !scopes[depth - 1].count(e.token.text)) --depth;
    --depth; // The scope index holding the local; findLocal found it.
    // plan() restores the scopes after each trial, so look the local up again
    // after it rather than holding a pointer across it.
    auto local = [&]() -> Local & { return scopes[depth].at(e.token.text); };
    const auto record = local().type.record;
    if (!loops.empty() && depth < loops.back().depth)
      fail(e.token, "complete '" + e.token.text + "' outside the loop; a loop body can run more than "
                    "once, which would construct a part twice");
    std::vector<ConstructionPart> parts;
    for (const auto &part : symbols.parts(record))
      if (local().mustMiss.count(part.id())) parts.push_back(part);
    if (parts.empty())
      fail(e.token, "'" + e.token.text + "' was completed on some paths but not others; " +
                        partsText(record, local().mayMiss) + " may already have run");
    std::set<std::string> missing;
    auto calls = plan(e, record, parts, missing);
    if (calls.empty()) fail(e.token, "completing '" + e.token.text + "' requires arguments for " +
                                         partsText(record, local().mustMiss));
    for (const auto &call : calls)
      for (const auto &part : parts)
        if (part.path == call.path && std::find(part.overloads.begin(), part.overloads.end(), call.key) !=
                                          part.overloads.end()) {
          local().mayMiss.erase(part.id());
          local().mustMiss.erase(part.id());
        }
    analysis.completions[&e] = std::move(calls);
    return Type::Void;
  }
  // `g(5)` on a complete object of `record` (a local, a reference, a
  // pointer's payload, or owner's field at `field`) constructs a new state,
  // which must be complete, and replaces the old one in the same storage.
  // Takes names by value: plan() restores the scopes, which may hold them.
  Type reinitialization(const Expr &e, const std::string record, const std::string owner = "",
                        const std::vector<size_t> field = {}) {
    const auto saved = incompleteAllowed;
    incompleteAllowed = &e;
    const auto type = construction(e, record);
    incompleteAllowed = saved;
    if (!incompleteMissing.empty()) {
      const auto missing = incompleteMissing;
      incompleteMissing.clear();
      fail(e.token, "reinitializing '" + e.token.text + "' leaves " + partsText(record, missing) +
                        " missing; a reinitialization must pass arguments for every part");
    }
    auto calls = std::move(analysis.constructions.at(&e));
    analysis.constructions.erase(&e);
    analysis.reinitializations[&e] = {type, std::move(calls), owner, field};
    return Type::Void;
  }
  // Checks a call's arguments [first, first + count) against a signature's
  // parameters from offset.
  void arguments(const Expr &e, const Signature &signature, size_t offset, size_t first = 0,
                 size_t count = std::string::npos) {
    if (count == std::string::npos) count = e.args.size();
    for (size_t i = 0; i < count; ++i) {
      const auto &argument = *e.args[first + i];
      const auto &parameter = signature.parameters[i + offset];
      if (parameter.reference) {
        requireComplete(argument);
        auto local = location(argument);
        if (heapStorage(argument)) fail(argument.token, "cannot expose tracked heap storage by reference");
        if (growableElement(argument))
          fail(argument.token, "cannot pass an element of a dynamic array by reference: adding elements can "
                               "move it; copy it into a local first");
        if (local.constant) fail(argument.token, "cannot pass a constant to a mutable reference");
        // A derived container passes its state for the parameter's parent.
        std::vector<size_t> path;
        if (recordValue(local.type) && recordValue(parameter.type) && local.type != parameter.type) {
          const auto count = symbols.parentPath(local.type.record, parameter.type.record, path);
          if (count > 1)
            fail(argument.token, "'" + local.type.record + "' contains '" + parameter.type.record +
                                     "' through more than one parent, so the reference would be ambiguous");
          if (count) {
            analysis.upcasts[&argument] = path;
            continue;
          }
        }
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
  }
  // The qualified name a relative name or path denotes, or empty. Each scope
  // from the innermost container outward, then file scope, is searched for
  // its own member first and then through the using directives declared in
  // it; two directives that both provide the name make it ambiguous.
  template <class Exists>
  std::string qualify(const std::string &op, const Token &token, Exists exists) const {
    for (auto scope = container;;) {
      const auto direct = scope.empty() ? op : scope + "." + op;
      if (exists(direct)) return direct;
      std::string found, via;
      if (usings)
        for (const auto &directive : *usings) {
          const auto candidate = directive.target + "." + op;
          if (directive.level != scope || !exists(candidate) || candidate == found) continue;
          if (!found.empty())
            fail(token, "'" + op + "' is ambiguous between '" + found + "' (using " + via +
                            ") and '" + candidate + "' (using " + directive.target + ")");
          found = candidate;
          via = directive.target;
        }
      if (!found.empty()) return found;
      if (scope.empty()) return "";
      const auto dot = scope.rfind('.');
      scope = dot == std::string::npos ? "" : scope.substr(0, dot);
    }
  }
  Symbols::const_iterator resolve(const std::string &op, const Token &token) const {
    const auto name = qualify(op, token, [&](const std::string &n) { return symbols.count(n) != 0; });
    return name.empty() ? symbols.end() : symbols.find(name);
  }
  // The qualified container a construction call such as `Space()` or
  // `Name.Space()` names, resolved like a function, or empty.
  std::string resolveRecord(const std::string &op, const Token &token) const {
    return qualify(op, token, [&](const std::string &n) { return symbols.records.count(n) != 0; });
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
    return qualify(path, e.token, [&](const std::string &n) { return symbols.statics.count(n) != 0; });
  }
  // True when e, or a dotted prefix of it, is static storage.
  bool staticStorage(const Expr &e) {
    for (const Expr *part = &e;; part = part->args[0].get()) {
      if (!staticPath(*part).empty()) return true;
      if (part->kind != ".") return false;
    }
  }
  bool accessible(bool isProtected, const std::string &declaring) const {
    return !isProtected || container == declaring || container.rfind(declaring + ".", 0) == 0 ||
           symbols.derivesFrom(container, declaring);
  }
  // Whether e names a property: `Age` inside a method of a container that
  // has one and no local or field of that name, or `p.Age` where p's
  // container has no accessible field Age. A property may share its name
  // with a protected field, which it then exposes to outside code. Sets
  // owner to the receiver's container.
  bool propertyOf(const Expr &e, std::string &owner) {
    auto has = [&](const std::string &record, const std::string &name) {
      MemberPath path;
      return symbols.findMethod(record, "get-" + name, path) != symbols.end() ||
             symbols.findMethod(record, "set-" + name, path) != symbols.end();
    };
    if (e.kind == "name") {
      if (findLocal(e.token) || !method || !symbols.records.count(container) || !staticPath(e).empty())
        return false;
      if (!has(container, e.token.text)) return false;
      owner = container;
      return true;
    }
    if (e.kind != "." || enumOf(e) || !staticPath(e).empty()) return false;
    const auto &base = *e.args[0];
    Type type = storage(base) ? location(base).type : expression(base);
    if (type.depth) type = type.pointee();
    if (!recordValue(type)) return false;
    MemberPath field;
    if (symbols.findField(type.record, e.token.text, field) &&
        accessible(field.field->protectedAccess, field.declaring))
      return false;
    if (!has(type.record, e.token.text)) return false;
    owner = type.record;
    return true;
  }
  // q.Front, s.Top, p.Last, p.LastKey, and .Len on stored collections; Void
  // when e is not such a read.
  Type collectionMember(const Expr &e) {
    if (e.kind != "." || enumOf(e) || !staticPath(e).empty() || staticStorage(*e.args[0])) return Type::Void;
    const auto &base = *e.args[0];
    if (!storage(base)) return Type::Void;
    const auto type = location(base).type;
    if (arrayValue(type)) {
      if (e.token.text != "Len") fail(e.token, typeName(type) + " has no member '" + e.token.text + "'; an array has Len");
      requireComplete(base);
      analysis.collectionMembers[&e] = "Len";
      analysis.expressionTypes[&e] = Type::Long;
      return Type::Long;
    }
    if (!collectionValue(type)) return Type::Void;
    const auto &member = e.token.text;
    Type result = Type::Void;
    if (member == "Len") result = Type::Long;
    else if (type.base == Type::Queue && member == "Front") result = *type.element;
    else if (type.base == Type::Stack && member == "Top") result = *type.element;
    else if (type.base == Type::Pair && member == "Last") result = *type.element;
    else if (type.base == Type::Pair && member == "LastKey") result = *type.key;
    if (result == Type::Void)
      fail(e.token, typeName(type) + " has no member '" + member + "'; " +
                        (type.base == Type::Queue   ? "a queue has Front and Len"
                         : type.base == Type::Stack ? "a stack has Top and Len"
                                                    : "a pair has Last, LastKey, Len, and p[key]"));
    requireComplete(base);
    analysis.collectionMembers[&e] = member;
    analysis.expressionTypes[&e] = result;
    return result;
  }
  // Existing storage: an addressable expression that is not a property.
  bool storage(const Expr &e) {
    std::string owner;
    return e.addressable() && !propertyOf(e, owner) && !(e.kind == "index" && pairIndex(e));
  }
  // p[key] on a stored pair, which reads a copy rather than naming storage.
  bool pairIndex(const Expr &e) {
    const auto &base = *e.args[0];
    if (!storage(base)) return false;
    const auto type = location(base).type;
    return !type.depth && type.base == Type::Pair;
  }
  // Storage inside a dynamic array, which moves when the array grows, so
  // it cannot be borrowed by reference or address.
  bool growableElement(const Expr &e) const {
    for (const Expr *part = &e; part->kind == "." || part->kind == "index"; part = part->args[0].get())
      if (part->kind == "index" && dynamicArray(analysis.expressionTypes.at(part->args[0].get()))) return true;
    return false;
  }
  [[noreturn]] void notStorage(const Expr &e) const {
    fail(e.token, "'" + e.token.text + "' is a property, not storage; it cannot be passed by reference, "
                  "have its address taken, or be reinitialized");
  }
  // A property's receiver: a constant receiver cannot run accessors, which
  // may modify it, and a setter cannot run on a temporary.
  void propertyReceiver(const Expr &e, const std::string &verb) {
    if (e.kind != ".") return;
    const auto &base = *e.args[0];
    if (!storage(base)) {
      const auto type = expression(base);
      if (verb == "set" && !type.depth)
        fail(e.token, "cannot set property '" + e.token.text + "' of a temporary");
      return;
    }
    requireComplete(base);
    const auto local = location(base);
    if (!local.type.depth && local.constant)
      fail(e.token, "cannot " + verb + " property '" + e.token.text +
                        "' of a constant; its accessors may modify it");
  }
  struct PendingProperty : std::runtime_error {
    PendingProperty() : std::runtime_error("property type pending") {}
  };
  Symbols::const_iterator accessor(const Expr &e, const std::string &owner, const std::string &kind,
                                   std::vector<size_t> &path) {
    MemberPath member;
    const auto found = symbols.findMethod(owner, kind + "-" + e.token.text, member);
    if (found == symbols.end())
      fail(e.token, "property '" + e.token.text + "' of '" + owner + "' has no " +
                        (kind == "get" ? "getter; it can only be assigned" : "setter; it is read-only"));
    requireAccess(found, e.token);
    if (found->second.inferred) throw PendingProperty();
    path = member.indices;
    return found;
  }
  // `p.Age` calls the getter.
  Type propertyRead(const Expr &e, const std::string &owner) {
    propertyReceiver(e, "read");
    auto &access = analysis.properties[&e];
    access.owner = owner;
    const auto getter = accessor(e, owner, "get", access.getterPath);
    access.getter = getter->first;
    analysis.expressionTypes[&e] = getter->second.result;
    return getter->second.result;
  }
  // `p.Age = v` calls the setter; `p.Age += v` and `p.Age++` call the getter
  // and then the setter on a receiver evaluated once.
  Type propertyWrite(const Expr &e, const std::string &owner, bool read) {
    propertyReceiver(e, "set");
    auto &access = analysis.properties[&e];
    access.owner = owner;
    const auto setter = accessor(e, owner, "set", access.setterPath);
    access.setter = setter->first;
    if (read) access.getter = accessor(e, owner, "get", access.getterPath)->first;
    const auto type = setter->second.parameters.back().type;
    analysis.expressionTypes[&e] = type;
    return type;
  }
  // An addressable expression denotes existing storage. Dot inherits the
  // mutability of its base; dereferencing borrows the pointed-to storage.
  Local location(const Expr &e, bool read = true) {
    Local local{Type::Void, false};
    if (const auto qualified = staticPath(e); !qualified.empty()) {
      const auto &info = symbols.statics.at(qualified);
      // Statics initialize in declaration order, after every imported static.
      if (!initializing.empty() && &e != initializationTarget && !info.imported &&
          info.order >= symbols.statics.at(initializing).order)
        fail(e.token, qualified == initializing
                          ? "static field '" + qualified + "' cannot read itself in its initializer"
                          : "static field '" + qualified + "' is initialized after '" + initializing +
                                "'; static fields initialize in declaration order");
      const auto dot = qualified.rfind('.');
      requireAccess(info.protectedAccess, qualified.substr(0, dot), qualified.substr(dot + 1), e.token);
      analysis.staticFields[&e] = qualified;
      local = {info.type, info.constant && &e != initializationTarget};
    } else if (e.kind == "name") {
      if (std::string owner; !findLocal(e.token) && propertyOf(e, owner)) notStorage(e);
      local = lookup(e.token);
      if (local.receiver) analysis.receiverStorage.insert(&e);
      if (read) requireInitialized(e.token);
    } else if (e.kind == "index") {
      // a[i] names an element; an index outside the array traps.
      const auto &base = *e.args[0];
      if (!storage(base)) fail(e.token, "index a stored array");
      local = location(base, read);
      if (!arrayValue(local.type)) {
        if (!local.type.depth && local.type.base == Type::Pair)
          fail(e.token, "pair values are read-only so far; p[key] cannot be written or borrowed");
        fail(e.token, "only arrays and pairs can be indexed; " + typeName(local.type) + " cannot");
      }
      requireComplete(base);
      if (const auto index = expression(*e.args[1]); !integral(index) && !enumValue(index))
        fail(e.args[1]->token, "an array index must be an integer, not " + typeName(index));
      expression(*e.args[1], Type::Long);
      local.type = *local.type.element;
    } else if (e.kind == "unary" && e.token.text == "*") {
      auto ptr = expression(*e.args[0]);
      if (!ptr.depth || ptr == Type::Pointer) fail(e.token, "dereference requires a typed pointer");
      local.type = ptr.pointee();
    } else if (e.kind == ".") {
      if (enumOf(e))
        fail(e.token, "enum member '" + e.args[0]->token.text + "." + e.token.text +
                          "' is a constant");
      const auto &base = *e.args[0];
      if (std::string owner; propertyOf(e, owner)) notStorage(e);
      if (storage(base)) local = location(base);
      else {
        local.type = expression(base);
        if (recordValue(local.type)) fail(e.token, "field access requires addressable container storage");
      }
      if (local.type.depth) {
        local.type = local.type.pointee();
        local.constant = false; // A constant pointer can point to mutable storage.
      }
      if (collectionValue(local.type) || arrayValue(local.type))
        fail(e.token, "'" + e.token.text + "' is a read-only member of " + typeName(local.type));
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
    if (e.kind == "." || e.kind == "index" || (e.kind == "unary" && e.token.text == "*"))
      return heapStorage(*e.args[0]);
    return false;
  }
  Type checkExpression(const Expr &e, Type expected) {
    const auto &op = e.token.text;
    if (e.kind == "initializer") {
      if (!recordValue(expected)) fail(e.token, "named initialization requires a container value type");
      if (!symbols.constructors(expected.record).empty())
        fail(e.token, "'" + expected.record + "' has constructors; construct it with " + expected.record +
                          "(...) instead of a { field: value } initializer");
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
    if (e.kind == "index" && pairIndex(e)) {
      // p[key] reads the value stored for key; a missing key traps.
      const auto &base = *e.args[0];
      requireComplete(base);
      const auto type = location(base).type;
      expression(*e.args[1], *type.key);
      analysis.expressionTypes[&e] = *type.element;
      return *type.element;
    }
    if (e.addressable()) {
      if (std::string owner; propertyOf(e, owner)) return propertyRead(e, owner);
      if (const auto member = collectionMember(e); member != Type::Void) return member;
      return location(e).type;
    }
    if (e.kind == "entry") fail(e.token, "key : value entries can only be added to a pair, as p += key : value");
    if (e.kind == "unary" && op == "&") {
      const auto &arg = *e.args[0];
      requireComplete(arg);
      const Expr *root = &arg;
      std::string suffix;
      while (root->kind == ".") {
        suffix = "." + root->token.text + suffix;
        root = root->args[0].get();
      }
      if (root->kind == "name") {
        bool local = false;
        for (const auto &scope : scopes) local |= scope.count(root->token.text) != 0;
        auto function = resolve(root->token.text + suffix, arg.token);
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
      if (growableElement(arg))
        fail(arg.token, "cannot take the address of a dynamic array element: adding elements can move it");
      if (local.constant || managedValue(local.type))
        fail(arg.token, "cannot take the address of constant or managed text storage");
      return local.type.pointerTo();
    }
    if (e.kind == "call") {
      // `Gun g(5)` and `Gun* p(5)` arrive with the constructed type resolved.
      if (recordValue(e.castType)) return construction(e, e.castType.record);
      // `g(w)` on a container local supplies its missing parts; on a
      // complete one, or on a pointer to one, it reinitializes the object.
      if (!e.receiver && op.find('.') == std::string::npos)
        if (const auto *local = findLocal(e.token)) {
          const bool pointer = local->type.depth == 1 && local->type.base == Type::Record;
          if ((recordValue(local->type) || pointer) && local->receiver)
            fail(e.token, "cannot reinitialize the receiver inside its own method");
          if (recordValue(local->type) && !local->mayMiss.empty()) return completion(e);
          if (recordValue(local->type)) {
            if (local->constant) fail(e.token, "cannot reinitialize constant '" + e.token.text + "'");
            return reinitialization(e, local->type.record);
          }
          if (pointer) {
            requireInitialized(e.token);
            return reinitialization(e, local->type.record);
          }
        }
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
      auto found = resolve(op, e.token);
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
          requireComplete(base);
          Type t;
          bool constant = false, stored = true;
          if (storage(base)) {
            auto local = location(base);
            t = local.type;
            constant = !t.depth && local.constant;
          } else {
            t = expression(base);
            stored = t.depth != 0;
          }
          if (t.depth) t = t.pointee();
          if (!recordValue(t)) fail(base.token, "method receiver requires a container");
          MemberPath path;
          const auto name = op.substr(op.rfind('.') + 1);
          found = symbols.findMethod(t.record, name, path);
          // `box.Inner(5)` on a container field reinitializes the field.
          if (found == symbols.end() && symbols.findField(t.record, name, path) &&
              recordValue(path.field->type) && !path.field->parentView) {
            requireAccess(path.field->protectedAccess, path.declaring, name, e.token);
            if (constant) fail(e.token, "cannot reinitialize field '" + name + "' of a constant");
            if (!stored) fail(e.token, "cannot reinitialize field '" + name + "' of a temporary");
            return reinitialization(e, path.field->type.record, t.record, path.indices);
          }
          if (found == symbols.end()) fail(e.token, "unknown instance method '" + op + "'");
          if (constant) fail(base.token, "cannot call a mutable method on a constant");
          if (!path.indices.empty()) analysis.receiverPaths[&e] = {t.record, path.indices};
        }
      }
      const auto constructed = found == symbols.end() && !instance ? resolveRecord(op, e.token) : "";
      if (found == symbols.end() && !e.receiver && constructed.empty()) {
        Type builtin;
        if (calendarCall(e, builtin)) return builtin;
      }
      if (!constructed.empty()) return construction(e, constructed);
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
      arguments(e, signature, offset);
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
    if (!terminated) requireCompleteScopes(scopes.size() - 1);
    scopes.pop_back();
    return terminated;
  }
  bool statement(const Stmt &s) {
    if (s.kind == "block")
      return block(s.body);
    if (s.kind == "break" || s.kind == "continue") {
      if (loops.empty())
        fail(s.token, s.kind + " requires an enclosing loop");
      requireCompleteScopes(loops.back().depth);
      if (s.kind == "continue")
        loops.back().continues.push_back(scopes);
      else
        loops.back().breaks.push_back(scopes);
      return true;
    }
    if (s.kind == "returnOwner") {
      requireCompleteScopes(0);
      if (!s.expr || s.expr->kind != "name" || lookup(s.expr->token).constant ||
          expression(*s.expr) != result)
        fail(s.token, "return :: requires a mutable owning local of the declared type");
      return true;
    }
    if (s.kind == "return") {
      if (inferring) {
        result = expression(*s.expr);
        if (result == Type::Void) fail(s.expr->token, "a getter must produce a value");
        inferring = false;
        return true;
      }
      if (result.role == Type::Owner) fail(s.token, "owning function requires return :: on every return");
      if (s.expr) requireComplete(*s.expr);
      requireCompleteScopes(0);
      // `return local` moves a value with a destructor out of the function.
      if (s.expr && s.expr->kind == "name" &&
          (collectionValue(result) || (arrayValue(result) && symbols.moveOnly(result)) ||
           (recordValue(result) && symbols.destructible(result.record)))) {
        const auto *local = findLocal(s.expr->token);
        if (local && !local->receiver && !local->borrowed) {
          movable = s.expr.get();
          analysis.moves.insert(&s);
        }
      }
      require(s.expr ? expression(*s.expr, result) : Type::Void, result,
              s.token);
      movable = nullptr;
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
      loops.back().depth = scopes.size();
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
      loops.back().depth = scopes.size();
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
      requireCompleteScopes(scopes.size() - 1);
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
      std::set<std::string> missing;
      if (!s.expr && s.kind == "own" && recordValue(s.type.pointee())) {
        missing = defaultMissing(s.type.pointee().record);
        if (!missing.empty()) incomplete(s.token, s.type.pointee().record, missing);
      } else if (!s.expr && recordValue(s.type))
        missing = defaultMissing(s.type.record);
      if (s.kind == "let" && s.expr && recordValue(s.expr->castType)) incompleteAllowed = s.expr.get();
      incompleteMissing.clear();
      auto type = s.expr ? expression(*s.expr, hint) : s.type;
      if (incompleteAllowed) missing = incompleteMissing;
      incompleteAllowed = nullptr;
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
      analysis.localTypes[&s] = type;
      auto name = s.token;
      name.text = s.name;
      // A fixed array without an initializer gives each element its
      // default, which must not need running code.
      if (fixedArray(type) && !s.expr && recordValue(*type.element) &&
          symbols.runtimeDefaults(type.element->record))
        fail(s.token, "'" + s.name + "' cannot default its elements: '" + type.element->record +
                          "' is built at run time (constructors or computed defaults); use a dynamic array (" +
                          type.element->record + " []) and add elements to it");
      bind(name, {type, s.constant,
                  s.kind == "own" || s.expr != nullptr || recordValue(type) || collectionValue(type) ||
                      arrayValue(type)});
      auto &bound = scopes.back().at(name.text);
      bound.mayMiss = bound.mustMiss = missing;
      bound.declaration = name;
    } else if (s.kind == "expr")
      expression(*s.expr);
    else {
      requireComplete(*s.target);
      std::string owner;
      auto local = propertyOf(*s.target, owner) ? Local{propertyWrite(*s.target, owner, s.kind != "assign"), false}
                                                : location(*s.target, s.kind != "assign");
      if (local.constant) {
        const Expr *root = s.target.get();
        while ((root->kind == "." || root->kind == "index") && !root->args.empty()) root = root->args[0].get();
        fail(s.token, "cannot modify constant '" + root->token.text + "'");
      }
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
      if (arrayValue(local.type) && s.kind != "assign") {
        // a += x appends to a dynamic array and a-- removes its last element.
        if (fixedArray(local.type))
          fail(s.token, typeName(local.type) + " has a fixed length; '" + s.kind +
                            "' needs a dynamic array (" + typeName(*local.type.element) + " [])");
        if (s.kind == "+=") {
          if (s.expr->kind == "entry")
            fail(s.expr->token, "key : value entries belong in a pair, not in " + typeName(local.type));
          expression(*s.expr, *local.type.element);
        } else if (s.kind != "--")
          fail(s.token, "'" + s.kind + "' is not supported on " + typeName(local.type) +
                            " yet; dynamic arrays support += and --");
        return false;
      }
      if (collectionValue(local.type) && s.kind != "assign") {
        // q += x adds to a queue's back or a stack's top, p += key : value
        // adds or replaces an entry, and x-- removes the queue's front, the
        // stack's top, or the pair's last added entry.
        if (s.kind == "+=") {
          if (local.type.base == Type::Pair) {
            if (s.expr->kind != "entry")
              fail(s.expr->token, "add to a pair as p += key : value");
            expression(*s.expr->args[0], *local.type.key);
            expression(*s.expr->args[1], *local.type.element);
          } else {
            if (s.expr->kind == "entry")
              fail(s.expr->token, "key : value entries belong in a pair, not in " + typeName(local.type));
            expression(*s.expr, *local.type.element);
          }
        } else if (s.kind != "--")
          fail(s.token, "'" + s.kind + "' is not supported on " + typeName(local.type) +
                            " yet; collections support += and --");
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
      if (s.target->kind == "name" && !analysis.staticFields.count(s.target.get()) &&
          !analysis.properties.count(s.target.get()))
        lookup(s.target->token).initialized = true;
    }
    return false;
  }

public:
  Analyzer(const Symbols &symbols, Type result)
      : symbols(symbols), result(result) {}
  // The type an untyped getter's expression produces, or Void while it
  // reads another property whose type is not known yet.
  Type inferGetter(const Function &getter) {
    inferring = true;
    try {
      run(getter);
    } catch (const PendingProperty &) {
      return Type::Void;
    }
    return result;
  }
  // The LLVM constant for a static field initializer. Only literals, type
  // limits, and enum members of the field's type are compile-time constants.
  // Type-checks an initializer in its container's scope, without locals or
  // fields. Returns its LLVM constant, or nothing when it must be evaluated
  // at run time; an empty string is a string/name literal.
  std::optional<std::string> constant(const std::string &qualified, const Expr &e, Type type,
                                      Usings visible = nullptr) {
    container = qualified.substr(0, qualified.rfind('.'));
    usings = std::move(visible);
    initializerScope = true;
    scopes.emplace_back();
    expression(e, type);
    if (bigInteger(type) || recordValue(type)) return std::nullopt;
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
    return std::nullopt;
  }
  FunctionAnalysis run(const Function &function) {
    usings = function.usings;
    auto dot = function.name.text.rfind('.');
    if (dot != std::string::npos)
      container = function.name.text.substr(0, dot);
    scopes.emplace_back();
    method = function.method;
    if (!function.initializes.empty()) {
      initializing = function.initializes;
      initializationTarget = function.body.front()->target.get();
    }
    if (method && !function.generated) {
      for (const auto &path : symbols.visibleFields(container))
        bind(path.field->name, {path.field->type, false, true, true});
      scopes.emplace_back();
    }
    symbols.validateType(result, function.name);
    for (const auto &parameter : function.params) {
      symbols.validateType(parameter.type, parameter.name);
      bind(parameter.name, {parameter.type, false, true, method && &parameter == &function.params.front(),
                            parameter.reference});
    }
    if (!function.nativeSymbol.empty())
      return std::move(analysis);
    const bool terminated = sequence(function.body);
    if (!terminated) requireCompleteScopes(0);
    if (!terminated && result != Type::Void)
      fail(function.name, "not all paths return a value");
    return std::move(analysis);
  }
};
} // namespace
FunctionAnalysis analyzeFunction(const Function &function,
                                 const Symbols &symbols) {
  return Analyzer(symbols, function.result).run(function);
}
void resolveProperties(Symbols &symbols, const std::vector<const Program *> &programs,
                       const std::vector<Symbols *> &tables) {
  std::vector<const Function *> accessors;
  for (const auto *program : programs)
    for (const auto &f : *program)
      if (f.method && !propertyName(f.name.text).empty()) accessors.push_back(&f);
  auto update = [&](const Function &f) {
    for (auto *table : tables)
      if (auto found = table->find(f.name.text); found != table->end()) {
        found->second.result = f.result;
        found->second.parameters = f.params;
        found->second.inferred = false;
      }
  };
  // Untyped getters take their expression's type. One may read another
  // untyped property, so repeat until every type is known.
  for (bool progress = true; progress;) {
    progress = false;
    std::string waiting;
    const Token *at = nullptr;
    for (const auto *f : accessors) {
      if (!f->inferred || memberName(f->name.text)[0] != 'g') continue;
      const auto type = Analyzer(symbols, Type::Void).inferGetter(*f);
      if (type == Type::Void) {
        waiting = propertyName(f->name.text);
        at = &f->name;
        continue;
      }
      f->result = type;
      f->inferred = false;
      update(*f);
      progress = true;
    }
    if (!progress && at)
      fail(*at, "cannot infer the type of property '" + waiting +
                    "': its getter reads a property whose type is not known yet; declare one of them "
                    "with a type, as in int " + waiting + ": ...");
  }
  // An untyped setter takes its container's getter's type; a typed one
  // must agree with it.
  for (const auto *f : accessors) {
    if (memberName(f->name.text)[0] != 's') continue;
    const auto record = f->name.text.substr(0, f->name.text.rfind('.'));
    const auto property = propertyName(f->name.text);
    const auto getter = symbols.find(getterKey(record, property));
    auto &value = f->params.back();
    if (f->inferred) {
      if (getter == symbols.end())
        fail(f->name, "setter of property '" + property + "' needs a type, as in int " + property +
                          ": ..., because '" + record + "' declares no getter for it");
      value.type = getter->second.result;
      f->inferred = false;
      update(*f);
    } else if (getter != symbols.end() && getter->second.result != value.type)
      fail(f->name, "the getter and setter of property '" + property + "' disagree on its type: " +
                        typeName(getter->second.result) + " and " + typeName(value.type));
  }
  // A property may share its name only with a protected field of its own
  // container, which it then exposes.
  for (const auto *f : accessors) {
    const auto record = f->name.text.substr(0, f->name.text.rfind('.'));
    const auto property = propertyName(f->name.text);
    for (const auto &field : symbols.records.at(record).fields)
      if (!field.parentView && field.name.text == property && !field.protectedAccess)
        fail(f->name, "property '" + property + "' has the name of a public field of '" + record +
                          "'; a property may only share its name with a protected field it exposes");
    if (symbols.count(methodKey(record, property)))
      fail(f->name, "property '" + property + "' has the name of a method of '" + record + "'");
  }
}
void resolveFieldDefaults(Symbols &symbols) {
  for (auto &[name, record] : symbols.records)
    for (auto &field : record.fields)
      if (field.initializer) {
        auto value = Analyzer(symbols, Type::Void)
                         .constant(name + "." + field.name.text, *field.initializer, field.type, field.usings);
        field.runtimeDefault = !value;
        if (value) field.initialValue = *value;
      }
}
void resolveStaticInitializers(Symbols &symbols) {
  for (auto &[name, info] : symbols.statics) {
    if (info.imported) continue;
    if (!info.initializer) {
      info.value = "zeroinitializer";
      info.runtime = recordValue(info.type) && symbols.runtimeDefaults(info.type.record);
      continue;
    }
    auto value = Analyzer(symbols, Type::Void).constant(name, *info.initializer, info.type, info.usings);
    info.runtime = !value;
    if (value) info.value = *value;
  }
}
namespace {
std::unique_ptr<Expr> clone(const Expr &e) {
  auto copy = std::make_unique<Expr>();
  copy->token = e.token;
  copy->kind = e.kind;
  copy->castType = e.castType;
  for (const auto &arg : e.args) copy->args.push_back(clone(*arg));
  if (e.receiver) copy->receiver = clone(*e.receiver);
  return copy;
}
// `target = value` as the generated function's whole body.
Function assignment(Token name, std::vector<Parameter> params, std::unique_ptr<Expr> target,
                    std::unique_ptr<Expr> value) {
  Function f{name, Type::Void, std::move(params), {}, false, {}};
  auto s = std::make_unique<Stmt>();
  s->token = value->token;
  s->kind = "assign";
  s->target = std::move(target);
  s->expr = std::move(value);
  f.body.push_back(std::move(s));
  f.generated = true;
  return f;
}
} // namespace
std::vector<Function> generateInitializers(Symbols &symbols, Symbols &declarations) {
  std::vector<Function> generated;
  for (const auto &[name, record] : symbols.records)
    for (const auto &field : record.fields) {
      if (!field.runtimeDefault) continue;
      // A method whose receiver, $self, cannot be named by the default itself.
      Token self = field.name, key = field.name;
      self.text = "$self";
      key.text = defaultKey(name, field.name.text);
      Type type = Type::Record;
      type.record = name;
      std::vector<Parameter> params{{self, type, true}};
      Signature signature{Type::Void, params, "", key, {}, true, false};
      symbols.emplace(key.text, signature);
      if (record.imported) {
        declarations.emplace(key.text, signature);
        continue;
      }
      auto target = std::make_unique<Expr>();
      target->kind = ".";
      target->token = field.name;
      auto receiver = std::make_unique<Expr>();
      receiver->kind = "name";
      receiver->token = self;
      target->args.push_back(std::move(receiver));
      generated.push_back(assignment(key, params, std::move(target), clone(*field.initializer)));
      generated.back().method = true;
      generated.back().usings = field.usings;
    }
  for (const auto &[name, info] : symbols.statics) {
    if (info.imported || !info.runtime) continue;
    Token key = info.declaration;
    key.text = name + "$eval";
    symbols.emplace(key.text, Signature{Type::Void, {}, "", key, {}, false, false});
    // The bare name resolves to this static from its container's scope.
    auto target = std::make_unique<Expr>();
    target->kind = "name";
    target->token = info.declaration;
    target->token.text = name.substr(name.rfind('.') + 1);
    std::unique_ptr<Expr> value;
    if (info.initializer) value = clone(*info.initializer);
    else {
      // A container whose defaults run at construction: default-construct it.
      value = std::make_unique<Expr>();
      value->kind = "call";
      value->token = info.declaration;
      value->token.text = info.type.record;
    }
    generated.push_back(assignment(key, {}, std::move(target), std::move(value)));
    generated.back().initializes = name;
    generated.back().usings = info.usings;
  }
  return generated;
}
} // namespace duck

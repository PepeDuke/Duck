#include "Literals.h"
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
  if (markupValue(type)) return "markup";
  if (elementValue(type)) return "element";

  if (arrayValue(type)) {
    std::string dimensions;

    for (; arrayValue(type); type = *type.element)
      dimensions += "[" + (type.extent ? std::to_string(type.extent) : std::string()) + "]";

    return typeName(type) + " " + dimensions;
  }

  if (gridValue(type)) {
    std::string dimensions;

    for (unsigned i = 0; i < type.extent; ++i) dimensions += "[-]";

    return typeName(*type.element) + " " + dimensions;
  }

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

  if (funcValue(type)) {
    if (!type.signature) return "func";

    std::string text = "func " + typeName(type.signature->result) + "(";

    for (size_t i = 0; i < type.signature->parameters.size(); ++i)
      text += (i ? "," : "") + typeName(type.signature->parameters[i].type) +
              (type.signature->parameters[i].reference ? "&" : "");

    return text + ")";
  }

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
  // A for-each variable: the place of the collection it iterates, and whether
  // its element sits in storage that moves when the collection grows.
  std::string iterating;
  bool growable = false;
  // Construction parts a container local may (on some path) or must (on
  // every path) still be missing, by ConstructionPart::id.
  std::set<std::string> mayMiss, mustMiss;
  Token declaration;
  bool memberLambda = false; // A func field that can only be called through its object.
  bool callOnly = false;     // Holds a lambda over this frame: it can be called, not copied.
  bool lent = false;         // Received from a function that may lend: it may not own its object.
  std::string scope;         // A field in a named scope, bound by its bare name.
  const Expr *constantValue = nullptr; // A # local's initializer, for Switch label checks.
};
class Analyzer {
  const Symbols &symbols;
  using Scopes = std::vector<std::map<std::string, Local>>;
  Scopes scopes;
  struct LoopFlow {
    std::vector<Scopes> continues;
    std::vector<Scopes> breaks;
    size_t depth = 0; // Scopes at or past this index belong to the loop body.
    bool isSwitch = false; // A break target only; continue passes through it.
  };
  std::vector<LoopFlow> loops;
  LoopFlow *enclosingLoop() {
    for (auto loop = loops.rbegin(); loop != loops.rend(); ++loop)
      if (!loop->isSwitch) return &*loop;

    return nullptr;
  }
  // The for-each loops being analyzed, innermost last: each collection's
  // place (see placeKey) and the variable the place starts at.
  struct Iteration {
    std::string place, root;
  };
  std::vector<Iteration> iterations;
  FunctionAnalysis analysis;
  Type result;
  const Expr *ownerDestination = nullptr;
  // Finding what a function's returns can be, before callers are analyzed.
  bool probing = false, probedOwns = false, probedLends = false;
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
  // A generated `$default$Field` method: the lambda it assigns is a member lambda.
  const Expr *memberLambdaSite = nullptr;
  // The lambda a local declaration may keep over this frame, and whether it did.
  const Expr *scopedLambda = nullptr;
  bool scopedLambdaBorrowed = false;
  // The enclosing states of the lambdas being analyzed, outermost first.
  // frames[i].scopes are the scopes lambda i was written in, and
  // frames[i].info describes lambda i.
  struct Frame {
    Scopes scopes;
    std::vector<LoopFlow> loops;
    std::vector<Iteration> iterations;
    Type result;
    const Expr *ownerDestination, *incompleteAllowed, *movable, *scopedLambda;
    FunctionAnalysis::LambdaInfo info;
  };
  std::vector<Frame> frames;
  // The `if p` branches being analyzed, whose uses of p are bound to it.
  struct CheckedPlace {
    std::string place;
    const Stmt *statement;
    size_t frame;
  };
  std::vector<CheckedPlace> checks;

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

    return frames.empty() ? nullptr : capture(frames.size() - 1, token, scopes);
  }
  // Finds token in the scopes lambda `level` was written in, capturing through
  // any lambdas between, and binds it in `into`, the scopes of that lambda.
  Local *capture(size_t level, const Token &token, Scopes &into) {
    Local *outer = nullptr;
    auto &enclosing = frames[level].scopes;

    for (auto scope = enclosing.rbegin(); scope != enclosing.rend() && !outer; ++scope) {
      auto found = scope->find(token.text);

      if (found != scope->end()) outer = &found->second;
    }

    if (!outer && level > 0) outer = capture(level - 1, token, enclosing);
    if (!outer) return nullptr;

    auto &info = frames[level].info;

    if (!outer->iterating.empty())
      fail(token, "a lambda cannot capture the for loop variable '" + token.text +
                      "': it names the current element only while the loop runs; copy it into a local first");

    if (!outer->initialized) fail(token, "variable '" + token.text + "' may be read before assignment");

    if (!outer->mayMiss.empty())
      fail(token, "a lambda cannot capture '" + token.text + "' while it may be incomplete");

    if (outer->callOnly)
      fail(token, "'" + token.text + "' holds a lambda over this frame; it can be called here but not captured");

    Local inner = *outer;
    inner.borrowed = inner.growable = false;
    inner.iterating.clear();
    auto add = [&](FunctionAnalysis::Capture::Kind kind, Token named, const Type &type) {
      for (const auto &existing : info.captures)
        if (existing.token.text == named.text) return;

      info.captures.push_back({kind, named, type});
    };
    using Capture = FunctionAnalysis::Capture;

    if (outer->receiver) {
      // this and fields are reached through the captured receiver.
      Token self = token;
      self.text = "this";
      Type record = Type::Record;
      record.record = container;
      add(Capture::This, self, record);
      info.usesThis = info.borrowed = true;
      info.record = container;
    } else if (outer->type.role != Type::Raw) {
      // A tracked pointer is captured as an alias: the lambda never owns it.
      inner.type.role = Type::Alias;
      inner.constant = true;
      add(Capture::Alias, token, inner.type);
    } else if (outer->borrowed) {
      add(Capture::Borrow, token, outer->type);
      info.borrowed = true;
    } else {
      add(Capture::Box, token, outer->type);
      analysis.captured.insert(captureKey(outer->declaration));
    }

    return &into[0].insert_or_assign(token.text, inner).first->second;
  }
  // A use of the receiver that names no local: an implicit method or property.
  void useReceiver(const Token &token) {
    if (frames.empty() || !method) return;

    Token self = token;
    self.text = "this";
    findLocal(self);
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
    // Lowering names the collection again on each use of the loop variable.
    for (const auto &iteration : iterations)
      if (iteration.root == token.text)
        fail(token, "'" + token.text + "' names the collection a for loop is iterating; use another name here");

    if (local.declaration.text.empty()) local.declaration = token;

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
  // The function a direct call resolves to, or null.
  const Signature *calledFunction(const Expr &e) const {
    if (e.kind != "call") return nullptr;

    const auto target = analysis.callTargets.find(&e);
    return target == analysis.callTargets.end() ? nullptr : &symbols.at(target->second);
  }
  // `::` takes ownership, which a function that only lends never gives.
  void requireOwningResult(const Expr &source, const Token &token) const {
    if (const auto *called = calledFunction(source); called && called->lends && !called->owns)
      fail(token, "'" + source.token.text + "' only lends its result, so '::' cannot take ownership of it; "
                  "receive it with ':'");
  }
  // A condition takes a bool or a pointer, which is true when it is not null.
  Type truth(const Expr &e, Type type, const Token &token) {
    if (type.depth) {
      analysis.pointerTests.insert(&e);
      return Type::Bool;
    }

    require(type, Type::Bool, token);
    return type;
  }
  // Named storage holding a tracked reference, which an alias can be taken from.
  bool trackedPlace(const Expr &e) {
    std::string owner;
    return (e.kind == "name" || e.kind == "." || e.kind == "index") && !propertyOf(e, owner) &&
           !analysis.collectionMembers.count(&e);
  }
  // How a chain of names and fields was written, without `this`; empty for
  // anything else. Conservative: it ignores shadowing.
  static std::string pathText(const Expr &e) {
    if (e.kind == "name") return e.token.text;
    if (e.kind != ".") return "";

    const auto base = pathText(*e.args[0]);

    if (base.empty()) return "";

    return base == "this" ? e.token.text : base + "." + e.token.text;
  }
  static bool writesPath(const Expr *e, const std::string &path) {
    if (!e) return false;

    const auto written = pathText(*e);
    return !written.empty() && (written == path || path.rfind(written + ".", 0) == 0);
  }
  // Whether a statement assigns the path or one it goes through, or moves it.
  static bool writesPlace(const Stmt &s, const std::string &path) {
    if (writesPath(s.target.get(), path)) return true;

    if ((s.kind == "own" || s.kind == "moveInto" || s.kind == "returnOwner") && writesPath(s.expr.get(), path))
      return true;

    if ((s.initializer && writesPlace(*s.initializer, path)) || (s.step && writesPlace(*s.step, path))) return true;
    for (const auto &child : s.body) if (writesPlace(*child, path)) return true;
    for (const auto &child : s.alternate) if (writesPlace(*child, path)) return true;

    return false;
  }
  // An owner a transfer can take: a call result or mutable owning storage.
  bool ownerSource(const Expr &e) {
    return e.kind == "call" || ((e.kind == "name" || e.kind == ".") && !location(e).constant);
  }
  // `target :: source` moves or allocates into an owning field.
  void ownershipMove(const Expr &target, const Expr &source, const Token &token) {
    requireComplete(target);
    const auto destination = location(target);

    if (destination.type.role != Type::Owner)
      fail(token, "'::' into a field needs an owning field, declared as T* " + target.token.text + "() or T* " +
                      target.token.text + " :: null");

    if (destination.constant) constantTarget(target, token);

    const auto payload = destination.type.pointee();
    const bool literal = source.kind == "int" || source.kind == "real" ||
                         (source.kind == "unary" && source.token.text == "-" && source.args[0]->kind == "int");
    ownerDestination = &source;
    const auto type = expression(source, (literal && numeric(payload)) || source.kind == "initializer"
                                             ? payload : Type(Type::Void));
    ownerDestination = nullptr;

    if (!type.depth) {
      if (!heapValue(type)) fail(token, "heap allocation requires a scalar or container value");

      convert(source, type, payload);
      return;
    }

    if (type.role != Type::Owner || !ownerSource(source))
      fail(token, "ownership transfer requires a mutable owning source");

    require(type.pointee(), payload, token);
    requireOwningResult(source, token);
    const auto moved = pathText(source);

    if (!moved.empty() && pathText(target).rfind(moved + ".", 0) == 0)
      fail(token, "cannot move '" + moved + "' into its own field '" + pathText(target) +
                      "': it would create an ownership cycle");
  }
  void convert(const Expr &e, Type from, Type to) {
    // A tracked reference stored in an alias slot takes a counted reference.
    if (to.role == Type::Alias && from.role != Type::Raw && from.depth && from.pointee() == to.pointee()) {
      if (!trackedPlace(e)) fail(e.token, "tracked aliases require a named source");

      analysis.conversions[&e] = to;
      return;
    }

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
  // How storage was written, for diagnostics: rows[...], box.Items.
  static std::string storageText(const Expr &e) {
    if (e.kind == "index") return storageText(*e.args[0]) + "[...]";
    if (e.kind == ".") return storageText(*e.args[0]) + "." + e.token.text;
    if (e.kind == "unary") return "*" + storageText(*e.args[0]);

    return e.token.text;
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
      fail(e.token, "cannot copy " + typeName(type) + " '" + storageText(e) +
                        "': collections are move-only; pass it by reference (&), or return a local to move it");

    if (arrayValue(type) && symbols.moveOnly(type) && e.addressable() && !analysis.properties.count(&e) &&
        &e != movable)
      fail(e.token, "cannot copy " + typeName(type) + " '" + storageText(e) + "': " +
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
  struct ConstructionSplit {
    std::vector<PartCall> calls;
    std::set<std::string> missing;
    bool exact = true;
  };
  ConstructionSplit chooseSplit(const Expr &e, const std::string &record, const std::string &given, const std::vector<ConstructionSplit> &splits) {
    size_t fewest = splits.front().missing.size();

    for (const auto &split : splits) fewest = std::min(fewest, split.missing.size());

    std::vector<ConstructionSplit> best, exact;

    for (const auto &split : splits)
      if (split.missing.size() == fewest) {
        best.push_back(split);

        if (split.exact) exact.push_back(split);
      }

    if (best.size() > 1 && exact.size() == 1) best = exact;

    auto describe = [&](const ConstructionSplit &split) {
      std::string text;

      for (const auto &call : split.calls)
        if (call.count) text += (text.empty() ? "" : " + ") + constructorText(call.key);

      return text.empty() ? std::string("no arguments") : text;
    };

    if (best.size() > 1)
      fail(e.token, "constructor call " + record + "(" + given + ") is ambiguous between " +
                        describe(best[0]) + " and " + describe(best[1]));

    return best.front();
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
    std::vector<ConstructionSplit> splits;
    ConstructionSplit current;
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

    const auto chosen = chooseSplit(e, record, given, splits);

    for (const auto &call : chosen.calls)
      if (call.count) arguments(e, symbols.at(call.key), 1, call.first, call.count);

    missing = chosen.missing;
    return chosen.calls;
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

    if (const auto *loop = enclosingLoop(); loop && depth < loop->depth)
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
          fail(argument.token, "cannot pass an element of a dynamic array by reference, nor a pair value: "
                               "adding elements can move it; copy it into a local first");

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
      } else if (auto tracked = argument.addressable() && parameter.type.depth ? expression(argument) : Type(Type::Void);
                 tracked.role != Type::Raw) {
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
  // The expression a dotted path starts from, with the rest of the path, as
  // ".a.b", appended to suffix's front.
  static const Expr *dottedRoot(const Expr &e, std::string &suffix) {
    const Expr *root = &e;

    while (root->kind == ".") {
      suffix = "." + root->token.text + suffix;
      root = root->args[0].get();
    }

    return root;
  }
  // The qualified static field a name or dotted path denotes, resolved from
  // the innermost static container outward, or empty. Locals shadow statics.
  std::string staticPath(const Expr &e) {
    std::string path;
    const Expr *root = dottedRoot(e, path);

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
      useReceiver(e.token);
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
    if (e.kind != "." || enumOf(e) || !staticPath(e).empty()) return Type::Void;

    const auto &base = *e.args[0];

    // Static storage holds no collections, except through a named scope.
    if (const auto path = staticPath(base); staticStorage(base) && (path.empty() || symbols.statics.at(path).scope.empty()))
      return Type::Void;

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

    if (gridValue(type) && member != "Len")
      fail(e.token, typeName(type) + " has no member '" + member + "'; a grid has Len, the number of stored cells");

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
  // Checks a sequence or pair literal's entries against its type.
  void checkLiteral(const Expr &e, const Type &type) {
    if (gridValue(type))
      fail(e.token, typeName(type) + " starts empty and has no literal; write cells as g[x][y] = v");

    if (fixedArray(type) && e.args.size() != type.extent)
      fail(e.token, "a literal of type " + typeName(type) + " needs exactly " + std::to_string(type.extent) +
                        " element" + (type.extent == 1 ? "" : "s") + ", got " + std::to_string(e.args.size()));

    for (const auto &entry : e.args) {
      const bool keyed = entry->kind == "entry" || entry->kind == "fieldInitializer";

      if (type.base != Type::Pair) {
        if (keyed)
          fail(entry->token, "a literal of type " + typeName(type) + " lists values, not key : value entries");

        expression(*entry, *type.element);
        continue;
      }

      if (!keyed) fail(entry->token, "a literal of type " + typeName(type) + " lists key : value entries");

      if (entry->kind == "entry") {
        expression(*entry->args[0], *type.key);
        expression(*entry->args[1], *type.element);
        continue;
      }

      // A bare name is the key itself in a name-keyed pair, and a variable
      // holding the key otherwise.
      if (*type.key != Type::Name) expression(*entry->args[1], *type.key);

      expression(*entry->args[0], *type.element);
    }
  }
  // The type of a literal with no expected type: values make a dynamic
  // array of their common type, and key : value entries a pair of the
  // keys' and values' common types; bare-name keys are names.
  Type inferLiteral(const Expr &e) {
    if (e.args.empty())
      fail(e.token, "cannot infer the type of an empty literal {}; declare its type, as in int [] values = {}");

    std::vector<Type> keys, values;
    std::vector<const Expr *> keyAt, valueAt;
    bool keyed = false, plain = false;
    const auto savedAnalysis = analysis;
    const auto savedScopes = scopes;

    for (const auto &entry : e.args) {
      if (entry->kind == "entry") {
        keyed = true;
        keys.push_back(expression(*entry->args[0]));
        keyAt.push_back(entry->args[0].get());
        values.push_back(expression(*entry->args[1]));
        valueAt.push_back(entry->args[1].get());
      } else if (entry->kind == "fieldInitializer") {
        keyed = true;
        keys.push_back(Type::Name);
        keyAt.push_back(entry.get());
        values.push_back(expression(*entry->args[0]));
        valueAt.push_back(entry->args[0].get());
      } else {
        plain = true;
        values.push_back(expression(*entry));
        valueAt.push_back(entry.get());
      }
    }

    analysis = savedAnalysis;
    scopes = savedScopes;

    if (keyed && plain) fail(e.token, "a literal cannot mix values and key : value entries");

    auto unify = [&](const std::vector<Type> &types, const std::vector<const Expr *> &at, const std::string &what) {
      // A collection holds aliases, whichever kind of reference filled it.
      auto stored = [](Type t) {
        if (t.role != Type::Raw) t.role = Type::Alias;

        return t;
      };
      Type common = stored(types.front());

      for (size_t i = 1; i < types.size(); ++i) {
        const auto next = stored(types[i]);

        if (next == common) continue;

        if (numeric(common) && numeric(next)) {
          const auto widened = commonNumeric(common, next);

          if (widened == Type::Void)
            fail(at[i]->token, "the " + what + " of this literal mix signed and unsigned types (" +
                                   typeName(common) + " and " + typeName(next) + "); declare its type");

          common = widened;
          continue;
        }

        fail(at[i]->token, "the " + what + " of this literal have incompatible types, " + typeName(common) +
                               " and " + typeName(next) + "; declare its type");
      }

      return common;
    };
    const auto type = keyed ? Type::collection(Type::Pair, unify(values, valueAt, "values"),
                                               unify(keys, keyAt, "keys"))
                            : Type::array(unify(values, valueAt, "elements"), 0);
    symbols.validateType(type, e.token);
    return type;
  }
  // A mutation of a dynamic array, queue, stack, or pair. a += x appends,
  // q += x adds at a queue's back or a stack's top, p += key : value adds
  // or replaces an entry, and ++ adds a default element (a pair's next
  // integer key). -- removes the last element, the queue's front, the
  // stack's top, or the pair's last added entry; a -= [i] and p -= [key]
  // remove one element. Gives the type of the parenthesized form's result:
  // the inserted index for += and ++, the removed element otherwise.
  Type collectionMutation(const Expr &target, const Type &type, const std::string &op, const Expr *operand,
                          const Token &token) {
    if (fixedArray(type))
      fail(token, typeName(type) + " has a fixed length; '" + op + "' needs a dynamic array (" +
                      typeName(*type.element) + " [])");

    const auto &element = *type.element;

    if (removesCurrent(target, type, op, operand, token)) return element;

    if (gridValue(type)) {
      if (op != "-=" || operand->kind != "removal")
        fail(token, "'" + op + "' does not apply to " + typeName(type) +
                        "; write cells as g[x][y] = v and remove them as g -= [x][y]");

      if (operand->args.size() != type.extent)
        fail(operand->token, typeName(type) + " removes a cell by its " + std::to_string(type.extent) +
                                 " coordinate" + (type.extent == 1 ? "" : "s") + "; got " +
                                 std::to_string(operand->args.size()));

      std::vector<const Expr *> coordinates;

      for (const auto &coordinate : operand->args) coordinates.push_back(coordinate.get());

      gridCoordinates(coordinates);
      return element;
    }

    if (op == "-=" && operand->kind == "removal" && operand->args.size() != 1)
      fail(operand->token, "-= [i][j] removes a cell from a grid; remove from a nested array as a[i] -= [j]");

    const bool pair = type.base == Type::Pair;

    if (op == "+=") {
      if (operand->kind == "entry" && !pair)
        fail(operand->token, "key : value entries belong in a pair, not in " + typeName(type));

      if (pair) {
        if (operand->kind != "entry") fail(operand->token, "add to a pair as p += key : value");

        expression(*operand->args[0], *type.key);
        expression(*operand->args[1], element);
      } else
        expression(*operand, element);

      return Type::Long;
    }

    if (op == "++") {
      if (pair && !integral(*type.key))
        fail(token, "p++ adds the next integer key, but " + typeName(type) + " is keyed by " +
                        typeName(*type.key) + "; add entries as p += key : value");

      return Type::Long;
    }

    if (op == "--") return element;

    if (op == "-=") {
      if (operand->kind != "removal")
        fail(operand->token, pair ? std::string("remove from a pair as p -= [key]")
                                  : "remove from " + typeName(type) + " with " +
                                        (type.base == Type::Array ? "a -= [i] or a--" : "--"));

      if (!pair && type.base != Type::Array)
        fail(operand->token, typeName(type) + " removes only at its end with --; -= [i] needs an array or a pair");

      if (pair) expression(*operand->args[0], *type.key);
      else {
        if (const auto index = expression(*operand->args[0]); !integral(index) && !enumValue(index))
          fail(operand->args[0]->token, "an array index must be an integer, not " + typeName(index));

        expression(*operand->args[0], Type::Long);
      }

      return element;
    }

    fail(token, "'" + op + "' is not supported on " + typeName(type) +
                    "; collections support +=, ++, --, -= [i], and Clear()");
  }
  // A name for the storage a chain of names and fields reaches, the same
  // for every spelling of it; empty for any other expression.
  std::string placeKey(const Expr &e) const {
    if (e.kind == "call" && e.receiver) {
      const auto prefix = placeKey(*e.receiver);
      return prefix.empty() ? "" : prefix + ".GetChildren()";
    }

    if (e.kind == ".") {
      const auto &base = *e.args[0];

      if (base.kind == "name" && base.token.text == "this") return "0:" + e.token.text; // Fields bind in scope 0.

      const auto prefix = placeKey(base);
      return prefix.empty() ? "" : prefix + "." + e.token.text;
    }

    if (e.kind != "name") return "";

    for (size_t i = scopes.size(); i-- > 0;)
      if (scopes[i].count(e.token.text)) return std::to_string(i) + ":" + e.token.text;

    return "static:" + e.token.text;
  }
  // A for-each body changes the length of its collection only by removing
  // the current element, as `c -= x`. Whether op on target does that; any
  // other length change of a collection being iterated is rejected.
  bool removesCurrent(const Expr &target, const Type &type, const std::string &op, const Expr *operand,
                      const Token &token) {
    const auto place = placeKey(target);
    const auto active = std::count_if(iterations.begin(), iterations.end(),
                                      [&](const Iteration &i) { return i.place == place; });

    if (!active || place.empty()) return false;

    const auto *current = op == "-=" && operand->kind == "name" ? findLocal(operand->token) : nullptr;

    if (!current || current->iterating != place)
      fail(token, "'" + op + "' changes '" + target.token.text +
                      "' while a for loop iterates it; the body can only remove the current element, as " +
                      target.token.text + " -= x");

    if (active > 1)
      fail(token, "cannot remove from '" + target.token.text +
                      "' while more than one for loop iterates it: the other loop would lose its place");

    if (type.base == Type::Queue || type.base == Type::Stack)
      fail(token, typeName(type) + " removes only at its end; drain it with while " + target.token.text +
                      ".Len > 0 and -- instead of removing inside a for loop");

    return true;
  }
  // Declares a for-each's variables. Each names the current element's
  // storage rather than a copy of it.
  void bindIteration(const Stmt &s) {
    const auto &collection = *s.expr;
    const auto place = placeKey(collection);

    if (place.empty() || !storage(collection))
      fail(collection.token, "a for loop iterates a named collection or field; store this value in a local first");

    requireComplete(collection);
    const auto source = location(collection);
    const auto type = source.type;

    if (!arrayValue(type) && !collectionValue(type))
      fail(collection.token, "cannot iterate over " + typeName(type) +
                                 "; a for loop takes an array, queue, stack, pair, or grid");

    if (!s.key.empty() && type.base != Type::Pair)
      fail(collection.token, "for k, v : c needs a pair or a grid; " + typeName(type) + " has no keys");

    const Expr *root = &collection;

    while (root->kind == ".") root = root->args[0].get();

    iterations.push_back({place, root->token.text});
    Local value{*type.element, source.constant};
    value.borrowed = true;
    value.growable = !fixedArray(type) || growableElement(collection);
    value.iterating = place;
    auto name = s.token;
    name.text = s.name;
    bind(name, value);

    if (s.key.empty()) return;

    value.type = *type.key;
    value.constant = true;
    name.text = s.key;
    bind(name, value);
  }
  // Checks a range's endpoints and gives their integer type: expected if
  // there is one, else their common type. A literal endpoint takes the type
  // of the other endpoint.
  Type rangeEndpoints(const Expr &e, Type expected) {
    const auto &first = *e.args[0], &second = *e.args[1];
    auto literal = [](const Expr &x) {
      return x.kind == "int" || (x.kind == "unary" && x.token.text == "-" && x.args[0]->kind == "int");
    };
    auto integer = [](const Expr &x, const Type &type) {
      if (!integral(type)) fail(x.token, "a range needs integer endpoints, not " + typeName(type));

      return type;
    };

    if (expected != Type::Void) {
      expression(first, expected);
      expression(second, expected);
      return expected;
    }

    if (literal(first) != literal(second)) {
      const auto &typed = literal(first) ? second : first;
      const auto type = integer(typed, expression(typed));
      expression(literal(first) ? first : second, type);
      return type;
    }

    const auto a = integer(first, expression(first)), b = integer(second, expression(second));
    const auto type = common(a, b, e.token);
    convert(first, a, type);
    convert(second, b, type);
    return type;
  }
  // A grid's coordinates are ints, from int.Min to int.Max.
  void gridCoordinates(const std::vector<const Expr *> &coordinates) {
    for (const auto *coordinate : coordinates) {
      if (const auto type = expression(*coordinate); !integral(type) && !enumValue(type))
        fail(coordinate->token, "a grid coordinate must be an integer, not " + typeName(type));

      expression(*coordinate, Type::Int);
    }
  }
  [[noreturn]] void constantTarget(const Expr &target, const Token &token) const {
    const Expr *root = &target;

    while ((root->kind == "." || root->kind == "index") && !root->args.empty()) root = root->args[0].get();

    fail(token, "cannot modify constant '" + root->token.text + "'");
  }
  // Existing storage: an addressable expression that is not a property.
  bool storage(const Expr &e) {
    if (childrenCall(e)) return true;

    std::string owner;
    // q.Front gives a copy of an element, so q.Front.X reads through that.
    return e.addressable() && !propertyOf(e, owner) && !(e.kind == "index" && pairIndex(e)) &&
           !(e.kind == "." && collectionMember(e) != Type::Void);
  }
  // Whether a local-rooted receiver can be read as storage.
  bool localReceiver(const Expr &e) {
    const Expr *root = &e;

    while (root->kind == "." || root->kind == "index" || (root->kind == "call" && root->receiver))
      root = root->kind == "call" ? root->receiver.get() : root->args[0].get();

    return root->kind == "name" && findLocal(root->token);
  }
  // x.GetChildren() names x's child storage, so reading it copies nothing.
  // A method the container declares with that name keeps priority.
  bool childrenCall(const Expr &e) {
    if (e.kind != "call" || !e.receiver || !e.args.empty() || memberName(e.token.text) != "GetChildren" ||
        !localReceiver(*e.receiver) || !storage(*e.receiver))
      return false;

    auto type = location(*e.receiver).type;

    if (elementValue(type)) return true;
    if (type.depth) type = type.pointee();
    if (!recordValue(type)) return false;

    MemberPath path;
    return symbols.findMethod(type.record, "GetChildren", path) == symbols.end();
  }
  // p[key] on a stored pair, which reads a copy rather than naming storage.
  bool pairIndex(const Expr &e) {
    indexedBases.insert(e.args[0].get());
    const auto &base = *e.args[0];

    if (!storage(base)) return false;
    if (std::vector<const Expr *> coordinates; gridAccess(e, coordinates)) return false;

    return pairValue(location(base).type);
  }
  // grid[x][y]: the stored grid it indexes, with one coordinate per index,
  // outermost first; null when e does not index into a grid.
  const Expr *gridAccess(const Expr &e, std::vector<const Expr *> &coordinates) {
    std::vector<const Expr *> found;
    const Expr *root = &e;

    while (root->kind == "index") {
      found.insert(found.begin(), root->args[1].get());
      root = root->args[0].get();
    }

    if (!storage(*root) || !gridValue(location(*root).type)) return nullptr;

    coordinates = std::move(found);
    return root;
  }
  // Storage inside a dynamic array or a pair, which moves when the
  // collection grows, so it cannot be borrowed by reference or address.
  bool growableElement(const Expr &e) const {
    const Expr *part = &e;

    for (; part->kind == "." || part->kind == "index"; part = part->args[0].get())
      if (part->kind == "index") {
        const auto &holder = analysis.expressionTypes.at(part->args[0].get());

        if (dynamicArray(holder) || collectionValue(holder)) return true;
      }

    if (part->kind != "name") return false;

    for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope)
      if (auto local = scope->find(part->token.text); local != scope->end()) return local->second.growable;

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
  Local childrenLocation(const Expr &e) {
    auto type = location(*e.receiver).type;
    MemberPath path;

    if (!elementValue(type)) {
      if (type.depth) type = type.pointee();

      if (!symbols.findField(type.record, "$children", path))
        fail(e.token, "'" + type.record + "' has no markup children; give it a this{ } block or nest elements in its tag");
    }

    analysis.markupChildren[&e] = path.indices;
    analysis.expressionTypes[&e] = markupType();
    return {markupType(), true};
  }
  Local staticLocation(const Expr &e, const std::string &qualified) {
    Local local{Type::Void, false};
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
    local = {info.scope.empty() ? info.type : info.scopedType, info.constant && &e != initializationTarget};

    if (!info.scope.empty()) analysis.scopedPlaces[&e] = {info.scope, info.scopedType};

    return local;
  }
  bool indexLocation(const Expr &e, bool read, Local &local) {
    indexedBases.insert(e.args[0].get());

    // grid[x][y] names a cell: reading an unwritten one gives the element
    // default, and writing one stores it.
    if (std::vector<const Expr *> coordinates; const auto *grid = gridAccess(e, coordinates)) {
      local = location(*grid, read);
      requireComplete(*grid);
      const auto gridType = local.type;

      if (coordinates.size() != gridType.extent)
        fail(e.token, typeName(gridType) + " takes " + std::to_string(gridType.extent) + " coordinate" +
                          (gridType.extent == 1 ? "" : "s") + ", as g" +
                          (gridType.extent == 1 ? "[x]" : gridType.extent == 2 ? "[x][y]" : "[x][y][z]") +
                          "; got " + std::to_string(coordinates.size()));

      gridCoordinates(coordinates);

      // The partial indexes in between stand for the grid itself.
      for (const Expr *part = e.args[0].get(); part->kind == "index"; part = part->args[0].get())
        analysis.expressionTypes[part] = gridType;

      local.type = *gridType.element;
      analysis.expressionTypes[&e] = local.type;
      return true;
    }

    // a[i] names an element; an index outside the array traps. p[key]
    // names the value stored for key, which must be present.
    const auto &base = *e.args[0];

    if (!storage(base)) fail(e.token, "index a stored array");

    local = location(base, read);
    requireComplete(base);

    if (analysis.markupLookups.count(&base)) {
      expression(*e.args[1], Type::Long);
      analysis.markupIndexed.insert(&e);
      analysis.expressionTypes[&e] = local.type;
      return true;
    }

    if (markupValue(local.type)) {
      expression(*e.args[1], Type::Long);
      local.type = elementType();
      analysis.markupIndexed.insert(&e);
      analysis.expressionTypes[&e] = local.type;
      return true;
    }

    if (!local.type.depth && local.type.base == Type::Pair) {
      expression(*e.args[1], *local.type.key);
      local.type = *local.type.element;
      analysis.expressionTypes[&e] = local.type;
      return true;
    }

    const auto shape = nativeMath(local.type);

    if (!arrayValue(local.type) && !shape)
      fail(e.token, "only arrays and pairs can be indexed; " + typeName(local.type) + " cannot");

    if (const auto index = expression(*e.args[1]); !integral(index) && !enumValue(index))
      fail(e.args[1]->token, "an array index must be an integer, not " + typeName(index));

    expression(*e.args[1], Type::Long);
    local.type = shape ? shape.element() : *local.type.element;
    return false;
  }
  bool memberLocation(const Expr &e, bool read, Local &local) {
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

    if (!symbols.findField(local.type.record, e.token.text, path)) {
      if (const auto child = componentLookup(e, local.type.record); !child.empty()) {
        Type found = Type::Record;
        found.record = child;
        found = found.pointerTo();
        found.role = Type::Owner;
        local.type = found;
        local.constant = true;
        analysis.expressionTypes[&e] = found;
        return true;
      }

      fail(e.token, "unknown container field '" + e.token.text + "'");
    }

    requireAccess(path.field->protectedAccess, path.declaring, e.token.text, e.token);

    if (read && path.field->memberLambda) notFuncValue(e.token);

    analysis.fieldIndices[&e] = path.indices;
    local.type = path.field->scope.empty() ? path.field->type : path.field->scopedType;

    if (!path.field->scope.empty()) analysis.scopedPlaces[&e] = {path.field->scope, local.type};

    return false;
  }
  // An addressable expression denotes existing storage. Dot inherits the
  // mutability of its base; dereferencing borrows the pointed-to storage.
  Local location(const Expr &e, bool read = true) {
    Local local{Type::Void, false};

    if (childrenCall(e)) return childrenLocation(e);

    if (const auto qualified = staticPath(e); !qualified.empty()) {
      local = staticLocation(e, qualified);
    } else if (e.kind == "name") {
      if (std::string owner; !findLocal(e.token) && propertyOf(e, owner)) notStorage(e);

      local = lookup(e.token);

      if (!local.scope.empty()) analysis.scopedPlaces[&e] = {local.scope, local.type};
      if (read && local.memberLambda) notFuncValue(e.token);

      if (read && local.callOnly)
        fail(e.token, "'" + e.token.text + "' holds a lambda that uses this frame's storage; it can only be called here");

      if (local.receiver) analysis.receiverStorage.insert(&e);
      if (read) requireInitialized(e.token);
    } else if (e.kind == "index") {
      if (indexLocation(e, read, local)) return local;
    } else if (e.kind == "unary" && e.token.text == "*") {
      auto ptr = expression(*e.args[0]);

      if (!ptr.depth || ptr == Type::Pointer) fail(e.token, "dereference requires a typed pointer");

      local.type = ptr.pointee();
    } else if (e.kind == ".") {
      if (memberLocation(e, read, local)) return local;
    } else fail(e.token, "operand must be a mutable variable, field, or dereference");

    analysis.expressionTypes[&e] = local.type;

    if (local.type.role != Type::Raw)
      for (auto check = checks.rbegin(); check != checks.rend(); ++check)
        if (check->frame == frames.size() && check->place == placeKey(e)) {
          analysis.checkedUses[&e] = check->statement;
          break;
        }

    return local;
  }
  // Bases of index expressions: an indexed `x.Label[i]` may name a repeated child.
  std::set<const Expr *> indexedBases;
  // x.Label names x's child of container type Label: the type's qualified
  // name, or empty when e is not a component of a container with children.
  std::string componentLookup(const Expr &e, const std::string &record) {
    MemberPath holder;

    if (!symbols.findField(record, "$children", holder)) return "";

    const auto child = resolveRecord(e.token.text, e.token);

    if (child.empty()) return "";

    const auto &owner = symbols.records.at(record);

    if (owner.markupClosed) {
      const auto found = owner.markupChildren.find(child);
      const int count = found == owner.markupChildren.end() ? 0 : found->second;

      if (!count) fail(e.token, "'" + record + "' has no component '" + e.token.text + "'");

      if (count > 1 && !indexedBases.count(&e))
        fail(e.token, "component '" + e.token.text + "' has duplicates in '" + record + "'; choose one with " +
                          e.token.text + "[index]");
    }

    analysis.markupLookups[&e] = {child, holder.indices};
    return child;
  }
  bool heapStorage(const Expr &e) const {
    if (analysis.receiverStorage.count(&e)) return true;
    if (analysis.expressionTypes.at(&e).role != Type::Raw) return true;
    if (analysis.staticFields.count(&e)) return false; // What is left of it is a container name, not storage.

    if (e.kind == "." || e.kind == "index" || (e.kind == "unary" && e.token.text == "*"))
      return heapStorage(*e.args[0]);

    return false;
  }
  [[noreturn]] void notFuncValue(const Token &token) const {
    fail(token, "'" + token.text + "' is a lambda field that runs against its object; call it, as in x." +
                    token.text + "(), instead of reading it");
  }
  // A signature as parameters for arguments().
  static Signature shapeOf(const Type &func, const Token &token) {
    Signature shape;
    shape.result = func.signature->result;

    for (const auto &parameter : func.signature->parameters) {
      Parameter p;
      p.name = token;
      p.type = parameter.type;
      p.reference = parameter.reference;
      shape.parameters.push_back(p);
    }

    return shape;
  }
  Type indirectCall(const Expr &e, const Type &type, FunctionAnalysis::IndirectCall call) {
    const auto shape = shapeOf(type, e.token);

    if (shape.parameters.size() != e.args.size())
      fail(e.token, "wrong argument count for a call through " + typeName(type));

    arguments(e, shape, 0);
    call.type = type;
    analysis.indirectCalls[&e] = std::move(call);
    return shape.result;
  }
  // Binds every field an instance method can name directly.
  void bindVisibleFields() {
    for (const auto &path : symbols.visibleFields(container)) {
      Local field{path.field->scope.empty() ? path.field->type : path.field->scopedType, false, true, true};
      field.memberLambda = path.field->memberLambda;
      field.scope = path.field->scope;
      bind(path.field->name, field);
    }
  }
  // A global or static function named where a value is expected, or Void.
  Type functionValue(const Expr &e) {
    std::string path;
    const Expr *root = dottedRoot(e, path);

    if (root->kind != "name" || findLocal(root->token) || !staticPath(e).empty()) return Type::Void;

    path = root->token.text + path;
    const auto found = resolve(path, e.token);

    if (found == symbols.end() || found->second.method) return Type::Void;

    const auto &signature = found->second;

    if (!signature.nativeSymbol.empty())
      fail(e.token, "native function '" + path + "' cannot be a func value; pass &" + path + " as a callback");

    if (signature.result.role != Type::Raw)
      fail(e.token, "a function that returns ownership cannot be a func value");

    requireAccess(found, e.token);
    auto shape = std::make_shared<FuncSignature>();
    shape->result = signature.result;

    for (const auto &parameter : signature.parameters) shape->parameters.push_back({parameter.type, parameter.reference});

    Type type(Type::Func);
    type.signature = shape;
    analysis.functionValues[&e] = found->first;
    return type;
  }
  Type lambdaExpression(const Expr &e) {
    const auto &f = *e.lambda;
    Frame frame{std::move(scopes), std::move(loops), std::move(iterations), result,
                ownerDestination, incompleteAllowed, movable, scopedLambda, {}};
    const bool bound = &e == memberLambdaSite;
    frames.push_back(std::move(frame));
    scopes.clear();
    loops.clear();
    iterations.clear();
    result = f.result;
    ownerDestination = incompleteAllowed = movable = scopedLambda = nullptr;
    scopes.emplace_back(); // Captures, or the receiver's fields of a member lambda.

    if (bound) {
      frames.back().info.bound = true;
      frames.back().info.usesThis = true;
      frames.back().info.record = container;
      bindVisibleFields();
      scopes.emplace_back();
      Token self = f.name;
      self.text = "this";
      Type record = Type::Record;
      record.record = container;
      bind(self, {record, false, true, true, true});
    }

    scopes.emplace_back();
    symbols.validateType(result, f.name);

    for (const auto &parameter : f.params) {
      symbols.validateType(parameter.type, parameter.name);
      bind(parameter.name, {parameter.type, false, true, false, parameter.reference});
    }

    const bool terminated = sequence(f.body);

    if (!terminated) requireCompleteScopes(0);
    if (!terminated && result != Type::Void) fail(e.token, "not all paths of this lambda return a value");

    auto info = std::move(frames.back().info);
    scopes = std::move(frames.back().scopes);
    loops = std::move(frames.back().loops);
    iterations = std::move(frames.back().iterations);
    result = frames.back().result;
    ownerDestination = frames.back().ownerDestination;
    incompleteAllowed = frames.back().incompleteAllowed;
    movable = frames.back().movable;
    scopedLambda = frames.back().scopedLambda;
    frames.pop_back();

    if (info.borrowed) {
      if (&e != scopedLambda)
        fail(e.token, "this lambda uses 'this' or a reference parameter, so it cannot outlive the call; "
                      "store it directly in a local and call it there");

      scopedLambdaBorrowed = true;
    }

    analysis.lambdas[&e] = std::move(info);
    return e.castType;
  }
  // The container a reflection path such as this.Example names, or empty.
  std::string reflectedContainer(const Expr &path) {
    if (path.kind != ".") return "";

    const auto &base = *path.args[0];
    auto record = reflectedContainer(base);

    if (record.empty()) {
      if (!storage(base)) return "";

      auto type = location(base).type;

      if (type.depth) type = type.pointee();
      if (!recordValue(type)) return "";

      record = type.record;
    }

    MemberPath field;

    if (symbols.findField(record, path.token.text, field) && !field.field->parentView) return "";

    auto named = [&](const std::string &candidate) {
      return candidate == path.token.text ||
             (candidate.size() > path.token.text.size() &&
              candidate.compare(candidate.size() - path.token.text.size() - 1, std::string::npos,
                                "." + path.token.text) == 0);
    };

    if (named(record)) return record;

    std::function<std::string(const std::string &)> ancestor = [&](const std::string &current) -> std::string {
      for (const auto &parent : symbols.records.at(current).parents) {
        if (named(parent.text)) return parent.text;
        if (auto found = ancestor(parent.text); !found.empty()) return found;
      }

      return "";
    };
    return ancestor(record);
  }
  // this.Example.GetName(), this.Example.Money.GetName(), .GetTags(), and
  // .GetDeclaringName(). Nothing on the path is evaluated.
  bool reflectionCall(const Expr &e, Type expected, Type &type) {
    const auto query = memberName(e.token.text);

    if (query != "GetName" && query != "GetTags" && query != "GetDeclaringName") return false;
    if (!e.receiver || e.receiver->kind != ".") return false;

    const Expr *root = e.receiver.get();

    while (root->kind == ".") root = root->args[0].get();
    if (root->kind != "name" || !findLocal(root->token)) return false;

    FunctionAnalysis::Reflection reflection;
    reflection.query = query;
    const auto &path = *e.receiver;
    auto owner = reflectedContainer(*path.args[0]);
    const TypeMember *member = nullptr;
    std::vector<TypeMember> members;

    if (!owner.empty()) {
      members = symbols.members(owner);

      for (size_t i = 0; i < members.size(); ++i)
        if (members[i].name == path.token.text) {
          member = &members[i];
          reflection.member = long(i);
        }
    }

    if (!member) {
      owner = reflectedContainer(path);

      if (owner.empty()) return false;

      // A parent's own GetName method stays callable through its view.
      MemberPath own;

      if (symbols.findMethod(owner, query, own) != symbols.end()) return false;
    }

    if (!e.args.empty()) fail(e.token, query + " takes no arguments");

    reflection.record = owner;

    if (member) requireAccess(member->isProtected, member->declaring, member->name, e.token);

    if (query != "GetTags") {
      if (query == "GetDeclaringName" && !member) fail(e.token, "GetDeclaringName applies to a member, not a container");

      type = expected == Type::Name ? Type(Type::Name) : Type(Type::String);
      analysis.reflections[&e] = std::move(reflection);
      return true;
    }

    if (!member) fail(e.token, "GetTags applies to a field or method, not a container");

    // The expected element type selects the attributes; without one, they
    // must all share an underlying type.
    Type element = dynamicArray(expected) ? *expected.element : Type(Type::Void);

    if (expected != Type::Void && !dynamicArray(expected))
      fail(e.token, "GetTags gives a dynamic array, not " + typeName(expected));

    for (size_t i = 0; i < member->tags.size(); ++i) {
      const auto &tag = member->tags[i];
      Type tagType = Type::Enum;
      tagType.record = tag.type;
      const auto underlying = symbols.enums.at(tag.type).type;

      if (element == Type::Void) element = underlying;
      else if (expected == Type::Void && element != underlying)
        fail(e.token, "the attributes of '" + member->name + "' have different underlying types; "
                      "declare the result, as in " + typeName(underlying) + " [] tags = ...");

      if (element == underlying || element == tagType) reflection.tags.push_back(i);
    }

    if (element == Type::Void) element = Type::UInt8;

    if (!enumValue(element) && !integral(element) && !floating(element) && !managedText(element))
      fail(e.token, "GetTags cannot give " + typeName(element) + " elements");

    reflection.element = element;
    type = Type::array(element, 0);
    analysis.reflections[&e] = std::move(reflection);
    return true;
  }
  // `e..Foo(...)`, `e..?Foo(...)`, and `e..?Foo`. The site's shape comes from
  // the methods its receiver could dispatch to when they agree; otherwise
  // from the arguments. The runtime lookup compares that shape, so a method
  // of the right name but a different signature counts as absent.
  Type duckCall(const Expr &e, Type expected) {
    const auto &base = *e.receiver;
    requireComplete(base);
    Type type;
    bool constant = false;

    if (storage(base)) {
      const auto local = location(base);
      type = local.type;
      constant = !type.depth && local.constant;
    } else
      type = expression(base);

    const bool element = elementValue(type);

    if (type.depth && !element) type = type.pointee();
    if (!element && !recordValue(type)) fail(base.token, "a duck call needs a container receiver, not " + typeName(type));

    FunctionAnalysis::DuckCall call;
    call.record = type;

    if (e.kind == "duckExists") {
      analysis.duckCalls[&e] = std::move(call);
      return Type::Bool;
    }

    if (constant && e.kind == "duckCall") fail(base.token, "cannot call a mutable method on a constant");

    std::vector<const Signature *> candidates;

    for (const auto &[key, signature] : symbols) {
      if (!dispatched(key, signature.method) || signature.protectedAccess || memberName(key) != e.token.text ||
          signature.parameters.size() != e.args.size() + 1)
        continue;

      const auto owner = key.substr(0, key.rfind('.'));

      if (element || owner == type.record || symbols.derivesFrom(owner, type.record) || symbols.derivesFrom(type.record, owner))
        candidates.push_back(&signature);
    }

    Signature shape;

    for (size_t i = 0; i < e.args.size(); ++i) {
      bool agree = !candidates.empty();

      for (const auto *candidate : candidates)
        agree = agree && candidate->parameters[i + 1].type == candidates.front()->parameters[i + 1].type &&
                candidate->parameters[i + 1].reference == candidates.front()->parameters[i + 1].reference;

      Parameter parameter;
      const auto savedAnalysis = analysis;
      const auto savedScopes = scopes;

      // An argument that cannot convert is not a compile error: the call
      // keeps its own shape, and the runtime lookup reports the mismatch.
      if (agree && !candidates.front()->parameters[i + 1].reference) {
        try {
          expression(*e.args[i], candidates.front()->parameters[i + 1].type);
        } catch (const std::runtime_error &) {
          agree = false;
        }

        analysis = savedAnalysis;
        scopes = savedScopes;
      }

      if (agree) parameter = candidates.front()->parameters[i + 1];
      else {
        parameter.name = e.args[i]->token;
        parameter.type = expression(*e.args[i]);
        analysis = savedAnalysis;
        scopes = savedScopes;

        if (parameter.type == Type::Void) fail(e.args[i]->token, "a duck call argument must produce a value");
      }

      shape.parameters.push_back(parameter);
    }

    arguments(e, shape, 0);
    bool agree = !candidates.empty();

    for (const auto *candidate : candidates) agree = agree && candidate->result == candidates.front()->result;

    if (agree) call.result = candidates.front()->result;
    else if (expected != Type::Void) call.result = expected;
    else if (candidates.empty()) call.result = Type::Void;
    else fail(e.token, "methods named '" + e.token.text + "' disagree on their result; declare the type you expect");

    if (call.result.role != Type::Raw) fail(e.token, "a duck call cannot receive ownership");

    call.parameters = shape.parameters;
    call.signature = signatureText(call.result, call.parameters, 0);
    const auto produced = e.kind == "duckCheck" ? Type(Type::Bool) : call.result;
    analysis.duckCalls[&e] = std::move(call);
    return produced;
  }
  Type checkRecordInitializer(const Expr &e, const Type &expected) {
    for (const auto &field : e.args)
      if (field->kind != "fieldInitializer")
        fail(field->token, "a '" + expected.record + "' literal names its fields, as in {X: 1}");

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
  Type checkConstruct(const Expr &e) {
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
  Type checkCast(const Expr &e) {
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
  Type checkDecimalLiteral(const Expr &e, const Type &expected) {
    const auto &op = e.token.text;
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
  Type checkFloatingLiteral(const Expr &e, const Type &expected) {
    const auto &op = e.token.text;
    Type type = expected == Type::Double ? Type::Double : Type::Float;
    double value = 0;

    if (!parseFloatingLiteral(op, type == Type::Float, value))
      fail(e.token, "invalid or out-of-range floating literal");

    analysis.floatingConstants[&e] = floatingConstantText(value);
    return type;
  }
  Type checkIntegerLiteral(const Expr &e, const Type &expected, bool negativeLiteral) {
    const auto &op = e.token.text;
    const auto &digits = negativeLiteral ? e.args[0]->token.text : op;
    const auto base = integerLiteralBase(digits);
    const auto magnitude = base == 10 ? digits : digits.substr(2);

    if (magnitude.empty())
      fail(e.token, "integer literal requires digits after its prefix");

    // Validate before conversion so malformed huge literals do not hide
    // invalid digits behind an overflow diagnostic.
    for (char digit : magnitude) {
      const int value = digitValue(digit);

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
  Type checkGuardedDuckCall(const Expr &e, const Type &expected) {
    // ?e..Foo() skips a missing method, and reads as the result type's default.
    const auto type = duckCall(*e.args[0], expected);
    analysis.expressionTypes.emplace(e.args[0].get(), type);

    if (recordValue(type) && symbols.runtimeDefaults(type.record))
      fail(e.token, "?.. cannot read '" + type.record + "': its default is built at run time; check e..?Foo() first");

    return type;
  }
  Type checkGuardedCall(const Expr &e) {
    // ?p.q.Foo(x) runs only when every link is valid.
    const auto &call = *e.args[0];
    const auto type = expression(call);
    const auto target = analysis.callTargets.find(&call);
    const auto indirect = analysis.indirectCalls.find(&call);

    if (!(target != analysis.callTargets.end() && symbols.at(target->second).method) &&
        !(indirect != analysis.indirectCalls.end() && indirect->second.field))
      fail(e.token, "'?' guards a method called through a reference, such as ?p.Foo(); "
                    "this call has no reference to check");

    if (recordValue(type) && symbols.runtimeDefaults(type.record))
      fail(e.token, "'?' cannot read '" + type.record + "': its default is built at run time; check the reference with if first");

    return type;
  }
  Type checkGuardedRead(const Expr &e) {
    // ?a[i] and ?p[key] read the element, or the element type's default
    // when the index is outside the array or the key is missing.
    const auto type = expression(*e.args[0]);

    if (e.args[0]->kind != "index" && !analysis.scopedPlaces.count(e.args[0].get()))
      fail(e.token, "'?' guards an index such as ?a[i], a method call such as ?p.Foo(), a duck call such as ?e..Foo(), or a scoped variable");

    if (e.args[0]->kind != "index") return type;

    if (recordValue(type) && symbols.runtimeDefaults(type.record))
      fail(e.token, "?" + e.args[0]->args[0]->token.text + "[...] cannot read '" + type.record +
                        "': its default is built at run time (constructors or computed defaults); "
                        "check IsValid first");

    return type;
  }
  Type checkMutationResult(const Expr &e) {
    const auto &op = e.token.text;
    // (s += x) gives the inserted index; (s--) and (s -= [i]) give the
    // removed element.
    const auto &target = *e.args[0];

    if (!storage(target)) fail(e.token, "'" + op + "' needs a stored collection");

    requireComplete(target);
    const auto local = location(target);

    if (!arrayValue(local.type) && !collectionValue(local.type))
      fail(e.token, "only collection mutations give a result; '" + op + "' on " + typeName(local.type) +
                        " is a statement");

    if (local.constant) constantTarget(target, e.token);

    const auto mutated = collectionMutation(target, local.type, op, e.args.size() > 1 ? e.args[1].get() : nullptr, e.token);

    if (trackedAlias(mutated))
      fail(e.token, "a removed alias is released, not returned; read the element first, then remove it");

    return mutated;
  }
  Type checkAddressOf(const Expr &e) {
    const auto &arg = *e.args[0];
    requireComplete(arg);
    std::string suffix;
    const Expr *root = dottedRoot(arg, suffix);

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
      fail(arg.token, "cannot take the address of a dynamic array or pair element: adding elements can move it");

    if (local.constant || managedValue(local.type))
      fail(arg.token, "cannot take the address of constant or managed text storage");

    return local.type.pointerTo();
  }
  Type checkCall(const Expr &e, const Type &expected) {
    const auto &op = e.token.text;

    // `Gun g(5)` and `Gun* p(5)` arrive with the constructed type resolved.
    if (nativeMath(e.castType)) return checkNativeConstruct(e);
    if (recordValue(e.castType)) return construction(e, e.castType.record);

    if (const auto dot = op.find('.'); dot != std::string::npos && !e.receiver && nativeMath(op.substr(0, dot)))
      return checkNativeStatic(e, op.substr(0, dot), op.substr(dot + 1));

    if (op.rfind("Math.", 0) == 0) return checkMathCall(e, expected);

    if (childrenCall(e))
      fail(e.token, "GetChildren() is a view of stored children; use its Len, index it, or loop over it");

    if (Type reflected; reflectionCall(e, expected, reflected)) return reflected;

    if (e.receiver && localReceiver(*e.receiver) && storage(*e.receiver) && elementValue(location(*e.receiver).type)) {
      if (memberName(op) != "GetName" || !e.args.empty())
        fail(e.token, "an element from GetChildren() has GetName() and GetChildren(); use .. to call its methods");

      analysis.elementNames.insert(&e);
      return Type::Text;
    }

    // `g(w)` on a container local supplies its missing parts; on a
    // complete one, or on a pointer to one, it reinitializes the object.
    if (auto handled = checkLocalCallee(e)) return *handled;
    if (auto handled = checkBuiltinCall(e)) return *handled;

    auto found = resolve(op, e.token);

    // Inside an instance method an unqualified name finds the container's
    // own or inherited methods before static or global functions.
    if (method && !e.receiver && op.find('.') == std::string::npos) {
      MemberPath path;
      auto member = symbols.findMethod(container, op, path);

      if (member != symbols.end()) {
        found = member;
        useReceiver(e.token);

        if (!path.indices.empty()) analysis.receiverPaths[&e] = {container, path.indices};
      }
    }

    bool instance = false;

    if (e.receiver) {
      const Expr *root = e.receiver.get();

      while (root->kind == ".") root = root->args[0].get();

      instance = root->kind != "name" || findLocal(root->token) || staticStorage(*e.receiver);

      if (instance)
          if (auto handled = checkInstanceCall(e, found)) return *handled;
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
      fail(e.token, "receive an owning function result with : or ::");

    if (signature.method && !instance && (!method || e.receiver))
      fail(e.token, "instance method requires a receiver");

    const size_t offset = signature.method ? 1 : 0;

    if (signature.parameters.size() != e.args.size() + offset)
      fail(e.token, "wrong argument count for '" + op + "'");

    arguments(e, signature, offset);
    return signature.result;
  }
  static bool numberLiteral(const Expr &e) {
    const auto &value = e.kind == "unary" && e.token.text == "-" ? *e.args[0] : e;
    return value.kind == "int" || value.kind == "real";
  }
  // The scalar a literal should be parsed as beside a native vector or matrix.
  static Type nativeLiteralHint(const Expr &e, const NativeMath &shape) {
    if (!numberLiteral(e)) return Type::Void;

    const auto &literal = e.kind == "unary" ? *e.args[0] : e;
    const Type scalar = shape.scalar;
    return (literal.kind == "real" ? floating(scalar) : integral(scalar)) ? scalar : Type(Type::Void);
  }
  // vec3(x, y, z), vec3(s), vec4(v, w), vec3(other), mat3(s), mat3(a, b, c),
  // mat3(nine scalars) and mat3(other). A constructor is the explicit way to
  // change a vector's scalar type; lone scalars still convert only implicitly.
  Type checkNativeConstruct(const Expr &e) {
    const auto type = e.castType;
    const auto shape = nativeMath(type);
    const auto total = shape.columns * shape.rows;
    const Type scalar = shape.scalar;
    const bool single = e.args.size() == 1;
    unsigned supplied = 0;
    bool whole = e.args.empty();

    for (const auto &argument : e.args) {
      const auto given = decay(*argument, expression(*argument, nativeLiteralHint(*argument, shape)));
      const auto source = nativeMath(given);

      if (!source) {
        if (!numeric(given)) fail(argument->token, typeName(type) + " requires numbers or vectors, got " + typeName(given));

        convert(*argument, given, scalar);
        supplied += 1;
        whole = whole || single;
      } else if (source.matrix()) {
        if (!single || !shape.matrix()) fail(argument->token, "a matrix can only be passed alone to a matrix constructor");

        whole = true;
      } else {
        if (shape.matrix() && source.rows != shape.rows)
          fail(argument->token, typeName(type) + " takes columns of " + std::to_string(shape.rows) + " components, got " + typeName(given));

        supplied += source.rows;
        whole = whole || (single && !shape.matrix() && source.rows >= shape.rows);
      }
    }

    if (!whole && supplied != total)
      fail(e.token, typeName(type) + " needs " + std::to_string(total) + " components, got " + std::to_string(supplied));

    return type;
  }
  // vec3.Identity(), vec3.Zero(), vec3.One(), mat4.Identity() and mat4.Zero().
  Type checkNativeStatic(const Expr &e, const std::string &record, const std::string &name) {
    Type type(Type::Record);
    type.record = record;

    if (name != "Identity" && name != "Zero" && (name != "One" || nativeMath(record).matrix()))
      fail(e.token, record + " has no function '" + name + "'");

    if (!e.args.empty()) fail(e.token, "wrong argument count for '" + e.token.text + "'");

    analysis.nativeMethods.insert(&e);
    return type;
  }
  // Every method returns its result; only Normalize also changes the receiver.
  Type checkNativeMethod(const Expr &e, const Type &type) {
    const auto shape = nativeMath(type);
    const auto name = memberName(e.token.text);
    const Type scalar = shape.scalar;
    const bool fractional = floating(scalar);
    Type result = Type::Void;
    size_t count = 0;

    if (shape.matrix()) {
      if (name == "Transpose" || name == "Inverse") result = type;
      else if (name == "Determinant") result = scalar;
    } else if (name == "Abs") result = type;
    else if (name == "Min" || name == "Max") { result = type; count = 1; }
    else if (fractional) {
      if (name == "Length" || name == "LengthSquared") result = scalar;
      else if (name == "Normalize") result = type;
      else if (name == "Dot" || name == "Distance") { result = scalar; count = 1; }
      else if (name == "Cross" && shape.rows == 3) { result = type; count = 1; }
      else if (name == "Lerp") { result = type; count = 2; }
    }

    if (result == Type::Void) fail(e.token, typeName(type) + " has no method '" + name + "'");
    if (e.args.size() != count) fail(e.token, "wrong argument count for '" + name + "'");

    analysis.nativeMethods.insert(&e);

    if (count) {
      const auto other = expression(*e.args[0]);

      if (other != type) fail(e.args[0]->token, name + " requires " + typeName(type) + ", got " + typeName(other));
    }

    if (count == 2) {
      const auto amount = decay(*e.args[1], expression(*e.args[1], nativeLiteralHint(*e.args[1], shape)));

      if (!numeric(amount)) fail(e.args[1]->token, name + " requires a number, got " + typeName(amount));

      convert(*e.args[1], amount, scalar);
    }

    return result;
  }
  Type nativeOperation(const std::string &op, const Token &token, const Type &first, const Type &second,
                       const Expr *left, const Expr &right) {
    const auto a = nativeMath(first), b = nativeMath(second);
    auto refuse = [&]() { fail(token, "operator '" + op + "' does not apply to " + typeName(first) + " and " + typeName(second)); };
    const bool equality = op == "==" || op == "!=";

    if (!equality && op != "+" && op != "-" && op != "*" && op != "/") refuse();

    if (a && b) {
      if (first == second) {
        if (a.matrix() && op == "/") refuse();

        return equality ? Type(Type::Bool) : first;
      }

      if (op == "*" && a.matrix() && !b.matrix() && a.scalar == b.scalar && a.rows == b.rows) return second;

      refuse();
    }

    const auto &shape = a ? a : b;

    if (equality || !numeric(a ? second : first) || (shape.matrix() && (op == "+" || op == "-" || (!a && op == "/"))))
      refuse();

    convert(a ? right : *left, a ? second : first, shape.scalar);
    return a ? first : second;
  }
  // Math.Name(...): the compiler's own namespace. Every operand takes one
  // type, and a function that needs a fraction turns integers into one.
  Type checkMathCall(const Expr &e, const Type &expected) {
    enum class Domain { Real, Number, Vector, Vector3 };
    struct Entry {
      size_t arguments;
      Domain domain;
    };
    static const std::map<std::string, Entry> functions{
        {"Sqrt", {1, Domain::Real}},    {"Cbrt", {1, Domain::Real}},     {"Exp", {1, Domain::Real}},
        {"Exp2", {1, Domain::Real}},    {"Log", {1, Domain::Real}},      {"Log2", {1, Domain::Real}},
        {"Log10", {1, Domain::Real}},   {"Sin", {1, Domain::Real}},      {"Cos", {1, Domain::Real}},
        {"Tan", {1, Domain::Real}},     {"Asin", {1, Domain::Real}},     {"Acos", {1, Domain::Real}},
        {"Atan", {1, Domain::Real}},    {"Sinh", {1, Domain::Real}},     {"Cosh", {1, Domain::Real}},
        {"Tanh", {1, Domain::Real}},    {"Floor", {1, Domain::Real}},    {"Ceil", {1, Domain::Real}},
        {"Round", {1, Domain::Real}},   {"Trunc", {1, Domain::Real}},    {"Pow", {2, Domain::Real}},
        {"Atan2", {2, Domain::Real}},   {"Hypot", {2, Domain::Real}},    {"Mod", {2, Domain::Real}},
        {"Lerp", {3, Domain::Real}},    {"Abs", {1, Domain::Number}},    {"Sign", {1, Domain::Number}},
        {"Min", {2, Domain::Number}},   {"Max", {2, Domain::Number}},    {"Clamp", {3, Domain::Number}},
        {"Dot", {2, Domain::Vector}},   {"Length", {1, Domain::Vector}}, {"Distance", {2, Domain::Vector}},
        {"Normalize", {1, Domain::Vector}}, {"Cross", {2, Domain::Vector3}}};
    const auto &op = e.token.text;
    const auto found = functions.find(op.substr(5));

    if (found == functions.end()) fail(e.token, "the Math namespace has no function '" + op.substr(5) + "'");
    if (e.args.size() != found->second.arguments) fail(e.token, "wrong argument count for '" + op + "'");

    analysis.mathCalls.insert(&e);
    const auto domain = found->second.domain;

    if (domain == Domain::Vector || domain == Domain::Vector3) return checkMathVectors(e, domain == Domain::Vector3);

    // Named operands decide the type first, so a literal beside a double is
    // parsed at double precision wherever it is written.
    std::vector<Type> types(e.args.size());
    Type target = Type::Void;
    auto merge = [&](size_t i, Type type) {
      if (!numeric(type)) fail(e.args[i]->token, op + " requires numbers, got " + typeName(type));

      types[i] = type;
      target = target == Type::Void ? type : common(target, type, e.token);
    };

    for (size_t i = 0; i < e.args.size(); ++i)
      if (!numberLiteral(*e.args[i]))
        merge(i, decay(*e.args[i], expression(*e.args[i], floating(expected) ? expected : Type(Type::Void))));

    const Type wanted = target != Type::Void ? target : numeric(expected) ? expected : Type(Type::Void);

    for (size_t i = 0; i < e.args.size(); ++i) {
      if (!numberLiteral(*e.args[i])) continue;

      const auto &literal = e.args[i]->kind == "unary" ? *e.args[i]->args[0] : *e.args[i];
      const bool fits = literal.kind == "real" ? floating(wanted) : integral(wanted);
      merge(i, expression(*e.args[i], fits ? wanted : Type(Type::Void)));
    }

    if (domain == Domain::Real && !floating(target)) target = floating(expected) ? expected : Type(Type::Double);

    for (size_t i = 0; i < e.args.size(); ++i) convert(*e.args[i], types[i], target);

    return target;
  }
  // Duck has no vector type: a vector is any container whose fields are
  // exactly X, Y and optionally Z and W, all of one floating type.
  Type checkMathVectors(const Expr &e, bool threeOnly) {
    const auto &op = e.token.text;
    static const std::vector<std::string> names{"X", "Y", "Z", "W"};
    const auto first = expression(*e.args[0]);

    if (!recordValue(first)) fail(e.args[0]->token, op + " requires a vector container, got " + typeName(first));

    for (size_t i = 1; i < e.args.size(); ++i) {
      const auto other = expression(*e.args[i]);

      if (!(other == first))
        fail(e.args[i]->token, op + " requires both vectors to be '" + typeName(first) + "', got " + typeName(other));
    }

    const auto &record = symbols.records.at(first.record);
    const auto &fields = record.fields;
    bool vector = record.parents.empty() && fields.size() >= 2 && fields.size() <= 4 && floating(fields[0].type);

    for (size_t i = 0; vector && i < fields.size(); ++i)
      vector = fields[i].name.text == names[i] && fields[i].type == fields[0].type;

    if (!vector)
      fail(e.args[0]->token, op + " requires a container with only the fields X, Y and optionally Z and W, "
                                  "all float or all double; '" + first.record + "' does not qualify");

    if (threeOnly && fields.size() != 3) fail(e.args[0]->token, op + " requires a container with the fields X, Y and Z");

    for (const auto &field : fields) {
      MemberPath path;
      symbols.findField(first.record, field.name.text, path);
      requireAccess(path.field->protectedAccess, path.declaring, field.name.text, e.token);
    }

    return memberName(op) == "Cross" || memberName(op) == "Normalize" ? first : fields[0].type;
  }
  std::optional<Type> checkLocalCallee(const Expr &e) {
    const auto &op = e.token.text;

    if (!e.receiver && op.find('.') == std::string::npos)
      if (const auto *local = findLocal(e.token)) {
        if (funcValue(local->type)) {
          requireInitialized(e.token);
          FunctionAnalysis::IndirectCall call;

          if (local->receiver) {
            MemberPath field;
            symbols.findField(container, e.token.text, field);
            call.field = true;
            call.owner = container;
            call.path = field.indices;
          }

          const auto type = local->type;
          return indirectCall(e, type, call);
        }

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

    return std::nullopt;
  }
  std::optional<Type> checkBuiltinCall(const Expr &e) {
    const auto &op = e.token.text;

    if (op == "Length" || op == "CodeUnitAt") {
      if (e.args.size() != (op == "Length" ? 1u : 2u))
        fail(e.token, "wrong argument count for '" + op + "'");

      auto text = expression(*e.args[0]);

      if (!managedText(text)) fail(e.token, op + " requires string or name");
      if (op == "CodeUnitAt") expression(*e.args[1], Type::Long);

      return op == "Length" ? Type::Long : Type::UInt32;
    }

    if (op == "IsValid" && !e.receiver) {
      // IsValid(a[i]) checks the index without reading the element.
      if (e.args.size() != 1 || e.args[0]->kind != "index")
        fail(e.token, "IsValid expects one indexed element, as in IsValid(a[i])");

      expression(*e.args[0]);
      return Type::Bool;
    }

    if (op == "Exec" && !e.receiver) {
      // The call target also makes Exec a barrier for task scheduling.
      if (const auto *script = symbols.execTarget(e)) analysis.callTargets[&e] = *script;

      return Type::Void;
    }

    // Parts of a lowered T assertion. A value Print cannot show is left out
    // of the report rather than rejected: such values can still be compared.
    if (op == "$T.fail") return Type::Void;

    if (op == "$T.show") {
      expression(*e.args[0]);
      return Type::Void;
    }

    if (op == "Print") {
      if (e.args.size() != 1)
        fail(e.token, "Print expects one argument");

      auto printed = expression(*e.args[0]);

      if (printed == Type::Void)
        fail(e.token, "cannot print void");

      if (recordValue(printed) && !nativeMath(printed)) fail(e.token, "cannot print a container");

      if (arrayValue(printed) || collectionValue(printed))
        fail(e.token, "cannot print " + typeName(printed) + " directly; print its elements");

      if (printed.depth)
        fail(e.token, "cannot print an opaque pointer; compare it with null instead");

      if (funcValue(printed)) fail(e.token, "cannot print a func value; call it instead");

      return Type::Void;
    }

    return std::nullopt;
  }
  std::optional<Type> checkInstanceCall(const Expr &e, Symbols::const_iterator &found) {
    const auto &op = e.token.text;
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
    if (nativeMath(t)) {
      const auto result = checkNativeMethod(e, t);

      // Normalize updates what it was called on and also returns it. A
      // temporary has nothing to update, so it only gives the result.
      if (memberName(op) == "Normalize" && stored) {
        if (constant) fail(base.token, "cannot call a mutable method on a constant");

        analysis.nativeInPlace.insert(&e);
      }

      return result;
    }
    if (stored && !t.depth && (arrayValue(t) || collectionValue(t))) return checkCollectionClear(e, base, t, constant);
    if (!recordValue(t)) fail(base.token, "method receiver requires a container");

    MemberPath path;
    const auto name = op.substr(op.rfind('.') + 1);
    found = symbols.findMethod(t.record, name, path);

    if (found == symbols.end() && symbols.findField(t.record, name, path) && funcValue(path.field->type)) {
      requireAccess(path.field->protectedAccess, path.declaring, name, e.token);
      FunctionAnalysis::IndirectCall call;
      call.field = true;
      call.owner = t.record;
      call.path = path.indices;
      const auto callee = path.field->type;
      return indirectCall(e, callee, call);
    }

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

    return std::nullopt;
  }
  Type checkCollectionClear(const Expr &e, const Expr &base, const Type &t, bool constant) {
    const auto &op = e.token.text;

    // s.Clear() removes every element and frees the storage.
    if (memberName(op) != "Clear")
      fail(e.token, typeName(t) + " has no method '" + memberName(op) + "'; collections have Clear()");

    if (!e.args.empty()) fail(e.token, "Clear takes no arguments");

    if (fixedArray(t))
      fail(e.token, typeName(t) + " has a fixed length; Clear() needs a dynamic array (" +
                        typeName(*t.element) + " [])");

    if (constant) constantTarget(base, e.token);

    removesCurrent(base, t, "Clear", nullptr, e.token);
    return Type::Void;
  }
  Type checkConditional(const Expr &e, const Type &expected, const Type &first) {
    truth(*e.args[0], first, e.token);
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
  Type checkBinary(const Expr &e, const Type &expected, Type first) {
    const auto &op = e.token.text;
    const Type hint = binaryOperandHint(e, expected, first);
    auto second = decay(*e.args[1], expression(*e.args[1], hint));

    if (e.args.size() == 3) checkTolerance(e, first, second);

    if (nativeMath(first) || nativeMath(second))
      return nativeOperation(op, e.token, first, second, e.args[0].get(), *e.args[1]);

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

    if (arbitraryNumber(first) || arbitraryNumber(second)) return checkArbitraryBinary(e, first, second);

    if (calendarValue(first) || calendarValue(second)) {
      if (!isComparison(op))
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

    return isOrdering(op) ? Type::Bool : first;
  }
  // `a (0.5)== b`: the tolerance takes the scalar type being compared.
  void checkTolerance(const Expr &e, const Type &first, const Type &second) {
    const auto shape = nativeMath(first) ? nativeMath(first) : nativeMath(second);
    const Type scalar = shape ? Type(shape.scalar)
                        : numeric(first) && numeric(second) ? commonNumeric(first, second) : Type(Type::Void);

    if (!floating(scalar))
      fail(e.token, "a comparison tolerance needs float or double operands, got " + typeName(first) + " and " + typeName(second));

    const auto &tolerance = *e.args[2];
    const auto given = decay(tolerance, expression(tolerance, nativeLiteralHint(tolerance, {scalar.base, 1, 1})));

    if (!numeric(given)) fail(tolerance.token, "a comparison tolerance must be a number, got " + typeName(given));

    convert(tolerance, given, scalar);
  }
  Type binaryOperandHint(const Expr &e, const Type &expected, const Type &first) {
    const auto &op = e.token.text;
    Type hint = Type::Void;

    if (const auto shape = nativeMath(first)) return nativeLiteralHint(*e.args[1], shape);

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

    return hint;
  }
  Type checkArbitraryBinary(const Expr &e, const Type &first, const Type &second) {
    const auto &op = e.token.text;
    const auto type = arbitraryCommon(first, second);

    if (type == Type::Void)
      fail(e.token, "big/ubig and tiny/utiny combine only with their own kind and integers "
                    "up to 64 bits; got " + typeName(first) + " and " + typeName(second));

    const bool comparison = isComparison(op);

    if (!comparison && op != "+" && op != "-" && op != "*" && op != "/" &&
        !(op == "%" && bigInteger(type)))
      fail(e.token, typeName(type) + " supports + - * /" +
                        (bigInteger(type) ? " %" : "") + " and comparisons");

    convert(*e.args[0], first, type);
    convert(*e.args[1], second, type);
    return comparison ? Type::Bool : type;
  }
  Type checkExpression(const Expr &e, Type expected) {
    if (e.kind == "compileTime") return expression(*e.args[0], expected);

    if (e.kind == "scopeActive") {
      analysis.scopeEffects = true;
      analysis.expressionTypes[&e] = Type::Bool;
      return Type::Bool;
    }

    if (e.kind == "markupTake") {
      analysis.expressionTypes[&e] = markupType();
      return markupType();
    }

    if (e.kind == "macroValue") {
      scopes.emplace_back();

      for (const auto &binding : e.bindings) statement(*binding);

      const auto type = expression(*e.args[0], expected);
      scopes.pop_back();
      return type;
    }

    const auto &op = e.token.text;

    if (e.kind == "lambda") return lambdaExpression(e);
    if (e.kind == "duckCall" || e.kind == "duckCheck" || e.kind == "duckExists") return duckCall(e, expected);

    if (e.kind == "invoke") {
      const auto callee = expression(*e.receiver);

      if (!funcValue(callee)) fail(e.token, "only a func value can be called this way, not " + typeName(callee));

      return indirectCall(e, callee, {});
    }

    // Outside a for header a range is the array of its values.
    if (e.kind == "range") {
      if (expected != Type::Void && (!dynamicArray(expected) || !integral(*expected.element)))
        fail(e.token, "a range makes a dynamic array of integers, not " + typeName(expected));

      return Type::array(rangeEndpoints(e, expected == Type::Void ? expected : *expected.element), 0);
    }

    if (e.kind == "initializer" && !recordValue(expected)) {
      Type type = expected;

      if (expected == Type::Void) type = inferLiteral(e);
      else if (!arrayValue(expected) && !collectionValue(expected))
        fail(e.token, "a { ... } literal builds a container, array, queue, stack, or pair, not " +
                          typeName(expected));

      checkLiteral(e, type);
      return type;
    }

    if (e.kind == "initializer") return checkRecordInitializer(e, expected);

    if (e.kind == "char")
      return Type::Char;

    if (e.kind == "typeMember") return typeLimit(e);
    if (e.kind == "construct" && nativeMath(e.castType)) return checkNativeConstruct(e);
    if (e.kind == "construct") return checkConstruct(e);
    if (e.kind == "cast") return checkCast(e);
    if (e.kind == "real" && decimalValue(expected)) return checkDecimalLiteral(e, expected);
    if (e.kind == "real") return checkFloatingLiteral(e, expected);

    // Parse magnitudes without passing through a signed host integer. A
    // negated literal is checked as a whole, including the signed minima.
    const bool negativeLiteral = e.kind == "unary" && op == "-" &&
                                 e.args[0]->kind == "int";

    if (e.kind == "int" || negativeLiteral) return checkIntegerLiteral(e, expected, negativeLiteral);

    if (e.kind == "null")
      return expected.depth || funcValue(expected) ? expected : Type::Pointer;

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

    if (e.kind == "guard" && e.args[0]->kind == "duckCall") return checkGuardedDuckCall(e, expected);
    if (e.kind == "guard" && e.args[0]->kind == "call") return checkGuardedCall(e);
    if (e.kind == "guard") return checkGuardedRead(e);

    if (e.kind == "index" && pairIndex(e)) {
      // p[key] reads the value stored for key; a missing key traps.
      const auto &base = *e.args[0];
      requireComplete(base);
      const auto type = location(base).type;
      expression(*e.args[1], *type.key);
      analysis.expressionTypes[&e] = *type.element;
      return *type.element;
    }

    if (e.kind == "name" || e.kind == ".")
      if (const auto function = functionValue(e); function != Type::Void) return function;

    if (e.addressable()) {
      if (std::string owner; propertyOf(e, owner)) return propertyRead(e, owner);
      if (const auto member = collectionMember(e); member != Type::Void) return member;

      return location(e).type;
    }

    if (e.kind == "entry") fail(e.token, "key : value entries can only be added to a pair, as p += key : value");
    if (e.kind == "removal") fail(e.token, "-= [i] removes an element from an array or a pair");
    if (e.kind == "mutation") return checkMutationResult(e);

    if (e.kind == "moveCheck") {
      ownershipMove(*e.args[0], *e.args[1], e.token);
      return Type::Bool;
    }

    if (e.kind == "unary" && op == "&") return checkAddressOf(e);
    if (e.kind == "call") return checkCall(e, expected);

    auto first = expression(*e.args[0],
                            (e.kind == "unary" && op == "-" &&
                             (numeric(expected) || arbitraryNumber(expected))) ||
                            (e.kind == "binary" && op == "+" && managedText(expected)) ||
                            leftLiteralAdopts(e, expected)
                                ? expected
                                : e.kind == "binary" ? nativeLiteralHint(*e.args[0], nativeMath(expected))
                                                     : Type(Type::Void));

    // Both conditional branches keep their enum type.
    if (e.kind != "conditional") first = decay(*e.args[0], first);

    if (funcValue(first) && e.kind == "binary") {
      if ((op != "==" && op != "!=") || e.args[1]->kind != "null")
        fail(e.token, "a func value only compares with null, as in f != null");

      expression(*e.args[1], first);
      return Type::Bool;
    }

    if (e.kind == "conditional") return checkConditional(e, expected, first);

    if (e.kind == "unary" && arbitraryNumber(first)) {
      if (op != "-") fail(e.token, typeName(first) + " supports only arithmetic negation");

      if (first == Type::UBig || first == Type::UTiny)
        fail(e.token, "cannot negate unsigned " + typeName(first));

      return first;
    }

    if (e.kind == "unary" && nativeMath(first)) {
      if (op != "-") fail(e.token, "operator '" + op + "' does not apply to " + typeName(first));

      return first;
    }

    if (e.kind == "unary") {
      if (op == "!")
        return truth(*e.args[0], first, e.token);

      if (op == "~" && !integral(first))
        fail(e.token, "bitwise operators require integer operands");
      else if (!numeric(first))
        require(first, Type::Int, e.token);

      return first;
    }

    if (op == "&&" || op == "||") {
      truth(*e.args[0], first, e.token);
      truth(*e.args[1], decay(*e.args[1], expression(*e.args[1])), e.token);
      return Type::Bool;
    }

    return checkBinary(e, expected, first);
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

      analysis.floatingConstants[&e] = floatingConstantText(value);
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
          analysis.expressionTypes.at(s.expr.get()).role != Type::Owner || s.expr->kind != "name" ||
          analysis.receiverStorage.count(s.expr.get())) continue;

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
  // Literals, enum members, type limits, #Call(...), and # constants.
  bool constantLabel(const Expr &e) {
    if (e.kind == "int" || e.kind == "bool" || e.kind == "char" || e.kind == "text" || e.kind == "typeMember" ||
        e.kind == "compileTime" || analysis.enumMembers.count(&e) ||
        (e.kind == "unary" && e.token.text == "-" && e.args[0]->kind == "int"))
      return true;

    if (const auto field = analysis.staticFields.find(&e); field != analysis.staticFields.end())
      return symbols.statics.at(field->second).constant;

    if (e.kind != "name") return false;

    const auto *local = findLocal(e.token);
    return local && local->constant;
  }
  // Two labels with the same key can never both be reached. Empty when the
  // value is not known until later, as with #Call(...).
  std::string labelKey(const Expr &e) {
    if (const auto number = analysis.integerConstants.find(&e); number != analysis.integerConstants.end())
      return "integer " + number->second;

    if (const auto member = analysis.enumMembers.find(&e); member != analysis.enumMembers.end())
      return "member " + std::to_string(member->second);

    if (e.kind == "bool" || e.kind == "char" || e.kind == "text") return e.kind + " " + e.token.text;

    if (e.kind == "name")
      if (const auto *local = findLocal(e.token); local && local->constantValue)
        return labelKey(*local->constantValue);

    return "";
  }
  // Each section ends control, so entry facts hold at the start of every one
  // and only breaks, plus a missing default, reach the code after the Switch.
  bool switchStatement(const Stmt &s) {
    scopes.emplace_back();
    statement(*s.initializer);
    const auto selector = analysis.localTypes.at(s.initializer.get());

    if (!integral(selector) && selector != Type::Bool && selector != Type::Char && !enumValue(selector) &&
        !managedText(selector))
      fail(s.initializer->token, "a Switch selector must be an integer, bool, char, enum, string, or name, not " +
                                     typeName(selector));

    for (const auto &binding : s.alternate) statement(*binding);

    expression(*s.target);
    const auto entry = scopes;
    loops.emplace_back();
    loops.back().depth = scopes.size();
    loops.back().isSwitch = true;
    std::map<std::string, Token> seen;
    bool hasDefault = false;

    for (const auto &section : s.body) {
      if (section->isDefault) {
        if (hasDefault) fail(section->token, "a Switch can have only one default");

        hasDefault = true;
      }

      for (const auto &label : section->labels) {
        expression(*label, selector);

        if (!constantLabel(*label))
          fail(label->token, "a case label must be a literal, enum member, type limit, #Call(...), or # constant");

        const auto key = labelKey(*label);

        if (key.empty()) continue;

        const auto inserted = seen.emplace(key, label->token);

        if (!inserted.second)
          fail(label->token, "duplicate case label; the same value is used at " + where(inserted.first->second));
      }

      scopes = entry;

      if (!block(section->body))
        fail(section->token, "this Switch section can run off its end; finish it with break, continue, or return "
                             "(Duck has no implicit fallthrough; stack labels to share a body)");
    }

    auto exits = std::move(loops.back().breaks);
    loops.pop_back();

    if (!hasDefault) exits.push_back(entry);

    const bool terminated = exits.empty();
    scopes = terminated ? entry : merge(entry, exits);

    if (terminated) analysis.terminatingLoops.insert(&s);
    else requireCompleteScopes(scopes.size() - 1);

    scopes.pop_back();
    return terminated;
  }
  bool checkMarkupSplice(const Stmt &s) {
    const auto &content = *s.alternate.front();

    // A stored markup value moves in: nothing is copied.
    if (content.expr->addressable() && storage(*content.expr) && markupValue(location(*content.expr).type)) {
      location(*s.target, false);
      analysis.markupSplices.insert(&s);
      return false;
    }

    const auto savedAnalysis = analysis;
    const auto savedScopes = scopes;
    const auto type = expression(*content.expr);
    analysis = savedAnalysis;
    scopes = savedScopes;

    if (markupValue(type)) {
      location(*s.target, false);
      expression(*content.expr);
      analysis.markupSplices.insert(&s);
      return false;
    }

    bool holdsValue = false;

    if (content.target) {
      auto owner = location(*content.target->args[0]).type;

      if (owner.depth) owner = owner.pointee();

      MemberPath field;
      holdsValue = recordValue(owner) && symbols.findField(owner.record, "value", field);
    }

    if (!holdsValue)
      fail(content.token, "markup content must be an element or a markup value, or a value for a declared "
                          "value field; got " + typeName(type));

    return statement(content);
  }
  bool checkReturn(const Stmt &s) {
    if (inferring) {
      result = expression(*s.expr);

      if (result == Type::Void) fail(s.expr->token, "a getter must produce a value");

      inferring = false;
      return true;
    }

    if (s.expr && frames.empty() && (result.role == Type::Owner || (probing && result.depth == 1))) return checkTrackedReturn(s);
    if (s.expr) requireComplete(*s.expr);

    requireCompleteScopes(0);

    // `return local` moves a value with a destructor out of the function.
    if (s.expr && s.expr->kind == "name" &&
        (collectionValue(result) || (arrayValue(result) && symbols.moveOnly(result)) ||
         (recordValue(result) && symbols.destructible(result.record)))) {
      const auto *local = findLocal(s.expr->token);

      if (local && analysis.captured.count(captureKey(local->declaration)))
        fail(s.expr->token, "cannot move '" + s.expr->token.text + "' out of this function: a lambda captured it");

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
  bool checkTrackedReturn(const Stmt &s) {
    // Any path can hand back a tracked reference. The caller learns which
    // kind it got from the returned handle.
    requireCompleteScopes(0);
    analysis.handleReturns.insert(&s);

    if (s.expr->kind == "null") {
      expression(*s.expr, result);
      return true;
    }

    if (s.expr->kind == "call") ownerDestination = s.expr.get();

    const auto type = expression(*s.expr);
    ownerDestination = nullptr;
    const auto *called = calledFunction(*s.expr);

    if (probing) {
      probedOwns = probedOwns || (called && called->owns);
      probedLends = probedLends || (called ? called->lends : type.role != Type::Raw);
      return true;
    }

    if (type.role == Type::Raw || type.pointee() != result.pointee() ||
        (s.expr->kind != "call" && !trackedPlace(*s.expr)))
      fail(s.token, "this function returns tracked heap references, so it cannot return " + typeName(type) +
                        "; return a tracked reference, ::owner, or null");

    return true;
  }
  bool checkIf(const Stmt &s) {
    const auto tested = expression(*s.expr);

    if (!tested.depth) require(tested, Type::Bool, s.token);

    // The branch uses the reference `if p` evaluated, unless it replaces p.
    // A local owner needs no binding: nothing else can clear it.
    const auto place = tested.role != Type::Raw ? placeKey(*s.expr) : std::string();
    const bool localOwner = tested.role == Type::Owner && s.expr->kind == "name" &&
                            !analysis.receiverStorage.count(s.expr.get()) && !lookup(s.expr->token).lent;
    bool bound = !place.empty() && !localOwner;

    for (const auto &child : s.body) bound = bound && !writesPlace(*child, pathText(*s.expr));

    if (bound) {
      analysis.boundConditions.insert(&s);
      checks.push_back({place, &s, frames.size()});
    }

    auto before = scopes;
    bool yes = block(s.body);

    if (bound) checks.pop_back();

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
  // Analyzes a loop body and leaves `scopes` holding the facts at its end:
  // the merge of normal completion and every continue. Returns those states
  // and the states at each break.
  std::pair<std::vector<Scopes>, std::vector<Scopes>> loopBody(const Stmt &s, const Scopes &entry) {
    loops.emplace_back();
    loops.back().depth = scopes.size();
    const bool terminated = block(s.body);
    auto incoming = std::move(loops.back().continues);
    auto exits = std::move(loops.back().breaks);
    loops.pop_back();

    if (!terminated) incoming.push_back(scopes);

    scopes = incoming.empty() ? entry : merge(entry, incoming);
    return {std::move(incoming), std::move(exits)};
  }
  bool checkDo(const Stmt &s) {
    const auto entry = scopes;
    auto [incoming, exits] = loopBody(s, entry);

    // Without a condition only break leaves, so its facts are all that hold.
    if (s.expr) truth(*s.expr, expression(*s.expr), s.token);

    if (s.expr && !incoming.empty())
      exits.push_back(scopes);

    scopes = exits.empty() ? entry : merge(entry, exits);

    // An @end anywhere below a marked loop leaves it, so it can always finish.
    if (exits.empty() && !(s.body.size() == 1 && s.body.front()->kind == "scopedBody")) {
      analysis.terminatingLoops.insert(&s);
      return true;
    }

    return false;
  }
  bool checkPreconditionLoop(const Stmt &s) {
    scopes.emplace_back();

    if (s.initializer)
      statement(*s.initializer);

    if (s.kind == "foreach")
      bindIteration(s);
    else if (s.kind == "rangefor") {
      const auto type = rangeEndpoints(*s.expr, Type::Void);
      analysis.expressionTypes[s.expr.get()] = type;
      analysis.localTypes[&s] = type;
      auto name = s.token;
      name.text = s.name;
      bind(name, {type, false});
    } else if (s.expr)
      truth(*s.expr, expression(*s.expr), s.token);

    const auto entry = scopes;
    // Still type-check an unreachable step, using conservative entry facts.
    loopBody(s, entry);

    if (s.step)
      statement(*s.step);

    scopes = entry; // A precondition loop can execute zero times.
    requireCompleteScopes(scopes.size() - 1);
    scopes.pop_back();

    if (s.kind == "foreach") iterations.pop_back();

    return false;
  }
  void checkLocalDeclaration(const Stmt &s) {
    symbols.validateType(s.type, s.token);
    // Inspect the initializer before choosing between raw storage and an alias.
    const bool inferredFunc = funcValue(s.type) && !s.type.signature;
    const Type hint = declarationHint(s, inferredFunc);

    // A fresh inferred local receives a call's result as it comes.
    if (s.kind == "own" || (s.kind == "let" && s.type == Type::Void && s.expr && s.expr->kind == "call"))
      ownerDestination = s.expr.get();

    std::set<std::string> missing;
    bool lent = false;

    if (!s.expr && s.kind == "own" && recordValue(s.type.pointee())) {
      missing = defaultMissing(s.type.pointee().record);

      if (!missing.empty()) incomplete(s.token, s.type.pointee().record, missing);
    } else if (!s.expr && recordValue(s.type))
      missing = defaultMissing(s.type.record);

    if (s.kind == "let" && s.expr && recordValue(s.expr->castType)) incompleteAllowed = s.expr.get();

    incompleteMissing.clear();

    if (s.kind == "let" && s.expr && s.expr->kind == "lambda") scopedLambda = s.expr.get();

    scopedLambdaBorrowed = false;
    auto type = s.expr ? expression(*s.expr, hint) : s.type;
    scopedLambda = nullptr;
    const bool keepsFrame = scopedLambdaBorrowed;
    scopedLambdaBorrowed = false;

    if (inferredFunc && !funcValue(type)) fail(s.token, "func local '" + s.name + "' needs a func value, not " + typeName(type));
    if (incompleteAllowed) missing = incompleteMissing;

    incompleteAllowed = nullptr;
    ownerDestination = nullptr;
    resolveLocalRole(s, type, lent);

    if (type == Type::Void)
      fail(s.token, "locals currently support only values, not void");

    if (s.type != Type::Void && !inferredFunc) {
      auto declared = s.type;
      declared.role = type.role;
      require(type, declared, s.token);
    }

    analysis.localTypes[&s] = type;
    auto name = s.token;
    name.text = s.name;
    // A fixed array without an initializer gives each element its
    // default, which must not need running code.
    Type leaf = type;

    while (fixedArray(leaf)) leaf = *leaf.element;

    if (fixedArray(type) && !s.expr && recordValue(leaf) && symbols.runtimeDefaults(leaf.record))
      fail(s.token, "'" + s.name + "' cannot default its elements: '" + leaf.record +
                        "' is built at run time (constructors or computed defaults); use a dynamic array (" +
                        leaf.record + " []) and add elements to it");

    bind(name, {type, s.constant,
                s.kind == "own" || s.expr != nullptr || recordValue(type) || collectionValue(type) ||
                    arrayValue(type)});

    if (s.kind == "own" && s.type == Type::Void && method && !scopes.empty())
      if (const auto field = scopes.front().find(s.name);
          field != scopes.front().end() && field->second.receiver && field->second.type.role == Type::Owner)
        fail(s.token, "'" + s.name + "' is an owning field; write this." + s.name + " :: ... to move into it");

    auto &bound = scopes.back().at(name.text);
    bound.mayMiss = bound.mustMiss = missing;
    bound.lent = lent;
    bound.declaration = name;
    bound.callOnly = keepsFrame;
    bound.constantValue = s.constant ? s.expr.get() : nullptr;
  }
  Type declarationHint(const Stmt &s, bool inferredFunc) {
    Type hint = s.type;

    if (inferredFunc) {
      if (!s.expr) fail(s.token, "func local '" + s.name + "' needs an initializer or a signature, as in func int() " + s.name);

      hint = Type::Void;
    }

    if (s.expr && s.type.depth && s.expr->kind != "null") {
      const bool literal = s.expr->kind == "int" || s.expr->kind == "real" ||
          (s.expr->kind == "unary" && s.expr->token.text == "-" && s.expr->args[0]->kind == "int");
      hint = (literal && numeric(s.type.pointee())) || s.expr->kind == "initializer"
                 ? s.type.pointee() : Type(Type::Void);
    }

    return hint;
  }
  void resolveLocalRole(const Stmt &s, Type &type, bool &lent) {
    if (s.kind == "let" && s.type.depth && heapValue(s.type.pointee()) && heapValue(type)) {
      convert(*s.expr, type, s.type.pointee());
      type = s.type;
      type.role = Type::Owner;
    } else if (s.kind == "own") {
      if (s.expr && type.depth) {
        if (type.role != Type::Owner || !ownerSource(*s.expr))
          fail(s.token, "ownership transfer requires a mutable owning source");

        requireOwningResult(*s.expr, s.token);
      } else if (s.expr) {
        if (!heapValue(type))
          fail(s.token, "heap allocation requires a scalar or container value");

        type = type.pointerTo();
      }

      type.role = Type::Owner;
    } else if (type.role != Type::Raw && s.expr->kind == "call") {
      // Consumes an owning result and keeps a borrowed one as an alias.
      const auto *called = calledFunction(*s.expr);
      lent = called && called->lends;
    } else if (type.role != Type::Raw) {
      if (!trackedPlace(*s.expr)) fail(s.token, "tracked aliases require a named source");

      type.role = Type::Alias;
    } else if (s.expr && s.type.depth) {
      convert(*s.expr, type, s.type);
      type = s.type;
    }
  }
  bool checkAssignment(const Stmt &s) {
    // `?a[i] = v` assigns only when the element exists.
    const auto &target = s.target->kind == "guard" ? *s.target->args[0] : *s.target;
    requireComplete(target);
    std::string owner;
    auto local = propertyOf(target, owner) ? Local{propertyWrite(target, owner, s.kind != "assign"), false}
                                           : location(target, s.kind != "assign");

    if (&target != s.target.get() && target.kind != "index" && !analysis.scopedPlaces.count(&target))
      fail(s.token, "'?' guards an index such as ?a[i], a method call such as ?p.Foo(), a duck call such as ?e..Foo(), or a scoped variable");

    if (&target != s.target.get()) analysis.expressionTypes[s.target.get()] = local.type;
    if (local.constant) constantTarget(target, s.token);

    // Replacing a collection, or storing a new grid cell, changes its length.
    if (s.kind == "assign" && (arrayValue(local.type) || collectionValue(local.type)))
      removesCurrent(target, local.type, "=", nullptr, s.token);

    if (std::vector<const Expr *> coordinates; &target == s.target.get() && target.kind == "index")
      if (const auto *grid = gridAccess(target, coordinates))
        removesCurrent(*grid, location(*grid).type, "[...] =", nullptr, s.token);

    if (local.type.role != Type::Raw) {
      if (s.kind != "assign") fail(s.token, "tracked binding requires null or an alias assignment");

      auto source = expression(*s.expr);

      if (s.expr->kind != "null" && (local.type.role != Type::Alias || !trackedPlace(*s.expr)))
        fail(s.token, "tracked binding requires null or an alias assignment");

      if (s.expr->kind != "null" && (source.role == Type::Raw ||
          source.pointee() != local.type.pointee()))
        fail(s.token, "incompatible tracked alias source");

      return false;
    }

    if ((arrayValue(local.type) || collectionValue(local.type)) && s.kind != "assign") {
      collectionMutation(target, local.type, s.kind, s.expr.get(), s.token);
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

    const bool nativeCompound = s.kind != "assign" && nativeMath(local.type);

    if (nativeCompound) {
      if (s.kind != "+=" && s.kind != "-=" && s.kind != "*=" && s.kind != "/=")
        fail(s.token, "'" + s.kind + "' does not apply to " + typeName(local.type));

      const auto right = decay(*s.expr, expression(*s.expr, nativeLiteralHint(*s.expr, nativeMath(local.type))));

      if (nativeOperation(s.kind.substr(0, 1), s.token, local.type, right, nullptr, *s.expr) != local.type)
        fail(s.token, "'" + s.kind + "' must leave " + typeName(local.type) + " the same type");
    } else if (s.kind != "assign" && !(s.kind == "+=" && managedText(local.type)) && !arbitraryArithmetic &&
               (!numeric(local.type) || (s.kind == "%=" && !integral(local.type))))
      require(local.type, Type::Int, s.token);

    if (s.expr && !nativeCompound) {
      if (s.kind == "<<=" || s.kind == ">>=") {
        if (!integral(expression(*s.expr)))
          fail(s.expr->token, "bitwise operators require integer operands");
      } else
        expression(*s.expr, local.type);
    }

    if (s.target->kind == "name" && !analysis.staticFields.count(s.target.get()) &&
        !analysis.properties.count(s.target.get()))
      lookup(s.target->token).initialized = true;

    return false;
  }
  bool statement(const Stmt &s) {
    if (s.kind == "block" || s.kind == "ordered")
      return block(s.body);

    if (s.kind == "switch") return switchStatement(s);

    if (s.kind == "scopeStart" || s.kind == "scopeEnd") {
      analysis.scopeEffects = true;
      analysis.endsScope = analysis.endsScope || s.kind == "scopeEnd";
      return false;
    }

    if (s.kind == "scopedBody") {
      analysis.scopeEffects = true;
      return block(s.body);
    }

    if (s.kind == "markupAdd") {
      location(*s.target, false);
      expression(*s.expr);
      return false;
    }

    if (s.kind == "markupSplice") return checkMarkupSplice(s);

    if (s.kind == "break") {
      if (loops.empty())
        fail(s.token, "break requires an enclosing loop or Switch");

      requireCompleteScopes(loops.back().depth);
      loops.back().breaks.push_back(scopes);
      return true;
    }

    if (s.kind == "continue") {
      auto *loop = enclosingLoop();

      if (!loop)
        fail(s.token, "continue requires an enclosing loop");

      requireCompleteScopes(loop->depth);
      loop->continues.push_back(scopes);
      return true;
    }

    if (s.kind == "returnOwner") {
      requireCompleteScopes(0);

      if (!s.expr || s.expr->kind != "name" || lookup(s.expr->token).constant ||
          expression(*s.expr) != result)
        fail(s.token, "return :: requires a mutable owning local of the declared type");

      return true;
    }

    if (s.kind == "return") return checkReturn(s);

    if (s.kind == "moveInto") {
      ownershipMove(*s.target, *s.expr, s.token);
      return false;
    }

    if (s.kind == "if") return checkIf(s);
    if (s.kind == "do") return checkDo(s);
    if (s.kind == "while" || s.kind == "for" || s.kind == "foreach" || s.kind == "rangefor") return checkPreconditionLoop(s);

    if (s.kind == "let" || s.kind == "own") {
      checkLocalDeclaration(s);
    } else if (s.kind == "expr") {
      expression(*s.expr);

      if (analysis.nativeMethods.count(s.expr.get()) && !analysis.nativeInPlace.count(s.expr.get()))
        fail(s.expr->token, "'" + s.expr->token.text + "()' returns a new value and changes nothing; use its result");
    }
    else {
      return checkAssignment(s);
    }

    return false;
  }

public:
  Analyzer(const Symbols &symbols, Type result)
      : symbols(symbols), result(result) {}
  // What the function's returns can hand back, as far as is known so far: a
  // body that does not analyze yet is reported by its real analysis later.
  std::pair<bool, bool> probeReturns(const Function &function) {
    probing = true;

    try {
      run(function);
    } catch (const std::runtime_error &) {
    }

    return {probedOwns, probedLends};
  }
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

    // A lambda default is built at run time, and analyzed with its generated method.
    if (e.kind == "lambda") return std::nullopt;

    expression(e, type);

    if (bigInteger(type) || recordValue(type)) return std::nullopt;
    // A string or name literal; lowering emits it as immortal constant storage.
    if (e.kind == "text" && managedText(type)) return "";

    const bool converted = analysis.conversions.count(&e) != 0;
    auto encode = [](double value) {
      return floatingConstantText(value);
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
      bindVisibleFields();
      scopes.emplace_back();
    }

    if (function.generated && method && !function.body.empty() && function.body.front()->expr &&
        function.body.front()->expr->kind == "lambda")
      memberLambdaSite = function.body.front()->expr.get();

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
void resolveReturnRoles(Symbols &symbols, const std::vector<const Program *> &programs,
                        const std::vector<Symbols *> &tables) {
  std::vector<const Function *> candidates;

  for (const auto *program : programs)
    for (const auto &f : *program)
      if (f.nativeSymbol.empty() && f.result.depth == 1 && heapValue(f.result.pointee()) &&
          propertyName(f.name.text).empty())
        candidates.push_back(&f);

  auto update = [&](const Function &f, bool owns, bool lends) {
    if (owns || lends) f.result.role = Type::Owner;

    for (auto *table : tables)
      if (auto found = table->find(f.name.text); found != table->end()) {
        found->second.result = f.result;
        found->second.owns = owns;
        found->second.lends = lends;
      }
  };

  for (const auto *f : candidates)
    if (f->result.role == Type::Owner) update(*f, true, false);

  // A function can return another's result, so repeat until nothing changes.
  for (bool progress = true; progress;) {
    progress = false;

    for (const auto *f : candidates) {
      const auto &known = symbols.at(f->name.text);
      const auto [owns, lends] = Analyzer(symbols, f->result).probeReturns(*f);
      const bool allOwns = known.owns || owns, allLends = known.lends || lends;

      if (allOwns == known.owns && allLends == known.lends) continue;

      update(*f, allOwns, allLends);
      progress = true;
    }
  }
}
void resolveFieldDefaults(Symbols &symbols) {
  for (auto &[name, record] : symbols.records)
    for (auto &field : record.fields)
      if (field.initializer && (field.type.role == Type::Owner || markupValue(field.type))) {
        // Allocated at run time; analyzed with its generated method.
        field.runtimeDefault = true;
      } else if (field.initializer) {
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
  copy->lambda = e.lambda;

  for (const auto &binding : e.bindings) {
    auto local = std::make_unique<Stmt>();
    local->token = binding->token;
    local->kind = binding->kind;
    local->name = binding->name;
    local->type = binding->type;
    local->constant = binding->constant;
    local->expr = clone(*binding->expr);
    copy->bindings.push_back(std::move(local));
  }

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

      if (field.type.role == Type::Owner) generated.back().body.front()->kind = "moveInto";

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

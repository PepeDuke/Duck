#include "Pipeline.h"
#include <cmath>
#include <optional>

namespace duck {
namespace {
// A task may only exchange plain values with its caller. Text, pointers, and
// containers holding them would share storage that is not thread safe.
bool plainValue(const Type &type) {
  return !type.depth && type.role == Type::Raw &&
         (numeric(type) || type == Type::Bool || enumValue(type));
}
// What a task may reach through a reference: plain values, and containers and
// arrays built only from them. Reads and writes of it are tracked as resources.
bool taskData(const Type &type, const Symbols &symbols) {
  if (type == Type::Void || plainValue(type)) return true;
  if (arrayValue(type)) return taskData(*type.element, symbols);
  if (!recordValue(type) || symbols.destructible(type.record)) return false;

  for (const auto &field : symbols.records.at(type.record).fields)
    if (!field.scope.empty() || !taskData(field.type, symbols)) return false;

  return true;
}
template <class Visit> void visitExpression(const Expr &e, Visit &visit) {
  visit(e);

  if (e.receiver) visitExpression(*e.receiver, visit);

  for (const auto &binding : e.bindings)
    if (binding->expr) visitExpression(*binding->expr, visit);

  for (const auto &child : e.args) visitExpression(*child, visit);
}
template <class VisitExpr, class VisitStmt>
void visitStatement(const Stmt &s, VisitExpr &expr, VisitStmt &stmt) {
  stmt(s);

  if (s.expr) visitExpression(*s.expr, expr);
  if (s.target) visitExpression(*s.target, expr);
  if (s.initializer) visitStatement(*s.initializer, expr, stmt);
  if (s.step) visitStatement(*s.step, expr, stmt);
  for (const auto &label : s.labels) visitExpression(*label, expr);
  for (const auto &child : s.body) visitStatement(*child, expr, stmt);
  for (const auto &child : s.alternate) visitStatement(*child, expr, stmt);
}
bool candidate(const Function &f, const Symbols &symbols) {
  if (!f.nativeSymbol.empty() || f.generated || !(plainValue(f.result) || f.result == Type::Void)) return false;

  for (const auto &p : f.params)
    if (p.reference ? !taskData(p.type, symbols) : !plainValue(p.type)) return false;

  return true;
}

// Where the statements of one walk are recorded, for whichever pass wants them.
struct Recording {
  std::map<const Stmt *, EffectSet> statements;
  std::map<const Expr *, EffectSet> calls;      // A call's effects, without evaluating its arguments.
  std::map<const Stmt *, Resource> declared;    // The local a declaration creates.
  std::map<const Stmt *, FunctionAnalysis::OrderedLocks> ordered;
};

// Computes what a function body reads and writes, naming each piece of storage
// by its root and field path. Anything the walk does not understand is
// unknown, so a new kind of statement can only make scheduling more cautious.
class EffectWalker {
  struct Place {
    Resource resource;
    Type type;
    bool truncated = false; // Reached through an index: later fields do not refine it.
  };
  const Function &f;
  const FunctionAnalysis &a;
  const Symbols &symbols;
  const EffectTable &table;
  bool library;
  Recording *recording;
  std::vector<std::map<std::string, Place>> scopes;
  std::map<std::string, std::pair<Token, Type>> locals; // By unique id.
  std::vector<std::set<std::string> *> activeBlocks;    // Locals declared inside each open `${ }`.
  std::set<std::string> lockedLocals;                   // Locals some inner block locks by address.
  unsigned counter = 0;
  EffectSet current;

  Place *find(const std::string &name) {
    for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope)
      if (auto found = scope->find(name); found != scope->end()) return &found->second;

    return nullptr;
  }
  Resource declare(const Token &token, const Type &type) {
    Resource resource;
    resource.root = token.text + "#" + std::to_string(counter++);
    locals[resource.root] = {token, type};

    for (auto *block : activeBlocks) block->insert(resource.root);

    scopes.back()[token.text] = {resource, type, false};
    return resource;
  }
  void declareAlias(const Token &token, const Place &place) { scopes.back()[token.text] = place; }
  void extend(Place &place, const MemberPath &member) const {
    if (!place.truncated) place.resource.path.insert(place.resource.path.end(), member.indices.begin(), member.indices.end());

    place.type = member.field->type;
  }
  bool captured(const std::string &name) const {
    const auto suffix = ":" + name;

    for (const auto &key : a.captured)
      if (key.size() >= suffix.size() && key.compare(key.size() - suffix.size(), suffix.size(), suffix) == 0) return true;

    return false;
  }
  // The storage an expression names, evaluating any index in it. Empty when
  // the storage cannot be named, as through a pointer.
  std::optional<Place> place(const Expr &e) {
    if (a.scopedPlaces.count(&e) || a.properties.count(&e)) return std::nullopt;

    if (auto found = a.staticFields.find(&e); found != a.staticFields.end()) {
      const auto &info = symbols.statics.at(found->second);

      if (!info.scope.empty()) return std::nullopt;

      Place result;
      result.resource.kind = Resource::Static;
      result.resource.root = found->second;
      result.type = info.type;
      return result;
    }

    if (e.kind == "name") {
      if (captured(e.token.text)) return std::nullopt;
      if (auto *local = find(e.token.text)) return *local;
      if (!f.method) return std::nullopt;

      MemberPath member;

      if (!symbols.findField(f.params[0].type.record, e.token.text, member) || !member.field->scope.empty()) return std::nullopt;

      auto self = *find("this");
      extend(self, member);
      return self;
    }

    if (e.kind == ".") {
      auto base = place(*e.args[0]);

      if (!base || !recordValue(base->type)) return std::nullopt;

      MemberPath member;

      if (!symbols.findField(base->type.record, e.token.text, member) || !member.field->scope.empty()) return std::nullopt;

      extend(*base, member);
      return base;
    }

    if (e.kind == "index" && !a.markupIndexed.count(&e)) {
      auto base = place(*e.args[0]);

      for (size_t i = 1; i < e.args.size(); ++i) expression(*e.args[i]);
      if (!base) return std::nullopt;

      base->truncated = true;
      base->type = arrayValue(base->type) ? *base->type.element : Type::Void;
      return base;
    }

    return std::nullopt;
  }
  void read(const Expr &e) {
    if (auto found = place(e)) current.reads.insert(found->resource);
    else current.unknown = true;
  }
  void write(const Expr &e, bool alsoRead) {
    auto found = place(e);

    if (!found || (recordValue(found->type) && symbols.destructible(found->type.record))) {
      current.unknown = true;
      return;
    }

    current.writes.insert(found->resource);

    if (alsoRead) current.reads.insert(found->resource);
  }
  bool overridable(const std::string &key) const {
    if (!dispatched(key, true)) return false;
    if (library) return true;

    const auto owner = key.substr(0, key.rfind('.'));
    const auto member = memberName(key);

    for (const auto &[record, info] : symbols.records) {
      (void)info;

      if (record != owner && symbols.derivesFrom(record, owner) && symbols.count(methodKey(record, member))) return true;
    }

    return false;
  }
  // Rewrites a callee's parameter-relative effects in the caller's terms.
  EffectSet apply(const EffectSummary &callee, const std::map<size_t, std::optional<Place>> &bound) {
    EffectSet result;
    result.unknown = callee.unknown;
    result.external = callee.external;
    result.locks = callee.locks;
    result.exclusive = callee.exclusive;
    auto map = [&](const std::set<Resource> &from, std::set<Resource> &into) {
      for (const auto &resource : from) {
        if (resource.kind != Resource::Parameter) {
          into.insert(resource);
          continue;
        }

        auto argument = bound.find(resource.parameter);

        if (argument == bound.end()) continue; // A temporary receiver: nobody else can see it.

        if (!argument->second) {
          result.unknown = true;
          continue;
        }

        auto mapped = argument->second->resource;

        if (!argument->second->truncated) mapped.path.insert(mapped.path.end(), resource.path.begin(), resource.path.end());

        into.insert(mapped);
      }
    };
    map(callee.reads, result.reads);
    map(callee.writes, result.writes);

    if (callee.locks)
      for (const auto &[index, argument] : bound) {
        (void)index;

        if (argument && argument->resource.kind == Resource::Local) lockedLocals.insert(argument->resource.root);
      }

    return result;
  }
  // Reference arguments are bound to the caller's storage; the rest are read.
  void bindArguments(const Expr &e, const Signature &signature, size_t offset, size_t first, size_t count,
                     std::map<size_t, std::optional<Place>> &bound) {
    for (size_t i = 0; i < count; ++i) {
      const auto &argument = *e.args[first + i];
      const auto index = i + offset;

      if (!signature.parameters[index].reference) {
        expression(argument);
        continue;
      }

      auto storage = place(argument);

      if (storage)
        if (auto up = a.upcasts.find(&argument); up != a.upcasts.end() && !storage->truncated)
          storage->resource.path.insert(storage->resource.path.end(), up->second.begin(), up->second.end());

      bound[index] = storage;
    }
  }
  void call(const Expr &e) {
    const auto &op = e.token.text;

    if (a.reinitializations.count(&e) || a.completions.count(&e) || a.reflections.count(&e) ||
        a.indirectCalls.count(&e) || a.elementNames.count(&e)) {
      current.unknown = true;
      return;
    }

    if (auto built = a.constructions.find(&e); built != a.constructions.end() || recordValue(e.castType)) {
      const auto type = a.expressionTypes.at(&e);

      if (recordValue(type) && symbols.runtimeDefaults(type.record)) current.unknown = true;
      if (built == a.constructions.end()) return;

      for (const auto &part : built->second) {
        const auto callee = table.find(part.key);

        if (callee == table.end()) {
          current.unknown = true;
          continue;
        }

        std::map<size_t, std::optional<Place>> bound;
        bindArguments(e, symbols.at(part.key), 1, part.first, part.count, bound);
        current.merge(apply(callee->second, bound));
      }

      return;
    }

    if (a.mathCalls.count(&e)) { // Pure: it only reads its operands.
      for (const auto &argument : e.args) expression(*argument);

      return;
    }

    if (op == "Print" || op == "$T.fail" || op == "$T.show") current.external = true;
    if (e.receiver && memberName(op) == "Clear" && !a.callTargets.count(&e)) return write(*e.receiver, true);
    if (op == "IsValid" && !e.receiver) return read(*e.args[0]);

    const auto target = a.callTargets.find(&e);

    if (target == a.callTargets.end()) {
      if (e.receiver) current.unknown = true;
      for (const auto &argument : e.args) expression(*argument);

      return;
    }

    const auto callee = table.find(target->second);

    if (callee == table.end() || !symbols.count(target->second)) {
      current.unknown = true;

      for (const auto &argument : e.args) expression(*argument);

      return;
    }

    std::map<size_t, std::optional<Place>> bound;

    if (op == "Exec" && !e.receiver) { // Its path names the script; the script takes no arguments.
      current.merge(apply(callee->second, bound));
      return;
    }

    const auto &signature = symbols.at(target->second);

    if (signature.method) {
      std::optional<Place> self;
      bool temporary = false;

      if (!e.receiver) self = *find("this");
      else if (e.receiver->addressable() && !a.properties.count(e.receiver.get())) self = place(*e.receiver);
      else {
        temporary = true;
        expression(*e.receiver);
      }

      if (self && recordValue(self->type)) {
        // Calls on this or a reference parameter run the object's final overrider.
        if (self->resource.kind == Resource::Parameter && overridable(target->second)) current.unknown = true;

        if (auto inherited = a.receiverPaths.find(&e); inherited != a.receiverPaths.end() && !self->truncated)
          self->resource.path.insert(self->resource.path.end(), inherited->second.second.begin(), inherited->second.second.end());
      } else if (!temporary) {
        self.reset();
      }

      if (!temporary) bound[0] = self;
    }

    bindArguments(e, signature, signature.method ? 1 : 0, 0, e.args.size(), bound);
    const auto mapped = apply(callee->second, bound);

    if (recording) recording->calls[&e] = mapped;

    current.merge(mapped);
  }
  void expression(const Expr &e) {
    static const std::set<std::string> constants{"int", "real", "bool", "char", "text", "null", "typeMember", "compileTime"};

    if (constants.count(e.kind)) return;

    if (e.kind == "name" || e.kind == "." || e.kind == "index") {
      if (a.enumMembers.count(&e) || a.functionValues.count(&e)) return;
      if (a.collectionMembers.count(&e)) return read(*e.args[0]);

      if (a.markupLookups.count(&e)) {
        current.unknown = true;
        return;
      }

      return read(e);
    }

    if (e.kind == "call") return call(e);

    if (e.kind == "unary" && (e.token.text == "&" || e.token.text == "*")) {
      current.unknown = true;
      return;
    }

    if (e.kind == "fieldInitializer") return expression(*e.args[0]);

    if (e.kind == "binary" || e.kind == "unary" || e.kind == "conditional" || e.kind == "cast" || e.kind == "construct" ||
        e.kind == "initializer" || e.kind == "entry" || e.kind == "removal" || e.kind == "range") {
      for (const auto &child : e.args) expression(*child);

      return;
    }

    if (e.kind == "macroValue") {
      scopes.emplace_back();

      for (const auto &binding : e.bindings) statement(*binding);

      expression(*e.args[0]);
      scopes.pop_back();
      return;
    }

    if (e.kind == "mutation") {
      for (size_t i = 1; i < e.args.size(); ++i) expression(*e.args[i]);

      return write(*e.args[0], true);
    }

    current.unknown = true;
  }
  void list(const std::vector<std::unique_ptr<Stmt>> &statements) {
    scopes.emplace_back();

    for (const auto &s : statements) statement(*s);

    scopes.pop_back();
  }
  // The leaves of a resource, each one lock. Elements of an array cannot be
  // named before entry, so an array needs the scheduler-wide exclusion.
  void leaves(const Type &type, std::vector<size_t> path, const Resource &root, FunctionAnalysis::OrderedLocks &locks) {
    if (arrayValue(type)) {
      locks.exclusive = true;
      return;
    }

    if (recordValue(type)) {
      const auto &fields = symbols.records.at(type.record).fields;

      for (size_t i = 0; i < fields.size(); ++i) {
        path.push_back(i);
        leaves(fields[i].type, path, root, locks);
        path.pop_back();
      }

      return;
    }

    FunctionAnalysis::OrderedPlace place;
    place.path = std::move(path);

    if (root.kind == Resource::Static) {
      place.isStatic = true;
      place.staticName = root.root;
    } else if (root.kind == Resource::Parameter) place.root = f.params[root.parameter].name;
    else place.root = locals.at(root.root).first;

    locks.places.push_back(std::move(place));
  }
  void ordered(const Stmt &s) {
    auto outer = std::move(current);
    current = {};
    std::set<std::string> inside;
    activeBlocks.push_back(&inside);
    list(s.body);
    activeBlocks.pop_back();

    FunctionAnalysis::OrderedLocks locks;
    locks.exclusive = current.unknown || current.exclusive;
    std::set<Resource> resources = current.reads;
    resources.insert(current.writes.begin(), current.writes.end());

    for (const auto &resource : resources) {
      if (resource.kind == Resource::Local && inside.count(resource.root)) continue;
      if (resource.kind == Resource::Local) lockedLocals.insert(resource.root);

      Type type;

      if (resource.kind == Resource::Static) type = symbols.statics.at(resource.root).type;
      else if (resource.kind == Resource::Parameter) type = f.params[resource.parameter].type;
      else type = locals.at(resource.root).second;

      for (auto index : resource.path) type = symbols.records.at(type.record).fields[index].type;

      leaves(type, resource.path, resource, locks);
    }

    // A callee would lock a local born inside this block, whose address does
    // not exist yet: there is no way to take that lock in order.
    for (const auto &local : lockedLocals)
      if (inside.count(local)) locks.exclusive = true;

    // Locals of this block die with it, so nothing outside can conflict on them.
    auto outside = [&](std::set<Resource> &set) {
      for (auto it = set.begin(); it != set.end();)
        it = it->kind == Resource::Local && inside.count(it->root) ? set.erase(it) : std::next(it);
    };
    outside(current.reads);
    outside(current.writes);
    current.locks = current.locks || locks.exclusive || !locks.places.empty();
    current.exclusive = current.exclusive || locks.exclusive;

    if (recording) recording->ordered[&s] = std::move(locks);

    outer.merge(current);
    current = std::move(outer);
  }
  void statement(const Stmt &s) {
    auto outer = std::move(current);
    current = {};
    effects(s);

    if (recording) recording->statements[&s] = current;

    outer.merge(current);
    current = std::move(outer);
  }
  void letEffects(const Stmt &s) {
    if (s.expr) expression(*s.expr);

    const auto found = a.localTypes.find(&s);

    if (found == a.localTypes.end()) {
      current.unknown = true;
      return;
    }

    const auto &type = found->second;

    if (recordValue(type) && (symbols.destructible(type.record) ||
                              (!s.expr && (symbols.runtimeDefaults(type.record) || !symbols.parts(type.record).empty()))))
      current.unknown = true;

    auto name = s.token;
    name.text = s.name;
    const auto local = declare(name, type);
    current.writes.insert(local);

    if (recording) recording->declared[&s] = local;
  }
  void forEffects(const Stmt &s) {
    scopes.emplace_back();

    if (s.initializer) statement(*s.initializer);
    if (s.expr) expression(*s.expr);

    list(s.body);

    if (s.step) statement(*s.step);

    scopes.pop_back();
  }
  void rangeForEffects(const Stmt &s) {
    expression(*s.expr);
    scopes.emplace_back();
    auto name = s.token;
    name.text = s.name;
    declare(name, a.localTypes.at(&s));
    list(s.body);
    scopes.pop_back();
  }
  void forEachEffects(const Stmt &s) {
    auto collection = place(*s.expr);
    scopes.emplace_back();

    if (collection) {
      current.reads.insert(collection->resource);
      // The loop variable has no storage of its own: it is the element.
      collection->truncated = true;
      collection->type = arrayValue(collection->type) ? *collection->type.element : Type::Void;
    } else current.unknown = true;

    auto name = s.token;
    name.text = s.name;

    if (collection) declareAlias(name, *collection);
    else declare(name, Type::Void);

    if (!s.key.empty()) {
      name.text = s.key;
      declare(name, Type::Void);
    }

    list(s.body);
    scopes.pop_back();
  }
  void switchEffects(const Stmt &s) {
    scopes.emplace_back();
    statement(*s.initializer);

    for (const auto &binding : s.alternate) statement(*binding);

    expression(*s.target);

    for (const auto &section : s.body) statement(*section);

    scopes.pop_back();
  }
  void effects(const Stmt &s) {
    static const std::set<std::string> mutations{"assign", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>=", "++", "--"};

    if (s.kind == "ordered") return ordered(s);
    if (s.kind == "block") return list(s.body);

    if (s.kind == "return") {
      if (s.expr) expression(*s.expr);

      return;
    }

    if (s.kind == "break" || s.kind == "continue") return;
    if (s.kind == "expr") return expression(*s.expr);
    if (s.kind == "let") return letEffects(s);

    if (mutations.count(s.kind)) {
      if (s.expr) expression(*s.expr);

      if (s.target->kind == "guard" || a.properties.count(s.target.get())) {
        current.unknown = true;
        return;
      }

      return write(*s.target, s.kind != "assign");
    }

    if (s.kind == "if") {
      expression(*s.expr);
      list(s.body);
      list(s.alternate);
      return;
    }

    if (s.kind == "while" || s.kind == "do") {
      if (s.expr) expression(*s.expr);

      list(s.body);
      return;
    }

    if (s.kind == "for") return forEffects(s);
    if (s.kind == "rangefor") return rangeForEffects(s);
    if (s.kind == "foreach") return forEachEffects(s);
    if (s.kind == "switch") return switchEffects(s);

    if (s.kind == "case") {
      for (const auto &label : s.labels) expression(*label);

      list(s.body);
      return;
    }

    current.unknown = true; // own, moveInto, markup, scopes, and whatever comes next.
  }

public:
  EffectWalker(const Function &f, const FunctionAnalysis &a, const Symbols &symbols, const EffectTable &table,
               bool library, Recording *recording)
      : f(f), a(a), symbols(symbols), table(table), library(library), recording(recording) {}
  EffectSet run() {
    scopes.emplace_back();

    for (size_t i = 0; i < f.params.size(); ++i) {
      const auto &p = f.params[i];

      if (p.reference) {
        Place place;
        place.resource.kind = Resource::Parameter;
        place.resource.parameter = i;
        place.type = p.type;
        scopes.back()[p.name.text] = place;
      } else declare(p.name, p.type);
    }

    if (a.scopeEffects) current.unknown = true;

    list(f.body);
    return current;
  }
};

// A rough number of simple operations a call performs, to decide whether a
// task pays for its thread. It only ever changes how fast a program runs.
class CostModel {
  static constexpr double DefaultCount = 1000, HugeCost = 1e6, MaxCost = 1e15;
  using Sizes = std::map<size_t, double>; // Element counts of array parameters.
  struct Frame {
    const Function &f;
    const FunctionAnalysis &a;
    const Sizes &sizes;
    std::map<std::string, Type> locals;
  };
  const Program &program;
  const std::vector<FunctionAnalysis> &analyses;
  const Symbols &symbols;
  std::map<std::string, size_t> index;
  std::set<std::string> active;
  std::map<std::string, double> memo;

  Type typeOf(const Expr &e, const Frame &frame) const {
    if (auto found = frame.a.expressionTypes.find(&e); found != frame.a.expressionTypes.end() && arrayValue(found->second))
      return found->second;

    if (e.kind == "name") {
      for (const auto &p : frame.f.params)
        if (p.name.text == e.token.text) return p.type;

      if (auto local = frame.locals.find(e.token.text); local != frame.locals.end()) return local->second;

      MemberPath member;

      if (frame.f.method && symbols.findField(frame.f.params[0].type.record, e.token.text, member)) return member.field->type;
      if (auto found = frame.a.staticFields.find(&e); found != frame.a.staticFields.end()) return symbols.statics.at(found->second).type;
    }

    if (e.kind == ".") {
      if (auto found = frame.a.staticFields.find(&e); found != frame.a.staticFields.end()) return symbols.statics.at(found->second).type;

      const auto base = typeOf(*e.args[0], frame);
      MemberPath member;

      if (recordValue(base) && symbols.findField(base.record, e.token.text, member)) return member.field->type;
    }

    if (e.kind == "index") {
      const auto base = typeOf(*e.args[0], frame);

      if (arrayValue(base)) return *base.element;
    }

    return Type::Void;
  }
  // The expected element count of a collection, or a negative number.
  double sizeOf(const Expr &e, const Frame &frame) const {
    if (e.kind == "name")
      for (size_t i = 0; i < frame.f.params.size(); ++i)
        if (frame.f.params[i].name.text == e.token.text)
          if (auto given = frame.sizes.find(i); given != frame.sizes.end()) return given->second;

    const auto type = typeOf(e, frame);

    if (fixedArray(type)) return double(type.extent);
    if (type.sizeHint == Type::HugeHint) return HugeCost;
    if (type.sizeHint) return double(type.sizeHint);

    return -1;
  }
  double bound(const Expr &e, const Frame &frame) const {
    if (e.kind == "int") {
      if (auto constant = frame.a.integerConstants.find(&e); constant != frame.a.integerConstants.end()) return std::stod(constant->second);

      return std::stod(e.token.text);
    }

    if (e.kind == "." && e.token.text == "Len" && frame.a.collectionMembers.count(&e)) return sizeOf(*e.args[0], frame);

    return -1;
  }
  double iterations(const Stmt &s, const Frame &frame) const {
    double count = -1;

    if (s.kind == "foreach") count = sizeOf(*s.expr, frame);
    else if (s.kind == "rangefor") {
      const auto low = bound(*s.expr->args[0], frame), high = bound(*s.expr->args[1], frame);

      if (low >= 0 && high >= 0) count = std::fabs(high - low) + 1;
    } else if ((s.kind == "for" || s.kind == "while") && s.expr && s.expr->kind == "binary" && s.expr->args.size() == 2) {
      const auto &op = s.expr->token.text;

      if (op == "<" || op == "<=" || op == ">" || op == ">=" || op == "!=") {
        count = bound(*s.expr->args[1], frame);

        if (count < 0) count = bound(*s.expr->args[0], frame);
      }
    }

    return count < 0 ? DefaultCount : count;
  }
  double expression(const Expr &e, const Frame &frame) {
    double cost = 0;

    if (e.kind == "call") {
      if (auto target = frame.a.callTargets.find(&e); target != frame.a.callTargets.end()) cost += call(e, target->second, frame);
    }

    if (e.receiver) cost += expression(*e.receiver, frame);
    for (const auto &child : e.args) cost += expression(*child, frame);

    return cost;
  }
  double body(const std::vector<std::unique_ptr<Stmt>> &statements, Frame &frame) {
    double cost = 0;

    for (const auto &s : statements) cost += statement(*s, frame);

    return cost;
  }
  double statement(const Stmt &s, Frame &frame) {
    double cost = 1;

    if (s.expr) cost += expression(*s.expr, frame);
    if (s.target) cost += expression(*s.target, frame);
    if (s.initializer) cost += statement(*s.initializer, frame);
    if (s.step) cost += statement(*s.step, frame);

    cost += body(s.body, frame) + body(s.alternate, frame);
    const bool loop = s.kind == "for" || s.kind == "while" || s.kind == "do" || s.kind == "foreach" || s.kind == "rangefor";
    return loop ? cost * iterations(s, frame) : cost;
  }
  double function(const std::string &name, const Sizes &sizes) {
    const auto found = index.find(name);

    if (found == index.end()) return 1; // Native or imported: unknown, and never scheduled.
    if (active.count(name)) return HugeCost; // Recursion: assume the work is large.

    if (sizes.empty())
      if (auto known = memo.find(name); known != memo.end()) return known->second;

    active.insert(name);
    const auto &f = program[found->second];
    const auto &a = analyses[found->second];
    auto frame = frameFor(f, a, sizes);
    const auto cost = std::min(body(f.body, frame), MaxCost);
    active.erase(name);

    // Inside a recursion the result depends on who is on the stack.
    if (sizes.empty() && active.empty()) memo[name] = cost;

    return cost;
  }
  // A frame for f with the type of every local its body declares.
  Frame frameFor(const Function &f, const FunctionAnalysis &a, const Sizes &sizes) const {
    Frame frame{f, a, sizes, {}};
    auto noExpr = [](const Expr &) {};
    auto collect = [&](const Stmt &s) {
      if (s.kind == "let" || s.kind == "own")
        if (auto type = a.localTypes.find(&s); type != a.localTypes.end()) frame.locals[s.name] = type->second;
    };

    for (const auto &s : f.body) visitStatement(*s, noExpr, collect);

    return frame;
  }
  double call(const Expr &e, const std::string &target, const Frame &frame) {
    Sizes sizes;

    if (symbols.count(target)) {
      const auto &signature = symbols.at(target);
      const size_t offset = signature.method ? 1 : 0;

      for (size_t i = 0; i < e.args.size(); ++i)
        if (i + offset < signature.parameters.size() && signature.parameters[i + offset].reference)
          if (const auto size = sizeOf(*e.args[i], frame); size >= 0) sizes[i + offset] = size;
    }

    return function(target, sizes);
  }

public:
  CostModel(const Program &program, const std::vector<FunctionAnalysis> &analyses, const Symbols &symbols)
      : program(program), analyses(analyses), symbols(symbols) {
    for (size_t i = 0; i < program.size(); ++i)
      if (program[i].nativeSymbol.empty()) index[program[i].name.text] = i;
  }
  double estimate(const Expr &e, const std::string &target, const Function &f, const FunctionAnalysis &a) {
    static const Sizes none;
    return call(e, target, frameFor(f, a, none));
  }
};
// About what starting a thread costs, in the model's simple operations. A loop
// with no estimate of its length is assumed to clear it.
constexpr double TaskThreshold = 1000;

class Scheduler {
  const Function &function;
  const EffectTable &table;
  FunctionAnalysis &analysis;
  CostModel &cost;
  Recording recorded;

  // An external or unknown effect, or leaving the sequence, must observe every
  // earlier task as finished. This keeps output, traps, and nontermination in order.
  bool barrier(const Stmt &s) const {
    const auto &effects = recorded.statements.at(&s);

    if (effects.unknown || effects.external) return true;

    bool found = false;
    auto expr = [&](const Expr &e) {
      if (e.kind == "lambda" || e.kind == "duckCall" || e.kind == "invoke" || e.kind == "markupTake" ||
          e.kind == "scopeActive" || analysis.indirectCalls.count(&e))
        found = true;
    };
    auto stmt = [&](const Stmt &child) {
      if (child.kind == "break" || child.kind == "continue" || child.kind == "return" || child.kind == "returnOwner" ||
          child.kind == "scopeStart" || child.kind == "scopeEnd" || child.kind == "scopedBody" ||
          analysis.terminatingLoops.count(&child))
        found = true;
    };
    visitStatement(s, expr, stmt);
    return found;
  }
  // The task's own footprint: arguments are evaluated before it starts.
  EffectSet taskEffects(const Stmt &s) const {
    EffectSet effects = recorded.calls.at(s.expr.get());

    if (auto local = recorded.declared.find(&s); local != recorded.declared.end()) effects.writes.insert(local->second);

    return effects;
  }
  bool spawnable(const Stmt &s) {
    if ((s.kind != "let" && s.kind != "expr") || !s.expr || s.expr->kind != "call") return false;
    if (s.kind == "let" && !plainValue(analysis.localTypes.at(&s))) return false;
    if (s.expr->receiver && !s.expr->receiver->addressable()) return false;

    const auto target = analysis.callTargets.find(s.expr.get());

    if (target == analysis.callTargets.end() || !recorded.calls.count(s.expr.get())) return false;

    const auto summary = table.find(target->second);

    if (summary == table.end() || !summary->second.threadSafe || summary->second.unknown || summary->second.external) return false;
    if (recorded.calls.at(s.expr.get()).unknown) return false;

    return cost.estimate(*s.expr, target->second, function, analysis) >= TaskThreshold;
  }
  void list(const std::vector<std::unique_ptr<Stmt>> &statements) {
    for (const auto &s : statements) nested(*s);

    std::vector<std::pair<const Stmt *, EffectSet>> tasks;

    for (size_t i = 0; i + 1 < statements.size(); ++i) {
      const auto &s = *statements[i];

      if (!spawnable(s)) continue;

      // Starting a task that the next statement immediately joins only adds overhead.
      const auto &next = *statements[i + 1];
      auto effects = taskEffects(s);

      if (barrier(next) || effects.conflicts(recorded.statements.at(&next))) continue;

      analysis.taskSpawns.insert(&s);
      tasks.emplace_back(&s, std::move(effects));
    }

    if (tasks.empty()) return;

    std::set<const Stmt *> started;

    for (const auto &s : statements) {
      if (barrier(*s)) analysis.taskBarriers.insert(s.get());

      auto &waits = analysis.taskConflicts[s.get()];

      for (const auto &[task, effects] : tasks)
        if (started.count(task) && effects.conflicts(recorded.statements.at(s.get()))) waits.insert(task);

      if (analysis.taskSpawns.count(s.get())) started.insert(s.get());
    }
  }
  void nested(const Stmt &s) {
    // A `${ }` body runs strictly in order, on the thread holding its locks.
    if (s.kind == "ordered") return;
    // Only statement lists spawn; for-initializers, steps, and a Switch's
    // macro argument bindings run in place.
    if (!s.body.empty()) list(s.body);
    if (!s.alternate.empty() && s.kind != "switch") list(s.alternate);
  }

public:
  Scheduler(const Function &function, const EffectTable &table, FunctionAnalysis &analysis, CostModel &cost,
            const Symbols &symbols, bool library)
      : function(function), table(table), analysis(analysis), cost(cost) {
    EffectWalker(function, analysis, symbols, table, library, &recorded).run();
  }
  void run() { list(function.body); }
};
} // namespace

EffectTable summarizeEffects(const Program &program, const std::vector<FunctionAnalysis> &analyses,
                             const Symbols &symbols, bool entry) {
  EffectTable table;
  std::map<std::string, size_t> index;

  for (size_t i = 0; i < program.size(); ++i) {
    const auto &f = program[i];
    auto &summary = table[f.name.text];

    if (!f.nativeSymbol.empty()) {
      summary.unknown = true; // Whatever native code touches is invisible here.
      continue;
    }

    index[f.name.text] = i;
    bool safe = candidate(f, symbols);

    for (const auto &[expr, type] : analyses[i].expressionTypes)
      safe = safe && taskData(type, symbols) && expr->kind != "duckCall" && expr->kind != "invoke";

    for (const auto &[stmt, type] : analyses[i].localTypes) {
      (void)stmt;
      safe = safe && taskData(type, symbols);
    }

    summary.threadSafe = safe;
  }

  // Least fixpoint: effects only grow, so recursive groups settle on their union.
  for (bool changed = true; changed;) {
    changed = false;

    for (const auto &[name, i] : index) {
      auto effects = EffectWalker(program[i], analyses[i], symbols, table, !entry, nullptr).run();

      for (auto *set : {&effects.reads, &effects.writes})
        for (auto it = set->begin(); it != set->end();)
          it = it->kind == Resource::Local ? set->erase(it) : std::next(it);

      auto &summary = table[name];

      if (summary.reads.size() == effects.reads.size() && summary.writes.size() == effects.writes.size() &&
          summary.unknown == effects.unknown && summary.external == effects.external &&
          summary.locks == effects.locks && summary.exclusive == effects.exclusive)
        continue;

      static_cast<EffectSet &>(summary) = std::move(effects);
      changed = true;
    }
  }

  // Greatest fixpoint: one unsafe callee taints everything that reaches it.
  for (bool changed = true; changed;) {
    changed = false;

    for (const auto &[name, i] : index) {
      auto &summary = table[name];

      if (!summary.threadSafe) continue;

      for (const auto &[call, target] : analyses[i].callTargets) {
        (void)call;
        const auto callee = table.find(target);

        if (callee != table.end() && callee->second.threadSafe) continue;

        summary.threadSafe = false;
        changed = true;
        break;
      }
    }
  }

  return table;
}

void planOrderedBlocks(const Function &function, const EffectTable &effects, const Symbols &symbols,
                       FunctionAnalysis &analysis, bool entry) {
  if (!function.nativeSymbol.empty()) return;

  Recording recording;
  EffectWalker(function, analysis, symbols, effects, !entry, &recording).run();
  analysis.orderedLocks = std::move(recording.ordered);
}

void scheduleTasks(const Program &program, size_t index, const EffectTable &effects, const Symbols &symbols,
                   std::vector<FunctionAnalysis> &analyses, bool entry) {
  const auto &function = program[index];

  if (!function.nativeSymbol.empty()) return;

  CostModel cost(program, analyses, symbols);
  Scheduler(function, effects, analyses[index], cost, symbols, !entry).run();
}
} // namespace duck

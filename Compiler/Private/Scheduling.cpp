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
  const std::set<std::string> &overridden; // From the module's SchedulingIndex.
  Recording *recording;
  // Every summary this walk read, so the fixpoint knows whom a change affects.
  std::set<std::string> *consulted = nullptr;
  EffectTable::const_iterator consult(const std::string &key) const {
    if (consulted) consulted->insert(key);

    return table.find(key);
  }
  std::vector<std::map<std::string, Place>> scopes;
  std::map<std::string, std::pair<Token, Type>> locals; // By unique id.
  std::vector<std::set<std::string> *> activeBlocks;    // Locals declared inside each open `${ }`.
  std::set<std::string> lockedLocals;                   // Locals some inner block locks by address.
  // A stored closure's resources are resolved at its declaration, before shadowing.
  bool checkingGroups = false;
  bool inBranch = false;
  std::set<const Stmt *> checkedGroups;
  std::set<std::string> branchNames, otherUses;
  std::set<size_t> boxedParameters; // Separate captured locals own distinct boxes.
  std::map<std::string, EffectSet> callableLocals;
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

    if (e.kind == ExprKind::Name) {
      if (!checkingGroups && captured(e.token.text)) return std::nullopt;
      if (auto *local = find(e.token.text)) return *local;
      const auto *receiver = find("this");
      if (!receiver) return std::nullopt;

      MemberPath member;

      if (!symbols.findField(receiver->type.record, e.token.text, member) || !member.field->scope.empty()) return std::nullopt;

      auto self = *find("this");
      extend(self, member);
      return self;
    }

    if (e.kind == ExprKind::Member) {
      auto base = place(*e.args[0]);

      if (!base || !recordValue(base->type)) return std::nullopt;

      MemberPath member;

      if (!symbols.findField(base->type.record, e.token.text, member) || !member.field->scope.empty()) return std::nullopt;

      extend(*base, member);
      return base;
    }

    if (e.kind == ExprKind::Index && !a.markupIndexed.count(&e)) {
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
    if (auto found = place(e); found && (!inBranch || taskData(found->type, symbols))) current.reads.insert(found->resource);
    else current.unknown = true;
  }
  void write(const Expr &e, bool alsoRead) {
    auto found = place(e);

    if (!found || (inBranch && !taskData(found->type, symbols)) ||
        (recordValue(found->type) && symbols.destructible(found->type.record))) {
      current.unknown = true;
      return;
    }

    current.writes.insert(found->resource);

    if (alsoRead) current.reads.insert(found->resource);
  }
  bool overridable(const std::string &key) const {
    if (!dispatched(key, true)) return false;
    if (library) return true;

    return overridden.count(methodKey(key.substr(0, key.rfind('.')), memberName(key))) != 0;
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
    const TokenText &op = e.token.text;

    if (a.reinitializations.count(&e) || a.completions.count(&e) || a.reflections.count(&e) ||
        a.indirectCalls.count(&e) || a.elementNames.count(&e)) {
      current.unknown = true;
      return;
    }

    if (a.nativeInPlace.count(&e)) {
      // The target of a pointer receiver is not known here.
      if (a.expressionTypes.at(e.receiver.get()).depth) current.unknown = true;
      else write(*e.receiver, true);

      for (const auto &argument : e.args) expression(*argument);

      return;
    }

    if (nativeMath(e.castType) || a.nativeMethods.count(&e)) { // Pure: it only reads its operands.
      if (e.receiver) expression(*e.receiver);
      for (const auto &argument : e.args) expression(*argument);

      return;
    }

    if (auto built = a.constructions.find(&e); built != a.constructions.end() || recordValue(e.castType)) {
      const auto type = a.expressionTypes.at(&e);

      if (recordValue(type) && symbols.runtimeDefaults(type.record)) current.unknown = true;
      if (built == a.constructions.end()) return;

      for (const auto &part : built->second) {
        const auto callee = consult(part.key);

        if (callee == table.end() || (inBranch && !callee->second.threadSafe)) {
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

    if (op == Tok::Print || op == Tok::TestFail || op == Tok::TestShow) current.external = true;
    if (e.receiver && memberName(op) == "Clear" && !a.callTargets.count(&e)) return write(*e.receiver, true);

    // File methods do I/O, so they stay in order with other external effects.
    if (const auto file = a.fileCalls.find(&e); file != a.fileCalls.end()) {
      current.external = true;
      write(*e.receiver, true);

      for (const auto &argument : e.args)
        if (file->second == "Read" && e.args.size() == 1) write(*argument, false);
        else expression(*argument);

      return;
    }
    if (op == Tok::IsValid && !e.receiver) return read(*e.args[0]);

    const auto target = a.callTargets.find(&e);

    if (target == a.callTargets.end()) {
      if (e.receiver) current.unknown = true;
      for (const auto &argument : e.args) expression(*argument);

      return;
    }

    const auto callee = consult(target->second);

    if (callee == table.end() || !symbols.count(target->second)) {
      current.unknown = true;

      for (const auto &argument : e.args) expression(*argument);

      return;
    }

    std::map<size_t, std::optional<Place>> bound;

    if (op == Tok::Exec && !e.receiver) { // Its path names the script; the script takes no arguments.
      current.merge(apply(callee->second, bound));
      return;
    }

    if (inBranch && !callee->second.threadSafe) current.unknown = true;
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
    if (a.threadQueries.count(&e)) {
      if (e.token.text == Tok::Status) expression(*e.args[0]->args[1]);
      return;
    }
    if (inBranch) {
      const auto type = a.expressionTypes.find(&e);
      if (type != a.expressionTypes.end() && !taskData(type->second, symbols)) current.unknown = true;
    }
    switch (e.kind) { // Constants.
    case ExprKind::Int:
    case ExprKind::Real:
    case ExprKind::Bool:
    case ExprKind::Char:
    case ExprKind::Text:
    case ExprKind::Null:
    case ExprKind::TypeMember:
    case ExprKind::CompileTime: return;
    default: break;
    }

    if (e.kind == ExprKind::Name || e.kind == ExprKind::Member || e.kind == ExprKind::Index) {
      if (a.enumMembers.count(&e) || a.integerConstants.count(&e) || a.functionValues.count(&e)) return;
      if (a.collectionMembers.count(&e)) return read(*e.args[0]);
      if (a.enumProperties.count(&e)) return expression(*e.args[0]);

      if (a.markupLookups.count(&e)) {
        current.unknown = true;
        return;
      }

      return read(e);
    }

    if (e.kind == ExprKind::Call) return call(e);

    if (e.kind == ExprKind::Unary && (e.token.text == Tok::Ampersand || e.token.text == Tok::Star)) {
      current.unknown = true;
      return;
    }

    if (e.kind == ExprKind::FieldInitializer) return expression(*e.args[0]);

    if (e.kind == ExprKind::Binary || e.kind == ExprKind::Unary || e.kind == ExprKind::Conditional || e.kind == ExprKind::Cast || e.kind == ExprKind::Construct ||
        e.kind == ExprKind::Initializer || e.kind == ExprKind::Entry || e.kind == ExprKind::Removal || e.kind == ExprKind::Range) {
      for (const auto &child : e.args) expression(*child);

      return;
    }

    if (e.kind == ExprKind::MacroValue) {
      scopes.emplace_back();

      for (const auto &binding : e.bindings) statement(*binding);

      expression(*e.args[0]);
      scopes.pop_back();
      return;
    }

    if (e.kind == ExprKind::Mutation) {
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
    std::optional<EffectSet> callable;
    if (checkingGroups && funcValue(type) && s.expr && branchNames.count(s.name) &&
        (s.constant || !otherUses.count(s.name)))
      callable = branchEffects(*s.expr);
    if (inBranch && !taskData(type, symbols)) current.unknown = true;
    const auto local = declare(name, type);
    if (callable) callableLocals.emplace(local.root, std::move(*callable));
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
    if (auto loop = a.enumLoops.find(&s); loop != a.enumLoops.end()) {
      // Members are constants: the loop reads nothing but its own variables.
      Type type = Type::Enum;
      type.record = loop->second;
      scopes.emplace_back();
      auto name = s.token;
      name.text = s.name;
      declare(name, type);

      if (!s.key.empty()) {
        name.text = s.key;
        declare(name, Type::Int);
      }

      list(s.body);
      scopes.pop_back();
      return;
    }

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
  EffectSet branchEffects(const Expr &e) {
    EffectSet effects;
    effects.unknown = true;
    if (const auto target = a.functionValues.find(&e); target != a.functionValues.end()) {
      const auto found = consult(target->second);
      if (found != table.end() && found->second.threadSafe) return found->second;
      return effects;
    }
    if (e.kind == ExprKind::Name) {
      if (auto *p = find(e.token.text)) {
        const auto known = callableLocals.find(p->resource.root);
        if (known != callableLocals.end()) return known->second;
      }
    }
    if (!e.lambda || !e.lambda->params.empty() ||
        !(e.lambda->result == Type::Void || plainValue(e.lambda->result))) return effects;

    std::map<std::string, Place> captures;
    for (const auto &capture : a.lambdas.at(&e).captures) {
      auto *p = find(capture.token.text);
      if (!p || !taskData(capture.type, symbols)) return effects;
      captures.emplace(capture.token.text, *p);
    }
    std::set<std::string> outerLocals;
    for (const auto &[name, place] : captures)
      if (place.resource.kind == Resource::Local) outerLocals.insert(place.resource.root);
    auto enclosing = std::move(scopes);
    scopes = {std::move(captures)};
    auto outer = std::move(current);
    current = {};
    const bool previous = std::exchange(inBranch, true);
    list(e.lambda->body);
    inBranch = previous;
    effects = std::move(current);
    for (auto *set : {&effects.reads, &effects.writes})
      for (auto it = set->begin(); it != set->end();)
        it = it->kind == Resource::Local && !outerLocals.count(it->root) ? set->erase(it) : std::next(it);
    current = std::move(outer);
    scopes = std::move(enclosing);
    return effects;
  }
  // Distinct reference parameters can designate the same storage, including
  // a static or a subobject passed by the caller.
  std::string conflict(const EffectSet &first, const EffectSet &second) const {
    auto meets = [&](const auto &left, const auto &right) -> std::string {
      for (const auto &x : left)
        for (const auto &y : right) {
          if (x.overlaps(y)) {
            const auto &r = x.path.size() >= y.path.size() ? x : y;
            std::string name;
            Type type;
            if (r.kind == Resource::Local) {
              const auto &local = locals.at(r.root);
              name = local.first.text;
              type = local.second;
            } else if (r.kind == Resource::Static) {
              name = r.root;
              type = symbols.statics.at(r.root).type;
            } else {
              if (r.parameter >= f.params.size()) return "a captured reference";
              name = f.params[r.parameter].name.text;
              type = f.params[r.parameter].type;
            }
            for (auto index : r.path) {
              const auto &field = symbols.records.at(type.record).fields.at(index);
              name += "." + field.name.text;
              type = field.type;
            }
            return "'" + name + "'";
          }
          const bool xBox = x.kind == Resource::Parameter && boxedParameters.count(x.parameter);
          const bool yBox = y.kind == Resource::Parameter && boxedParameters.count(y.parameter);
          if ((xBox && (yBox || y.kind == Resource::Static)) || (yBox && x.kind == Resource::Static)) continue;
          if (x.kind != Resource::Local && y.kind != Resource::Local &&
              (x.kind == Resource::Parameter || y.kind == Resource::Parameter) &&
              (x.kind != y.kind || x.parameter != y.parameter)) return "potentially aliased references";
        }
      return {};
    };
    auto resource = meets(first.writes, second.reads);
    if (resource.empty()) resource = meets(first.reads, second.writes);
    if (resource.empty()) resource = meets(first.writes, second.writes);
    return resource;
  }
  void parallelGroup(const Stmt &s) {
    if (!checkingGroups) {
      current.unknown = true; // Synchronize with any automatic tasks on entry.
      return;
    }
    checkedGroups.insert(&s);
    std::vector<EffectSet> branches;
    for (const auto &branch : s.labels) {
      auto effects = branchEffects(*branch);
      if (effects.unknown || effects.external)
        fail(branch->token, "cannot prove P branch " + std::to_string(branches.size() + 1) + " is safe for a worker");
      for (size_t i = 0; i < branches.size(); ++i)
        if (auto resource = conflict(branches[i], effects); !resource.empty())
          fail(branch->token, "P branches " + std::to_string(i + 1) + " and " +
                              std::to_string(branches.size() + 1) + " have conflicting accesses to " + resource);
      current.merge(effects);
      branches.push_back(std::move(effects));
    }
  }
  void effects(const Stmt &s) {
    if (s.kind == StmtKind::Parallel) return parallelGroup(s);

    if (s.kind == StmtKind::Ordered) {
      if (checkingGroups) return list(s.body);
      return ordered(s);
    }
    if (s.kind == StmtKind::Block) return list(s.body);

    if (s.kind == StmtKind::Return) {
      if (s.expr) expression(*s.expr);

      return;
    }

    if (s.kind == StmtKind::Break || s.kind == StmtKind::Continue) return;
    if (s.kind == StmtKind::Expr) return expression(*s.expr);
    if (s.kind == StmtKind::Let) return letEffects(s);

    if (assignment(s.kind)) {
      if (s.expr) expression(*s.expr);

      if (s.target->kind == ExprKind::Guard || a.properties.count(s.target.get())) {
        current.unknown = true;
        return;
      }

      return write(*s.target, s.kind != StmtKind::Assign);
    }

    if (s.kind == StmtKind::If) {
      expression(*s.expr);
      list(s.body);
      list(s.alternate);
      return;
    }

    if (s.kind == StmtKind::While || s.kind == StmtKind::Do) {
      if (s.expr) expression(*s.expr);

      list(s.body);
      return;
    }

    if (s.kind == StmtKind::For) return forEffects(s);
    if (s.kind == StmtKind::RangeFor) return rangeForEffects(s);
    if (s.kind == StmtKind::ForEach) return forEachEffects(s);
    if (s.kind == StmtKind::Switch) return switchEffects(s);

    if (s.kind == StmtKind::Case) {
      for (const auto &label : s.labels) expression(*label);

      list(s.body);
      return;
    }

    current.unknown = true; // own, moveInto, markup, scopes, and whatever comes next.
  }

public:
  EffectWalker(const Function &f, const FunctionAnalysis &a, const Symbols &symbols, const EffectTable &table,
               bool library, const std::set<std::string> &overridden, Recording *recording)
      : f(f), a(a), symbols(symbols), table(table), library(library), overridden(overridden),
        recording(recording) {}
  void recordReads(std::set<std::string> *into) { consulted = into; }
  void validateGroups() {
    checkingGroups = true;
    std::set<const Expr *> operands;
    for (auto *group : a.parallelGroups)
      for (const auto &branch : group->labels) {
        operands.insert(branch.get());
        if (branch->kind == ExprKind::Name) branchNames.insert(branch->token.text);
      }
    // A mutable local used only as a branch operand still has its initializer.
    // Any other use can pass it by reference or replace the callable.
    auto expr = [&](const Expr &e) {
      if (e.kind == ExprKind::Name && !operands.count(&e)) otherUses.insert(e.token.text);
    };
    auto stmt = [](const Stmt &) {};
    for (const auto &s : f.body) visitStatement(*s, expr, stmt);
    for (const auto &[lambda, info] : a.lambdas)
      for (const auto &s : lambda->lambda->body) visitStatement(*s, expr, stmt);
    run();
    // Lambdas outside a group are opaque to the automatic scheduler. Check
    // groups in their bodies too, treating borrowed inputs as possible aliases.
    for (const auto &[lambda, info] : a.lambdas) {
      bool needed = false;
      auto group = [&](const Stmt &s) {
        if (s.kind == StmtKind::Parallel && !checkedGroups.count(&s)) needed = true;
      };
      auto ignore = [](const Expr &) {};
      for (const auto &s : lambda->lambda->body) visitStatement(*s, ignore, group);
      if (!needed) continue;
      EffectWalker inner(*lambda->lambda, a, symbols, table, library, overridden, nullptr);
      inner.consulted = consulted;
      inner.checkingGroups = true;
      inner.branchNames = branchNames;
      inner.otherUses = otherUses;
      inner.scopes.emplace_back();
      size_t index = lambda->lambda->params.size();
      for (const auto &capture : info.captures) {
        Place p;
        p.resource.kind = Resource::Parameter;
        if (capture.kind == FunctionAnalysis::Capture::Box) inner.boxedParameters.insert(index);
        p.resource.parameter = index++;
        p.type = capture.type;
        inner.declareAlias(capture.token, p);
      }
      if (info.bound) {
        Place self;
        self.resource.kind = Resource::Parameter;
        self.resource.parameter = index;
        self.type = Type::Record;
        self.type.record = info.record;
        inner.scopes.back()["this"] = self;
      }
      inner.run();
      checkedGroups.insert(inner.checkedGroups.begin(), inner.checkedGroups.end());
    }
    for (auto *group : a.parallelGroups)
      if (!checkedGroups.count(group)) fail(group->token, "cannot establish the capture effects of this P group");
  }
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
  const std::map<std::string, size_t> &index;
  std::set<std::string> active;
  std::map<std::string, double> memo;

  Type typeOf(const Expr &e, const Frame &frame) const {
    if (auto found = frame.a.expressionTypes.find(&e); found != frame.a.expressionTypes.end() && arrayValue(found->second))
      return found->second;

    if (e.kind == ExprKind::Name) {
      for (const auto &p : frame.f.params)
        if (p.name.text == e.token.text) return p.type;

      if (auto local = frame.locals.find(e.token.text); local != frame.locals.end()) return local->second;

      MemberPath member;

      if (frame.f.method && symbols.findField(frame.f.params[0].type.record, e.token.text, member)) return member.field->type;
      if (auto found = frame.a.staticFields.find(&e); found != frame.a.staticFields.end()) return symbols.statics.at(found->second).type;
    }

    if (e.kind == ExprKind::Member) {
      if (auto found = frame.a.staticFields.find(&e); found != frame.a.staticFields.end()) return symbols.statics.at(found->second).type;

      const auto base = typeOf(*e.args[0], frame);
      MemberPath member;

      if (recordValue(base) && symbols.findField(base.record, e.token.text, member)) return member.field->type;
    }

    if (e.kind == ExprKind::Index) {
      const auto base = typeOf(*e.args[0], frame);

      if (arrayValue(base)) return *base.element;
    }

    return Type::Void;
  }
  // The expected element count of a collection, or a negative number.
  double sizeOf(const Expr &e, const Frame &frame) const {
    if (e.kind == ExprKind::Name)
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
    if (e.kind == ExprKind::Int) {
      if (auto constant = frame.a.integerConstants.find(&e); constant != frame.a.integerConstants.end()) return std::stod(constant->second);

      return std::stod(e.token.text);
    }

    if (e.kind == ExprKind::Member && e.token.text == Tok::Len && frame.a.collectionMembers.count(&e)) return sizeOf(*e.args[0], frame);

    return -1;
  }
  double iterations(const Stmt &s, const Frame &frame) const {
    double count = -1;

    if (s.kind == StmtKind::ForEach && !frame.a.enumLoops.count(&s)) count = sizeOf(*s.expr, frame);
    else if (s.kind == StmtKind::RangeFor) {
      const auto low = bound(*s.expr->args[0], frame), high = bound(*s.expr->args[1], frame);

      if (low >= 0 && high >= 0) count = std::fabs(high - low) + 1;
    } else if ((s.kind == StmtKind::For || s.kind == StmtKind::While) && s.expr && s.expr->kind == ExprKind::Binary && s.expr->args.size() == 2) {
      const TokenText &op = s.expr->token.text;

      if (op == Tok::Less || op == Tok::LessEqual || op == Tok::Greater || op == Tok::GreaterEqual || op == Tok::NotEqual) {
        count = bound(*s.expr->args[1], frame);

        if (count < 0) count = bound(*s.expr->args[0], frame);
      }
    }

    return count < 0 ? DefaultCount : count;
  }
  double expression(const Expr &e, const Frame &frame) {
    double cost = 0;

    if (e.kind == ExprKind::Call) {
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
    const bool loop = s.kind == StmtKind::For || s.kind == StmtKind::While || s.kind == StmtKind::Do || s.kind == StmtKind::ForEach || s.kind == StmtKind::RangeFor;
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
      if (s.kind == StmtKind::Let || s.kind == StmtKind::Own)
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
  CostModel(const Program &program, const std::vector<FunctionAnalysis> &analyses, const Symbols &symbols,
            const std::map<std::string, size_t> &index)
      : program(program), analyses(analyses), symbols(symbols), index(index) {}
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
      if (e.kind == ExprKind::Lambda || e.kind == ExprKind::DuckCall || e.kind == ExprKind::Invoke || e.kind == ExprKind::MarkupTake ||
          e.kind == ExprKind::ScopeActive || analysis.indirectCalls.count(&e))
        found = true;
    };
    auto stmt = [&](const Stmt &child) {
      if (child.kind == StmtKind::Break || child.kind == StmtKind::Continue || child.kind == StmtKind::Return || child.kind == StmtKind::ReturnOwner ||
          child.kind == StmtKind::ScopeStart || child.kind == StmtKind::ScopeEnd || child.kind == StmtKind::ScopedBody ||
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
    if ((s.kind != StmtKind::Let && s.kind != StmtKind::Expr) || !s.expr || s.expr->kind != ExprKind::Call) return false;
    if (s.kind == StmtKind::Let && !plainValue(analysis.localTypes.at(&s))) return false;
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
    if (s.kind == StmtKind::Ordered) return;
    // Only statement lists spawn; for-initializers, steps, and a Switch's
    // macro argument bindings run in place.
    if (!s.body.empty()) list(s.body);
    if (!s.alternate.empty() && s.kind != StmtKind::Switch) list(s.alternate);
  }

public:
  Scheduler(const Function &function, const EffectTable &table, FunctionAnalysis &analysis, CostModel &cost,
            const Symbols &symbols, bool library, const std::set<std::string> &overridden)
      : function(function), table(table), analysis(analysis), cost(cost) {
    EffectWalker(function, analysis, symbols, table, library, overridden, &recorded).run();
  }
  void run() { list(function.body); }
};
} // namespace

SchedulingIndex indexScheduling(const Program &program, const Symbols &symbols) {
  SchedulingIndex result;

  for (size_t i = 0; i < program.size(); ++i)
    if (program[i].nativeSymbol.empty()) result.functions[program[i].name.text] = i;

  // A method is overridden when a container that derives from its owner,
  // directly or through other parents, has a method of the same name.
  for (const auto &[key, signature] : symbols) {
    (void)signature;
    const auto dot = key.rfind('.');

    if (dot == std::string::npos || dot + 1 >= key.size() || key[dot + 1] != '$') continue;

    const auto record = key.substr(0, dot);

    if (!symbols.records.count(record)) continue;

    const auto member = key.substr(dot + 2);
    std::set<std::string> seen;
    std::vector<std::string> pending{record};

    while (!pending.empty()) {
      const auto found = symbols.records.find(pending.back());
      pending.pop_back();

      if (found == symbols.records.end()) continue;

      for (const auto &parent : found->second.parents)
        if (parent.text != record && seen.insert(parent.text).second) {
          result.overridden.insert(methodKey(parent.text, member));
          pending.push_back(parent.text);
        }
    }
  }

  return result;
}

EffectTable summarizeEffects(const Program &program, const std::vector<FunctionAnalysis> &analyses,
                             const Symbols &symbols, bool entry, const SchedulingIndex *shared) {
  std::optional<SchedulingIndex> own;

  if (!shared) shared = &own.emplace(indexScheduling(program, symbols));

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
      safe = safe && taskData(type, symbols) && expr->kind != ExprKind::DuckCall && expr->kind != ExprKind::Invoke;

    for (const auto &[stmt, type] : analyses[i].localTypes) {
      (void)stmt;
      safe = safe && taskData(type, symbols);
    }

    summary.threadSafe = safe;
  }

  // Least fixpoint: effects only grow, so recursive groups settle on their
  // union. A function is walked again only when a summary it read has grown,
  // instead of rewalking the whole module until a pass changes nothing.
  // Passes go in name order like a full sweep, but skip functions whose inputs
  // are unchanged. A reader later in the current pass sees the new summary anyway.
  std::map<std::string, std::set<std::string>> readers; // Summary -> functions that read it.
  std::set<std::string> pending;

  for (const auto &[name, i] : index) {
    (void)i;
    pending.insert(name);
  }

  while (!pending.empty()) {
    std::set<std::string> next;

    for (auto current = pending.begin(); current != pending.end(); ++current) {
      const auto &name = *current;
      const auto i = index.at(name);
      std::set<std::string> consulted;
      EffectWalker walker(program[i], analyses[i], symbols, table, !entry, shared->overridden, nullptr);
      walker.recordReads(&consulted);
      auto effects = walker.run();

      for (const auto &callee : consulted) readers[callee].insert(name);

      for (auto *set : {&effects.reads, &effects.writes})
        for (auto it = set->begin(); it != set->end();)
          it = it->kind == Resource::Local ? set->erase(it) : std::next(it);

      auto &summary = table[name];

      if (summary.reads.size() == effects.reads.size() && summary.writes.size() == effects.writes.size() &&
          summary.unknown == effects.unknown && summary.external == effects.external &&
          summary.locks == effects.locks && summary.exclusive == effects.exclusive)
        continue;

      static_cast<EffectSet &>(summary) = std::move(effects);

      if (const auto affected = readers.find(name); affected != readers.end())
        for (const auto &reader : affected->second)
          if (reader <= name || !pending.count(reader)) next.insert(reader);
    }

    pending = std::move(next);
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
                       FunctionAnalysis &analysis, bool entry, bool parallel, const SchedulingIndex *shared) {
  if (!function.nativeSymbol.empty()) return;

  std::optional<SchedulingIndex> own;

  if (!shared) shared = &own.emplace(indexScheduling(Program{}, symbols));

  Recording recording;
  EffectWalker(function, analysis, symbols, effects, !entry, shared->overridden, &recording).run();
  analysis.orderedLocks = std::move(recording.ordered);
  if (!analysis.parallelGroups.empty()) {
    EffectWalker(function, analysis, symbols, effects, !entry, shared->overridden, nullptr).validateGroups();
    analysis.parallelWorkers = parallel;
  }
}

void scheduleTasks(const Program &program, size_t index, const EffectTable &effects, const Symbols &symbols,
                   std::vector<FunctionAnalysis> &analyses, bool entry, const SchedulingIndex *shared) {
  const auto &function = program[index];

  if (!function.nativeSymbol.empty()) return;

  std::optional<SchedulingIndex> own;

  if (!shared) shared = &own.emplace(indexScheduling(program, symbols));

  CostModel cost(program, analyses, symbols, shared->functions);
  Scheduler(function, effects, analyses[index], cost, symbols, !entry, shared->overridden).run();
}
} // namespace duck

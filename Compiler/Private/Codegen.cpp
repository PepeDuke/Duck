#include "Literals.h"
#include "Pipeline.h"
#include "Unicode.h"
#include "WideInteger.h"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <optional>
#include <sstream>
#include <utility>
namespace duck {
namespace {
// The layouts of a type descriptor, its member table, and its attribute
// lists. Runtime lookup reads the same fields; see metaRuntimeIR.
//   type:   string name, name name, member count, members
//   member: string name, name name, cstring signature, string declaring,
//           name declaring, flags, tag count, tags, function, offset, table
//   tag:    cstring enum name, member index
const char *const typeLayout = "{ ptr, ptr, i64, ptr }";
const char *const memberLayout = "{ ptr, ptr, ptr, ptr, ptr, i32, i32, ptr, ptr, i64, ptr }";
const char *const tagLayout = "{ ptr, i32 }";
enum MemberField { MemberName, MemberNameBytes, MemberSignature, MemberDeclaring, MemberDeclaringBytes,
                   MemberFlags, MemberTagCount, MemberTags, MemberFunction, MemberOffset, MemberTable };
std::string irType(Type t) {
  if (funcValue(t)) return "ptr";
  if (fixedArray(t)) return "[" + std::to_string(t.extent) + " x " + irType(*t.element) + "]";
  if (collectionValue(t) || dynamicArray(t) || fileValue(t)) return "ptr";

  if (integral(t))
    return "i" + std::to_string(integerWidth(t));

  if (t.depth) return "ptr";
  if (recordValue(t)) return "%struct." + t.record;
  if (enumValue(t)) return "i32"; // Member index.

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
  case Type::Char:
  case Type::Date:
    return "i32";
  case Type::Time:
    return "i64";
  case Type::DateTime:
    return "{ i32, i64 }";
  case Type::Big:
  case Type::UBig:
    return "ptr";
  case Type::Tiny:
  case Type::UTiny:
    return "{ i256, i32 }";
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
// LLVM lowers 65..128-bit division and floating conversions to compiler-rt
// calls that the MSVC-target link does not provide, but expands wider types
// inline. Those operations therefore run at 256 bits and truncate the result.
bool needsWideOperation(const Type &t) {
  return integerWidth(t) > 64 && integerWidth(t) <= 128;
}
Type wideOperationType(const Type &t) { return Type::integer(256, unsignedInteger(t)); }
struct Value {
  Type type;
  std::string value;
};
std::string enumTextSymbol(const std::string &name, size_t index) {
  return "@enum." + name + ".value." + std::to_string(index);
}
// A member's name as an immortal managed name, for d.Name.
std::string enumLabelSymbol(const std::string &name, size_t index) {
  return "@enum." + name + ".label." + std::to_string(index);
}
// An immortal managed string or name literal (reference count -1) holding
// text.text, which is UTF-8. string stores UTF-16 code units, name bytes.
std::string managedTextGlobal(const std::string &symbol, const Token &text, Type type) {
  std::vector<uint32_t> units;

  if (type == Type::Name) {
    for (unsigned char byte : text.text) units.push_back(byte);
  } else
    for (auto cp : decodeUTF8(text)) {
      if (cp < 0x10000) units.push_back(cp);
      else {
        cp -= 0x10000;
        units.push_back(0xD800 | (cp >> 10));
        units.push_back(0xDC00 | (cp & 1023));
      }
    }

  const std::string unit = type == Type::String ? "i16" : "i8";
  const auto array = "[" + std::to_string(units.size() + 1) + " x " + unit + "]";
  std::string ir = symbol + " = private unnamed_addr constant { i64, i64, " + array +
                   " } { i64 -1, i64 " + std::to_string(units.size()) + ", " + array + " [";

  for (auto value : units) ir += unit + " " + std::to_string(value) + ", ";

  return ir + unit + " 0] }, align 8\n";
}
// Managed storage is never null. A string, name, or big field or static
// without an initializer holds an immortal empty value: "" or zero, defined
// privately in each module that needs it.
std::string managedZero(const Type &type) {
  return type == Type::String ? "@duck$empty.string" : type == Type::Name ? "@duck$empty.name"
                                                                          : "@duck$big.zero";
}
// True when any field, nested container, or parent of the record has a
// default. A managed field always has one.
bool hasDefaults(const Symbols &symbols, const std::string &record) {
  for (const auto &field : symbols.records.at(record).fields)
    if (field.initializer || managedValue(field.type) ||
        (recordValue(field.type) && hasDefaults(symbols, field.type.record)))
      return true;

  return false;
}
std::string defaultTextSymbol(const std::string &record, const Parameter &field) {
  return "@duck$text." + record + "." + field.name.text;
}
// A record's default value as an LLVM constant. Its string and name
// literals are emitted by emitRecords in every module that uses the record.
std::string recordConstant(const Symbols &symbols, const std::string &record) {
  if (!hasDefaults(symbols, record)) return "zeroinitializer";

  std::string value = "{ ";
  const auto &fields = symbols.records.at(record).fields;

  for (size_t i = 0; i < fields.size(); ++i) {
    const auto &field = fields[i];
    value += (i ? ", " : "") + irType(field.type) + " ";

    // A runtime default starts from the value an uninitialized field has.
    if (recordValue(field.type)) value += recordConstant(symbols, field.type.record);
    else if (!field.initializer || field.runtimeDefault)
      value += managedValue(field.type) ? managedZero(field.type) : "zeroinitializer";
    else if (field.initialValue.empty()) value += defaultTextSymbol(record, field);
    else value += field.initialValue;
  }

  return value + " }";
}
std::string tableSymbol(const DispatchTable &table) {
  std::string symbol = "@\"duck$vt." + table.first;

  for (auto index : table.second) symbol += "$" + std::to_string(index);

  return symbol + "\"";
}
std::string tableType(const Symbols &symbols, const std::string &record) {
  std::string type = "{ i64, i64, ptr";
  const auto names = symbols.dispatchNames(record).size();

  for (size_t i = 0; i < symbols.records.at(record).parents.size(); ++i) type += ", ptr";
  for (size_t i = 0; i < names; ++i) type += ", ptr, i64, ptr";

  return type + " }";
}
// The byte offset of the parent state at path within record, as a constant.
std::string pathOffset(const std::string &record, const std::vector<size_t> &path) {
  if (path.empty()) return "0";

  std::string indices;

  for (auto index : path) indices += ", i32 " + std::to_string(index);

  return "ptrtoint (ptr getelementptr (%struct." + record + ", ptr null, i32 0" + indices + ") to i64)";
}
// A for-each in progress. `current` is an i64 slot holding the position of
// the element the loop variable names, or -1 once it has been removed, and
// `next` the position to visit after it.
struct Iteration {
  const Expr *collection;
  Type type;
  bool reverse;
  std::string current, next;
};
struct Binding {
  Type type;
  std::string value;
  bool constant = false;
  bool owned = false;
  bool scopedBorrow = false;
  size_t order = 0; // Declaration order; cleanup runs in reverse.
  // For this and container reference parameters: the dispatch table of the
  // object the storage belongs to.
  std::string table;
  // A for-each variable has no storage of its own: variable() finds the
  // current element at each use, so a collection that moved stays safe.
  const Iteration *iteration = nullptr;
  bool iterationKey = false; // The key variable of `for k, v : pair`.
  // A local shared with lambdas: the address of the word holding its box.
  std::string box;
  const Parameter *scoped = nullptr; // A field bound by name that lives in a named scope.
  bool scopeLoop = false; // Not a variable: an iteration of a [@Name] loop, ended when its scope exits.
  bool pin = false; // Not a variable: an active use, ended when its scope exits.
  bool lent = false; // Received from a function that may lend; it may not own its object.
  bool orderedLock = false; // Not a variable: a `${ }` block's locks, released when its scope exits.
};
class Generator {
  const Symbols &functions;
  const FunctionAnalysis &analysis;
  std::string symbolPrefix;
  // A container's dispatch names and table layout, built once per function.
  // Every dynamically dispatched call needs them, and building them walks all
  // of the container's methods, which dominated lowering for big containers.
  std::map<std::string, std::vector<std::string>> dispatchNameCache;
  std::map<std::string, std::string> tableLayoutCache;
  const std::vector<std::string> &dispatchNames(const std::string &record) {
    auto found = dispatchNameCache.find(record);
    if (found == dispatchNameCache.end()) found = dispatchNameCache.emplace(record, functions.dispatchNames(record)).first;
    return found->second;
  }
  const std::string &tableLayout(const std::string &record) {
    auto found = tableLayoutCache.find(record);
    if (found == tableLayoutCache.end()) found = tableLayoutCache.emplace(record, tableType(functions, record)).first;
    return found->second;
  }
  std::vector<std::map<std::string, Binding>> scopes;
  std::ostringstream globals, code, allocations;
  struct Loop {
    std::string exit, next;
    size_t scopeDepth;
    bool isSwitch = false; // A break target only; continue passes through it.
    std::string scope;     // The descriptor of a [@Name] loop's scope.
  };
  std::vector<Loop> loops;
  // A task record is {handle, arguments..., result} in the caller's frame.
  struct PendingTask {
    std::string name, record, layout;
    size_t result;
    Type resultType;
    const Expr *call;
    Binding destination; // Empty name: a call statement, with nothing to store.
    const Stmt *origin;
  };
  std::vector<std::vector<PendingTask>> pending;
  std::ostringstream thunks;
  std::string currentBlock = "entry";
  int next = 0, strings = 0, tasks = 0;
  bool usesTextRuntime = false, usesHeapRuntime = false, usesTaskRuntime = false, usesLockRuntime = false;
  bool usesCalendarRuntime = false, usesWideRuntime = false;
  bool usesBigRuntime = false, usesDecimalRuntime = false, usesCollectionRuntime = false;
  bool usesFuncRuntime = false, usesMetaRuntime = false, usesMarkupRuntime = false, usesScopeRuntime = false;
  bool usesFileRuntime = false;
  std::map<std::string, std::string> mathImports;
  std::optional<std::string> tolerance; // The (t)== being lowered, if any.
  std::set<std::string> scopeNames;
  Type functionResult = Type::Void; // What a return from the function being lowered gives.
  bool guardScopedPlace = false;    // The scoped place lowered next may be absent.
  std::map<std::string, std::string> scopeInitials;
  int lambdaCount = 0;
  // The uses begun by the statement being lowered, and whether its tracked
  // dereferences need them: only code that can run during the statement can
  // destroy the object.
  std::vector<std::string> activeUses;
  bool pinning = false;
  // The payload of each link a guarded call has already checked.
  std::map<const Expr *, std::string> guardedLinks;
  // The payload each bound `if p` evaluated, while lowering its branch.
  std::map<const Stmt *, std::string> checkedPayloads;
  struct UseFrame {
    size_t mark;
    bool pinning;
  };
  UseFrame beginUses(bool runs) {
    const UseFrame frame{activeUses.size(), pinning};
    pinning = runs;
    return frame;
  }
  void endUses(const UseFrame &frame) {
    while (activeUses.size() > frame.mark) {
      emit("call void @duck.heap$unpin(ptr " + activeUses.back() + ")");
      activeUses.pop_back();
    }

    pinning = frame.pinning;
  }
  // Whether evaluating e can run Duck code, which could release an object
  // this statement is using.
  bool runsCode(const Expr &e) const {
    if (e.kind == "invoke" || e.kind == "duckCall" || e.kind == "duckCheck" || e.kind == "duckExists" ||
        e.kind == "mutation" || e.kind == "moveCheck" || analysis.properties.count(&e) ||
        analysis.indirectCalls.count(&e))
      return true;

    if (e.kind == "call" && !analysis.mathCalls.count(&e) && !analysis.nativeMethods.count(&e) &&
        (e.receiver || analysis.callTargets.count(&e) || analysis.constructions.count(&e) ||
         analysis.reinitializations.count(&e)))
      return true;

    if (e.receiver && runsCode(*e.receiver)) return true;

    for (const auto &binding : e.bindings)
      if (!binding->expr || runsCode(*binding->expr)) return true;

    for (const auto &argument : e.args)
      if (runsCode(*argument)) return true;

    return false;
  }
  bool statementRuns(const Stmt &s) const {
    if ((s.expr && runsCode(*s.expr)) || (s.target && runsCode(*s.target))) return true;
    if (!s.target) return false;

    // Overwriting storage can run a destructor.
    const auto type = analysis.expressionTypes.at(s.target.get());
    return type.role == Type::Owner || functions.needsCleanup(type);
  }
  // Whether a dereference of e needs a use of its own. A local owner does
  // not: only this function can clear it.
  bool pinnable(const Expr &e, const Type &type) const {
    if (!e.addressable() || e.kind == "unary" || analysis.properties.count(&e) ||
        analysis.collectionMembers.count(&e) || analysis.staticFields.count(&e) || analysis.enumProperties.count(&e))
      return false;

    if (e.kind != "name") return true;

    const auto binding = findBinding(e.token);
    return !binding.scopedBorrow &&
           (type.role != Type::Owner || binding.lent || analysis.receiverStorage.count(&e) != 0);
  }
  // A null word that a `heap$pin` can record its block in, once per entry.
  std::string useRecord() {
    auto record = fresh();
    allocations << "  " << record << " = alloca ptr\n  store ptr null, ptr " << record << '\n';
    return record;
  }
  std::string pinCall(const Binding &slot, const std::string &record) {
    usesHeapRuntime = true;
    auto payload = fresh();
    emit(payload + " = call ptr @duck.heap$pin(ptr " + slot.value + ", i1 " + ownerFlag(slot.type) + ", ptr " + record +
         ")");
    return payload;
  }
  // Begins a use of the object e refers to, for the rest of the statement.
  std::string pinUse(const Expr &e) {
    const auto slot = location(e);
    const auto record = useRecord();
    activeUses.push_back(record);
    const auto payload = pinCall(slot, record);
    auto valid = fresh();
    emit(valid + " = icmp ne ptr " + payload + ", null");
    require(valid);
    return payload;
  }
  Value condition(const Expr &e) {
    const auto frame = beginUses(runsCode(e));
    auto v = expression(e);
    endUses(frame);
    return v;
  }
  std::set<std::string> cleanups, destroys;
  std::map<std::string, std::string> helpers;
  std::set<DispatchTable> tables;
  std::set<std::string> types;
  // The receiver's container inside a method, and whether the method takes
  // a dispatch table (%vt); constructors and destructors use their own
  // container's table, so their calls on this never reach an override.
  std::string selfRecord;
  bool dispatching = false;
  std::string table(const DispatchTable &t) {
    tables.insert(t);
    return tableSymbol(t);
  }
  std::string selfTable() { return dispatching ? "%vt" : table({selfRecord, {}}); }
  // The table of the parent state at path below a table of `record`.
  std::string childTable(std::string vt, std::string record, const std::vector<size_t> &path) {
    for (auto index : path) {
      auto slot = fresh(), child = fresh();
      emit(slot + " = getelementptr " + tableLayout(record) + ", ptr " + vt + ", i32 0, i32 " +
           std::to_string(3 + index));
      emit(child + " = load ptr, ptr " + slot);
      vt = child;
      record = functions.records.at(record).fields[index].type.record;
    }

    return vt;
  }
  // The table-carrying binding (this, or a container reference parameter)
  // an expression starts at, and the parent-view path it takes from there,
  // as in this.Animal; false for storage of a known exact type.
  bool tableRoot(const Expr &e, std::vector<size_t> &path, Binding &root) {
    if (e.kind == "name") {
      if (analysis.staticFields.count(&e)) return false;

      root = variable(e.token);
      return !root.table.empty() && recordValue(root.type);
    }

    if (e.kind != "." || analysis.staticFields.count(&e)) return false;

    const auto base = analysis.expressionTypes.find(e.args[0].get());

    if (base == analysis.expressionTypes.end() || !recordValue(base->second)) return false;
    if (!tableRoot(*e.args[0], path, root)) return false;

    MemberPath member;
    const auto &record = base->second.record;

    if (!functions.findField(record, e.token.text, member) || !member.field->parentView) return false;

    path.insert(path.end(), member.indices.begin(), member.indices.end());
    return true;
  }
  // A method's receiver, evaluated once: its storage, or a temporary copy of
  // a value, and the table-carrying binding it starts at, if any.
  struct Receiver {
    std::string address;
    bool rooted = false;
    Binding root;
    std::vector<size_t> view;
    Value temporary{Type::Void, ""};
  };
  Receiver receiver(const Expr *e, const Token &at) {
    Receiver r;

    if (!e) {
      Token self = at;
      self.text = "this";
      r.root = variable(self);
      r.rooted = true;
      r.address = r.root.value;
      return r;
    }

    r.rooted = tableRoot(*e, r.view, r.root);

    if (analysis.expressionTypes.at(e).depth) r.address = dereference(*e).value;
    else if (e->addressable() && !analysis.properties.count(e)) r.address = location(*e).value;
    else {
      auto temporary = expression(*e);
      r.address = fresh();
      allocations << "  " << r.address << " = alloca " << irType(temporary.type) << '\n';
      emit("store " + irType(temporary.type) + " " + temporary.value + ", ptr " + r.address);
      r.temporary = {temporary.type, r.address};
    }

    return r;
  }
  // The receiver and dispatch-table arguments for calling method `target`
  // on r. A call on this, or on a container reference parameter, runs the
  // final overrider for the object: its slot in the table gives the
  // function (set as callee), its receiver's offset within the object, and
  // its table. Other calls are direct: the receiver's exact type is known,
  // or a base call through this.Parent bypasses the overrides below it.
  // `inherited` is the receiver's container and the parent-view path to the
  // method's declaring state, for an inherited method.
  std::string receiverArguments(const Receiver &r, const std::string &target,
                                const std::pair<std::string, std::vector<size_t>> *inherited,
                                std::string &callee) {
    if (r.rooted && r.view.empty() && dispatched(target, true)) {
      const auto &record = r.root.type.record;
      const auto &type = tableLayout(record);
      const auto &names = dispatchNames(record);
      const auto first = 3 + functions.records.at(record).parents.size() +
                         3 * (std::find(names.begin(), names.end(), memberName(target)) - names.begin());
      auto field = [&](size_t index, const std::string &ir) {
        auto slot = fresh(), value = fresh();
        emit(slot + " = getelementptr " + type + ", ptr " + r.root.table + ", i32 0, i32 " +
             std::to_string(index));
        emit(value + " = load " + ir + ", ptr " + slot);
        return value;
      };
      const auto own = field(0, "i64"), function = field(first, "ptr"), offset = field(first + 1, "i64"),
                 table = field(first + 2, "ptr");
      auto back = fresh(), base = fresh(), self = fresh();
      emit(back + " = sub i64 0, " + own);
      emit(base + " = getelementptr i8, ptr " + r.root.value + ", i64 " + back);
      emit(self + " = getelementptr i8, ptr " + base + ", i64 " + offset);
      callee = function;
      return "ptr " + self + ", ptr " + table;
    }

    // An inherited method receives the declaring parent's embedded state.
    auto address = r.address;

    if (inherited && !inherited->second.empty()) {
      Type owner = Type::Record;
      owner.record = inherited->first;
      auto adjusted = fresh();
      emit(adjusted + " = getelementptr " + irType(owner) + ", ptr " + address + ", i32 0" +
           indexList(inherited->second));
      address = adjusted;
    }

    std::string args = "ptr " + address;

    if (!dispatched(target, true)) return args;

    std::vector<size_t> path;

    if (inherited) path = inherited->second;

    if (r.rooted) {
      auto view = r.view;
      view.insert(view.end(), path.begin(), path.end());
      return args + ", ptr " + childTable(r.root.table, r.root.type.record, view);
    }

    const auto owner = inherited ? inherited->first : target.substr(0, target.rfind('.'));
    return args + ", ptr " + table({owner, path});
  }
  // Evaluates a call's receiver and arguments, in source order, into the
  // argument list of its call instruction. `arguments` collects by-value
  // arguments for the caller to release.
  std::string callArguments(const Expr &e, const std::string &target, const Signature &f, std::string &callee,
                            Value &temporaryReceiver, std::vector<Value> &arguments) {
    std::string args;

    if (f.method) {
      const auto r = receiver(e.receiver.get(), e.token);
      const auto inherited = analysis.receiverPaths.find(&e);
      args = receiverArguments(r, target, inherited != analysis.receiverPaths.end() ? &inherited->second : nullptr,
                               callee);
      temporaryReceiver = r.temporary;
    }

    for (size_t i = 0; i < e.args.size(); ++i) {
      const auto &parameter = f.parameters[i + (f.method ? 1 : 0)];
      const auto &argument = *e.args[i];

      if (!args.empty())
        args += ", ";

      if (parameter.reference) {
        args += referenceArgument(argument, parameter, !f.nativeSymbol.empty());
      } else {
        auto v = expression(argument);
        arguments.push_back(v);

        if (!f.nativeSymbol.empty() && enumValue(v.type)) v = enumUnderlying(v);

        if (!f.nativeSymbol.empty() && managedText(v.type)) {
          auto data = fresh();
          emit(data + " = getelementptr i8, ptr " + v.value + ", i64 16");
          v.value = data; // Read-only native borrow; owner stays alive through the call.
        }

        args += irType(v.type) + " " + v.value;
      }
    }

    return args;
  }
  // Calls a property accessor on r, passing `value` to a setter.
  // Calls callee and returns the register holding its result, or empty for void.
  std::string emitCall(const Type &result, const std::string &callee, const std::string &args) {
    const std::string id = result == Type::Void ? "" : fresh();
    emit((id.empty() ? "" : id + " = ") + "call " + irType(result) + " " + callee + "(" + args + ")");
    return id;
  }
  void releaseTemporary(const Receiver &r) {
    if (!r.temporary.value.empty()) releaseBinding({r.temporary.type, r.temporary.value});
  }
  // Evaluates one call argument into args. A value argument is also kept in
  // arguments, for the caller to release after the call.
  void appendArgument(const Expr &argument, const Parameter &parameter, std::string &args, std::vector<Value> &arguments) {
    if (parameter.reference) {
      args += ", " + referenceArgument(argument, parameter, false);
      return;
    }

    const auto v = expression(argument);
    arguments.push_back(v);
    args += ", " + irType(v.type) + " " + v.value;
  }
  Value accessorCall(const Receiver &r, const std::string &owner, const std::string &key,
                     const std::vector<size_t> &path, const Value *value = nullptr) {
    const auto &f = functions.at(key);
    std::string callee = "@duck." + key;
    const std::pair<std::string, std::vector<size_t>> inherited{owner, path};
    auto args = receiverArguments(r, key, &inherited, callee);

    if (value) args += ", " + irType(value->type) + " " + value->value;

    const auto id = emitCall(f.result, callee, args);
    return {f.result, id};
  }
  // A reference argument: its address and, for a container parameter, the
  // table of the object it belongs to. A derived container passes the
  // state of the parameter's parent, with that state's table.
  std::string referenceArgument(const Expr &argument, const Parameter &parameter, bool native) {
    const auto storage = location(argument);
    auto address = storage.value;
    const bool tabled = carriesTable(parameter) && !native;
    std::string vt;

    if (tabled) {
      std::vector<size_t> path;
      Binding root;
      vt = tableRoot(argument, path, root) ? childTable(root.table, root.type.record, path)
                                           : table({storage.type.record, {}});
    }

    if (auto up = analysis.upcasts.find(&argument); up != analysis.upcasts.end()) {
      auto adjusted = fresh();
      emit(adjusted + " = getelementptr " + irType(storage.type) + ", ptr " + address + ", i32 0" +
           indexList(up->second));
      address = adjusted;

      if (tabled) vt = childTable(vt, storage.type.record, up->second);
    }

    return "ptr " + address + (tabled ? ", ptr " + vt : "");
  }
  // Replacing a whole object through a reference that may hold only a
  // parent's state of a larger object would leave the rest inconsistent,
  // so it traps unless the table describes a whole object.
  void requireWhole(const Binding &binding) {
    if (binding.table.empty() || binding.table[0] != '%' || !recordValue(binding.type)) return;

    auto slot = fresh(), depth = fresh(), whole = fresh();
    emit(slot + " = getelementptr " + tableLayout(binding.type.record) + ", ptr " + binding.table +
         ", i32 0, i32 1");
    emit(depth + " = load i64, ptr " + slot);
    emit(whole + " = icmp eq i64 " + depth + ", 0");
    require(whole);
  }
  // A generated initializer writes storage that was never constructed, so
  // overwriting it destroys nothing.
  bool initializer = false;
  // Element slots a guarded assignment already found, keyed by its index.
  std::map<const Expr *, std::string> guardedSlots;
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
  // Applies `runtime` (retain or release) to each managed value in a record
  // value, through nested containers and parent state.
  void eachManaged(Value value, const std::string &runtime) {
    for (size_t i = 0; i < functions.records.at(value.type.record).fields.size(); ++i) {
      const auto &field = functions.records.at(value.type.record).fields[i];

      if (!functions.managed(field.type)) continue;

      auto part = fresh();
      emit(part + " = extractvalue " + irType(value.type) + " " + value.value + ", " + std::to_string(i));

      if (recordValue(field.type)) eachManaged({field.type, part}, runtime);
      else if (funcValue(field.type)) {
        usesFuncRuntime = true;
        emit("call void @duck.cell$" + runtime + "(ptr " + part + ")");
      } else if (trackedAlias(field.type)) {
        usesHeapRuntime = true;
        emit("call void @duck.heap$" + runtime + "(ptr " + part + ")");
      } else {
        usesTextRuntime = true;
        emit("call void @duck.text." + runtime + "(ptr " + part + ")");
      }
    }
  }
  void retain(Value value) {
    if (recordValue(value.type) && functions.managed(value.type)) return eachManaged(value, "retain");

    if (funcValue(value.type)) {
      usesFuncRuntime = true;
      return emit("call void @duck.cell$retain(ptr " + value.value + ")");
    }

    if (!managedValue(value.type)) return;

    usesTextRuntime = true;
    emit("call void @duck.text.retain(ptr " + value.value + ")");
  }
  void destroyAt(const std::string &record, const std::string &address) {
    destroys.insert(record);
    usesCollectionRuntime = true; // Destroy routines may free collection fields.
    emit("call void @duck$destroy." + record + "(ptr " + address + ")");
  }
  // The allocation size of a type, as a constant.
  static std::string sizeOf(const Type &type) {
    return "ptrtoint (ptr getelementptr (" + irType(type) + ", ptr null, i32 1) to i64)";
  }
  // The release function for collection slots of a type, or null.
  std::string slotRelease(const Type &type) {
    if (elementValue(type)) {
      usesMarkupRuntime = true;
      return "@duck.markup$node";
    }

    if (trackedAlias(type)) {
      usesHeapRuntime = true;
      return "@duck.heap$slot";
    }

    if (!managedValue(type)) return "null";

    usesTextRuntime = true;
    return "@duck$slot.release";
  }
  // How a pair compares keys of a type: null compares bytewise.
  std::string keyEquality(const Type &type) {
    if (type == Type::String) return "@duck$key.string";
    if (type == Type::Name) return "@duck$key.name";

    if (bigInteger(type)) {
      usesBigRuntime = true;
      return "@duck$key.big";
    }

    return "null";
  }
  // The function that cleans up one array element in place, or null:
  // releasing text and big values, or a container's heap cleanup, which runs
  // its destructor and releases its fields.
  std::string elementCleanup(const Type &type) {
    if (trackedAlias(type)) return slotRelease(type);

    if (fileValue(type)) {
      usesFileRuntime = true;
      return "@duck.file$drop";
    }

    if (funcValue(type)) {
      usesFuncRuntime = true;
      return "@duck$slot.func";
    }

    if (managedValue(type)) {
      usesCollectionRuntime = true;
      return slotRelease(type);
    }

    if (recordValue(type) && functions.needsCleanup(type)) {
      cleanups.insert(type.record);
      usesTextRuntime = true;
      return "@duck$cleanup." + type.record;
    }

    if (collectionValue(type) || (arrayValue(type) && functions.needsCleanup(type))) {
      // An element that is itself an array: a dynamic one frees its storage
      // and elements, a fixed one cleans up each element in place. A boxed
      // local holding a queue, stack, or pair frees it the same way.
      std::string name = "@duck$cleanup.array.";

      for (unsigned char c : typeName(type))
        if (std::isalnum(c)) name += char(c);
        else name += "_x" + std::to_string(int(c));

      if (helpers.count(name)) return name;

      helpers[name] = "";
      usesCollectionRuntime = true;
      std::string body = "define internal void " + name + "(ptr %slot) {\nentry:\n";

      if (collectionValue(type)) {
        body += "  %c = load ptr, ptr %slot\n";

        if (type.base == Type::Pair)
          body += "  call void @duck.pair$free(ptr %c, ptr " + slotRelease(*type.key) + ", ptr " +
                  slotRelease(*type.element) + ")\n";
        else
          body += "  call void @duck.seq$free(ptr %c, ptr " + slotRelease(*type.element) + ")\n";
      } else if (dynamicArray(type)) {
        body += "  %c = load ptr, ptr %slot\n  call void @duck.seq$free(ptr %c, ptr " +
                elementCleanup(*type.element) + ")\n";
      } else {
        const auto [leaf, count] = flatten(type);
        body += "  call void @duck.array$each(ptr %slot, i64 " + std::to_string(count) + ", i64 " + sizeOf(leaf) +
                ", ptr " + elementCleanup(leaf) + ")\n";
      }

      helpers[name] = body + "  ret void\n}\n";
      return name;
    }

    return "null";
  }
  // A fixed array's innermost non-fixed element type and how many of them
  // it stores contiguously: int [5][10] is 50 ints.
  static std::pair<Type, unsigned long long> flatten(Type type) {
    unsigned long long count = 1;

    while (fixedArray(type)) {
      count *= type.extent;
      type = *type.element;
    }

    return {type, count};
  }
  void freeArray(const std::string &value, const Type &type) {
    usesCollectionRuntime = true;
    emit("call void @duck.seq$free(ptr " + value + ", ptr " + elementCleanup(*type.element) + ")");
  }
  // Cleans up each element of the fixed array at address.
  void cleanupElements(const std::string &address, const Type &type) {
    usesCollectionRuntime = true;
    emit("call void @duck.array$each(ptr " + address + ", i64 " + std::to_string(type.extent) + ", i64 " +
         sizeOf(*type.element) + ", ptr " + elementCleanup(*type.element) + ")");
  }
  // Gives each element of a zeroed fixed array its default when zero bits
  // are not one: empty text, a zero big, or a container's constant default.
  void fillDefaults(const std::string &address, const Type &type) {
    const auto [element, count] = flatten(type);
    std::string source;

    if (managedValue(element)) {
      Value empty;

      if (bigInteger(element)) empty = bigLiteral("0", element);
      else {
        Token text;
        empty = {element, "@str." + symbolPrefix + "." + std::to_string(strings++)};
        globals << managedTextGlobal(empty.value, text, element);
      }

      source = fresh();
      allocations << "  " << source << " = alloca ptr\n";
      emit("store ptr " + empty.value + ", ptr " + source);
    } else if (recordValue(element) && hasDefaults(functions, element.record))
      source = "@duck$default." + element.record;
    else return;

    usesCollectionRuntime = true;
    emit("call void @duck.array$fill(ptr " + address + ", i64 " + std::to_string(count) + ", i64 " +
         sizeOf(element) + ", ptr " + source + ")");
  }
  // Adds key : value to the pair whose handle is at holder. A new key is
  // appended with its value; an existing key keeps its place and gets the
  // new value, and the duplicate key is released.
  // Adds key : value, or replaces the value of a present key; gives the entry.
  std::string pairPut(const std::string &holder, const Type &type, const Value &key, const Value &value) {
    usesCollectionRuntime = true;
    const auto probe = keySlot(key);
    auto c = fresh(), found = fresh(), missing = fresh();
    emit(c + " = load ptr, ptr " + holder);
    emit(found + " = call ptr @duck.pair$find(ptr " + c + ", ptr " + probe + ", ptr " + keyEquality(*type.key) + ")");
    emit(missing + " = icmp eq ptr " + found + ", null");
    auto add = label(), replace = label(), join = label();
    emit("br i1 " + missing + ", label %" + add + ", label %" + replace);
    startBlock(add);
    auto added = fresh();
    emit(added + " = call ptr @duck.pair$append(ptr " + holder + ", i64 " + sizeOf(*type.key) + ", i64 " +
         sizeOf(*type.element) + ")");
    emit("call void @duck.mem$copy(ptr " + added + ", ptr " + probe + ", i64 " + sizeOf(*type.key) + ")");
    const auto addEnd = currentBlock;
    emit("br label %" + join);
    startBlock(replace);
    release(key);

    if (managedValue(*type.element) || trackedAlias(*type.element)) {
      auto previous = fresh(), old = fresh();
      emit(previous + " = call ptr @duck.pair$value(ptr " + c + ", ptr " + found + ")");
      emit(old + " = load ptr, ptr " + previous);

      if (trackedAlias(*type.element)) {
        usesHeapRuntime = true;
        emit("call void @duck.heap$release(ptr " + old + ")");
      } else
        release({*type.element, old});
    }

    const auto replaceEnd = currentBlock;
    emit("br label %" + join);
    startBlock(join);
    auto entry = fresh(), current = fresh(), slot = fresh();
    emit(entry + " = phi ptr [" + added + ", %" + addEnd + "], [" + found + ", %" + replaceEnd + "]");
    emit(current + " = load ptr, ptr " + holder);
    emit(slot + " = call ptr @duck.pair$value(ptr " + current + ", ptr " + entry + ")");
    emit("store " + irType(value.type) + " " + value.value + ", ptr " + slot);
    return entry;
  }
  // The key p++ adds: 0 for an empty pair, else the last key added plus 1.
  // Trap when that overflows or is already present.
  Value nextKey(const std::string &holder, const Type &type) {
    const auto &keyType = *type.key;
    const auto key = irType(keyType);
    auto c = fresh(), count = fresh(), empty = fresh();
    emit(c + " = load ptr, ptr " + holder);
    emit(count + " = call i64 @duck.pair$length(ptr " + c + ")");
    emit(empty + " = icmp eq i64 " + count + ", 0");
    auto first = label(), after = label(), join = label();
    emit("br i1 " + empty + ", label %" + first + ", label %" + after);
    startBlock(first);
    emit("br label %" + join);
    startBlock(after);
    auto entry = fresh(), last = fresh(), next = fresh(), wrapped = fresh(), fits = fresh();
    emit(entry + " = call ptr @duck.pair$last(ptr " + c + ")");
    emit(last + " = load " + key + ", ptr " + entry);
    emit(next + " = add " + key + " " + last + ", 1");
    emit(wrapped + " = icmp " + (unsignedInteger(keyType) ? "ult " : "slt ") + key + " " + next + ", " + last);
    emit(fits + " = xor i1 " + wrapped + ", true");
    require(fits);
    const auto afterEnd = currentBlock;
    emit("br label %" + join);
    startBlock(join);
    auto chosen = fresh(), found = fresh(), fresh_ = fresh();
    emit(chosen + " = phi " + key + " [0, %" + first + "], [" + next + ", %" + afterEnd + "]");
    const Value result{keyType, chosen};
    emit(found + " = call ptr @duck.pair$find(ptr " + c + ", ptr " + keySlot(result) + ", ptr null)");
    emit(fresh_ + " = icmp eq ptr " + found + ", null");
    require(fresh_);
    return result;
  }
  Value mutateAppend(const Expr &target, const std::string &op, const Expr *operand, bool result, const Type &type) {
    const auto &element = *type.element;
    const bool pair = type.base == Type::Pair;

    if (pair) {
      Value key, value;
      std::string holder;

      if (op == "+=") {
        key = expression(*operand->args[0]);
        value = expression(*operand->args[1]);
        holder = location(target).value;
      } else {
        holder = location(target).value;
        key = nextKey(holder, type);
        value = missingElement(element);
      }

      const auto entry = pairPut(holder, type, key, value);

      if (!result) return {};

      auto c = fresh(), index = fresh();
      emit(c + " = load ptr, ptr " + holder);
      emit(index + " = call i64 @duck.pair$position(ptr " + c + ", ptr " + entry + ")");
      return {Type::Long, index};
    }

    // The value moves into the new last slot; growing moves the existing
    // elements bytewise into the larger buffer.
    const auto value = op == "+=" ? expression(*operand) : missingElement(element);
    const auto holder = location(target).value;
    auto c = fresh(), index = fresh(), slot = fresh();
    emit(c + " = load ptr, ptr " + holder);
    emit(index + " = call i64 @duck.seq$length(ptr " + c + ")");
    emit(slot + " = call ptr @duck.seq$push(ptr " + holder + ", i64 " + sizeOf(element) + ")");
    emit("store " + irType(value.type) + " " + value.value + ", ptr " + slot);
    return {Type::Long, index};
  }
  Value mutateRemoveCurrent(const Expr *operand, bool result, const Type &type, const std::string &holder) {
    const auto &element = *type.element;
    const bool pair = type.base == Type::Pair;
    const auto &iteration = *findBinding(operand->token).iteration;
    auto c = fresh(), position = fresh(), slot = fresh();
    emit(c + " = load ptr, ptr " + holder);
    emit(position + " = load i64, ptr " + iteration.current);
    Value v;

    if (pair) {
      auto entry = fresh();
      emit(entry + " = call ptr @duck.pair$nth(ptr " + c + ", i64 " + position + ")");
      releaseEntryKey(*type.key, entry);
      emit(slot + " = call ptr @duck.pair$value(ptr " + c + ", ptr " + entry + ")");
      v = takeElement(slot, slotRelease(element), element, result);
      emit("call void @duck.pair$remove(ptr " + c + ", ptr " + entry + ")");
    } else {
      emit(slot + " = call ptr @duck.seq$index(ptr " + c + ", i64 " + position + ")");
      v = takeElement(slot, elementCleanup(element), element, result);
      emit("call void @duck.seq$removeAt(ptr " + c + ", i64 " + position + ")");
    }

    emit("store i64 -1, ptr " + iteration.current);

    if (!iteration.reverse) emit("store i64 " + position + ", ptr " + iteration.next);

    return v;
  }
  Value mutatePopLast(bool result, const Type &type, const std::string &holder) {
    const auto &element = *type.element;
    const bool pair = type.base == Type::Pair;
    auto c = fresh(), slot = fresh();
    emit(c + " = load ptr, ptr " + holder);

    if (pair) {
      auto entry = fresh();
      emit(entry + " = call ptr @duck.pair$last(ptr " + c + ")");
      emit(slot + " = call ptr @duck.pair$value(ptr " + c + ", ptr " + entry + ")");
      const auto v = takeElement(slot, slotRelease(element), element, result);
      emit("call void @duck.pair$dropLast(ptr " + c + ", ptr " + slotRelease(*type.key) + ", ptr null)");
      return v;
    }

    const std::string back = type.base == Type::Queue ? "0" : "1";
    emit(slot + " = call ptr @duck.seq$at(ptr " + c + ", i1 " + back + ")");
    const auto v = takeElement(slot, dynamicArray(type) ? elementCleanup(element) : slotRelease(element), element, result);
    emit("call void @duck.seq$drop(ptr " + c + ", i1 " + back + ", ptr null)");
    return v;
  }
  Value mutateRemoveAt(const Expr *operand, bool result, const Type &type, const std::string &holder) {
    const auto &element = *type.element;
    const bool pair = type.base == Type::Pair;
    const auto &which = *operand->args[0];

    if (pair) {
      std::vector<const Expr *> coordinates;

      for (const auto &coordinate : operand->args) coordinates.push_back(coordinate.get());

      const auto key = gridValue(type) ? gridKey(type, coordinates) : expression(which);
      auto c = fresh(), entry = fresh(), found = fresh(), slot = fresh();
      emit(c + " = load ptr, ptr " + holder);
      emit(entry + " = call ptr @duck.pair$find(ptr " + c + ", ptr " + keySlot(key) + ", ptr " +
           keyEquality(*type.key) + ")");
      release(key);
      emit(found + " = icmp ne ptr " + entry + ", null");
      require(found);
      releaseEntryKey(*type.key, entry);
      emit(slot + " = call ptr @duck.pair$value(ptr " + c + ", ptr " + entry + ")");
      const auto v = takeElement(slot, slotRelease(element), element, result);
      emit("call void @duck.pair$remove(ptr " + c + ", ptr " + entry + ")");
      return v;
    }

    const auto index = expression(which);
    auto c = fresh(), slot = fresh();
    emit(c + " = load ptr, ptr " + holder);
    emit(slot + " = call ptr @duck.seq$index(ptr " + c + ", i64 " + index.value + ")");
    const auto v = takeElement(slot, elementCleanup(element), element, result);
    emit("call void @duck.seq$removeAt(ptr " + c + ", i64 " + index.value + ")");
    return v;
  }
  void releaseEntryKey(const Type &key, const std::string &entry) {
    if (const auto release = slotRelease(key); release != "null") emit("call void " + release + "(ptr " + entry + ")");
  }
  // Releases or moves out the element in slot.
  Value takeElement(const std::string &slot, const std::string &cleanup, const Type &element, bool result) {
    if (result) {
      auto v = fresh();
      emit(v + " = load " + irType(element) + ", ptr " + slot);
      return {element, v};
    }

    if (cleanup != "null") emit("call void " + cleanup + "(ptr " + slot + ")");

    return {};
  }
  // Lowers a collection mutation (see Analyzer::collectionMutation). With
  // result, gives the inserted index for += and ++ or the removed element
  // for -- and -= [i]; a removed element moves out instead of being released.
  Value mutate(const Expr &target, const std::string &op, const Expr *operand, bool result) {
    usesCollectionRuntime = true;
    const auto type = analysis.expressionTypes.at(&target);

    if (op == "+=" || op == "++") return mutateAppend(target, op, operand, result, type);

    const auto holder = location(target).value;

    // c -= x removes the element a for-each is on. A forward loop's next
    // element shifts into the removed position; a reverse loop's does not move.
    if (op == "-=" && operand->kind == "name") return mutateRemoveCurrent(operand, result, type, holder);
    if (op == "--") return mutatePopLast(result, type, holder);

    // -= [i] or -= [key]: the element must exist.
    return mutateRemoveAt(operand, result, type, holder);
  }
  // A sequence or pair literal: a fixed array is built as an aggregate, and
  // the others in a fresh handle, adding the entries in order.
  Value literal(const Expr &e, const Type &type) {
    if (fixedArray(type)) {
      std::vector<Value> elements;
      elements.reserve(e.args.size());
      for (const auto &arg : e.args) elements.push_back(expression(*arg));

      // Constant elements go into one constant aggregate and only the others
      // are inserted. A chain of one insertvalue per element copies the whole
      // array each step, and clang -O0 takes minutes on a few thousand of them
      // (embedded SPIR-V, for example).
      const auto constant = [](const std::string &v) { return !v.empty() && v.find('%') == std::string::npos; };
      const auto elementType = irType(*type.element);
      std::string aggregate = "[";
      bool anyConstant = false;

      for (size_t i = 0; i < type.extent; ++i) {
        if (i) aggregate += ", ";

        if (i < elements.size() && constant(elements[i].value)) {
          aggregate += irType(elements[i].type) + " " + elements[i].value;
          anyConstant = true;
        } else {
          aggregate += elementType + " zeroinitializer";
        }
      }

      std::string value = anyConstant ? aggregate + "]" : "zeroinitializer";

      for (size_t i = 0; i < elements.size(); ++i) {
        if (constant(elements[i].value)) continue;

        auto next = fresh();
        emit(next + " = insertvalue " + irType(type) + " " + value + ", " + irType(elements[i].type) + " " +
             elements[i].value + ", " + std::to_string(i));
        value = next;
      }

      return {type, value};
    }

    usesCollectionRuntime = true;
    auto holder = fresh();
    allocations << "  " << holder << " = alloca ptr\n";
    emit("store ptr null, ptr " + holder);

    for (const auto &entry : e.args) {
      if (type.base != Type::Pair) {
        const auto value = expression(*entry);
        auto slot = fresh();
        emit(slot + " = call ptr @duck.seq$push(ptr " + holder + ", i64 " + sizeOf(*type.element) + ")");
        emit("store " + irType(value.type) + " " + value.value + ", ptr " + slot);
        continue;
      }

      Value key;

      if (entry->kind == "entry") key = expression(*entry->args[0]);
      else if (*type.key == Type::Name) {
        key = {Type::Name, "@str." + symbolPrefix + "." + std::to_string(strings++)};
        globals << managedTextGlobal(key.value, entry->token, Type::Name);
      } else
        key = expression(*entry->args[1]);

      const auto value = expression(*(entry->kind == "entry" ? entry->args[1] : entry->args[0]));
      pairPut(holder, type, key, value);
    }

    auto result = fresh();
    emit(result + " = load ptr, ptr " + holder);
    return {type, result};
  }
  void freeCollection(const std::string &value, const Type &type) {
    usesCollectionRuntime = true;

    if (type.base == Type::Pair)
      emit("call void @duck.pair$free(ptr " + value + ", ptr " + slotRelease(*type.key) + ", ptr " +
           slotRelease(*type.element) + ")");
    else emit("call void @duck.seq$free(ptr " + value + ", ptr " + slotRelease(*type.element) + ")");
  }
  // A zero-filled copy of a key, so its padding compares equal bytewise.
  std::string keySlot(const Value &key) {
    auto slot = fresh();
    allocations << "  " << slot << " = alloca " << irType(key.type) << '\n';
    emit("call void @duck.mem$zero(ptr " + slot + ", i64 " + sizeOf(key.type) + ")");
    emit("store " + irType(key.type) + " " + key.value + ", ptr " + slot);
    return slot;
  }
  // Reads a collection slot as a value of its own.
  Value loadSlot(const std::string &slot, const Type &type) {
    auto value = fresh();
    emit(value + " = load " + irType(type) + ", ptr " + slot);

    if (trackedAlias(type)) {
      usesHeapRuntime = true;
      auto payload = fresh();
      emit(payload + " = call ptr @duck.heap$payload(ptr " + value + ", i1 false)");
      return {type, payload};
    }

    retain({type, value});
    return {type, value};
  }
  void release(Value value) {
    if (collectionValue(value.type)) return freeCollection(value.value, value.type);

    if (fileValue(value.type)) {
      usesFileRuntime = true;
      emit("call void @duck.file$close(ptr " + value.value + ")");
      return;
    }

    if (dynamicArray(value.type)) return freeArray(value.value, value.type);

    if (fixedArray(value.type)) {
      if (!functions.needsCleanup(value.type)) return;

      auto slot = fresh();
      allocations << "  " << slot << " = alloca " << irType(value.type) << '\n';
      emit("store " + irType(value.type) + " " + value.value + ", ptr " + slot);
      return cleanupElements(slot, value.type);
    }

    if (recordValue(value.type) && functions.destructible(value.type.record)) {
      auto slot = fresh();
      allocations << "  " << slot << " = alloca " << irType(value.type) << '\n';
      emit("store " + irType(value.type) + " " + value.value + ", ptr " + slot);
      destroyAt(value.type.record, slot);

      if (!functions.managed(value.type)) return;

      value.value = fresh();
      emit(value.value + " = load " + irType(value.type) + ", ptr " + slot);
    }

    if (recordValue(value.type) && functions.managed(value.type)) return eachManaged(value, "release");

    if (funcValue(value.type)) {
      usesFuncRuntime = true;
      return emit("call void @duck.cell$release(ptr " + value.value + ")");
    }

    if (!managedValue(value.type)) return;

    usesTextRuntime = true;
    emit("call void @duck.text.release(ptr " + value.value + ")");
  }
  // Evaluates the runtime defaults of the record at address in layout order,
  // except for skipped field paths. Parents come first in the layout, so each
  // parent is defaulted and then runs its own parts from `calls` (the plan of
  // the record being constructed, at `relative` within it) before the
  // container's fields; nested fields are fully built in place.
  void runDefaults(const std::string &record, const std::string &address,
                   const std::set<std::vector<size_t>> &skip, std::vector<size_t> path,
                   const std::vector<PartCall> &calls = {}, const Expr *call = nullptr,
                   std::vector<size_t> relative = {}) {
    Type type = Type::Record;
    type.record = record;
    const auto &fields = functions.records.at(record).fields;

    for (size_t i = 0; i < fields.size(); ++i) {
      path.push_back(i);
      const auto &field = fields[i];

      if (skip.count(path)) {
      } else if (field.runtimeDefault) {
        emit("call void @duck." + defaultKey(record, field.name.text) + "(ptr " + address + ")");
      } else if (recordValue(field.type) && !field.initializer &&
                 functions.runtimeDefaults(field.type.record)) {
        auto part = fresh();
        emit(part + " = getelementptr " + irType(type) + ", ptr " + address + ", i32 0, i32 " +
             std::to_string(i));

        if (field.parentView) {
          relative.push_back(i);
          runDefaults(field.type.record, part, skip, path, calls, call, relative);
          runOwnParts(field.type, part, calls, call, relative);
          relative.pop_back();
        } else {
          const auto own = automaticParts(field.type.record);
          runDefaults(field.type.record, part, skip, path, own, nullptr, {});
          runOwnParts(field.type, part, own, nullptr, {});
        }
      }

      path.pop_back();
    }
  }
  // A record's default value: zero, or its constant defaults, then its
  // runtime defaults evaluated in place. Skipped fields keep the constant base.
  std::string defaultValue(const Type &type, const std::set<std::vector<size_t>> &skip = {}) {
    if (!recordValue(type)) return "zeroinitializer";

    if (!hasDefaults(functions, type.record)) {
      if (!functions.runtimeDefaults(type.record)) return "zeroinitializer";

      return constructRecord(type, automaticParts(type.record), nullptr, skip);
    }

    auto id = fresh();
    emit(id + " = load " + irType(type) + ", ptr @duck$default." + type.record);

    if (!functions.runtimeDefaults(type.record)) return id;

    return constructRecord(type, automaticParts(type.record), nullptr, skip, id);
  }
  // The zero-argument overload of every part that has one; a declared local
  // may be left missing the others.
  std::vector<PartCall> automaticParts(const std::string &record) const {
    std::vector<PartCall> calls;

    for (const auto &part : functions.parts(record))
      if (!part.automatic.empty()) calls.push_back({part.path, part.automatic, 0, 0});

    return calls;
  }
  // Runs construction parts on the record at address, each on the parent
  // state its path selects, with its slice of call's arguments.
  void runParts(const Type &type, const std::string &address, const std::vector<PartCall> &calls,
                const Expr *call) {
    for (const auto &part : calls) {
      auto self = address;

      if (!part.path.empty()) {
        self = fresh();
        emit(self + " = getelementptr " + irType(type) + ", ptr " + address + ", i32 0" + indexList(part.path));
      }

      const auto &parameters = functions.at(part.key).parameters;
      std::string args = "ptr " + self;
      std::vector<Value> values;

      for (size_t i = 0; i < part.count; ++i) {
        const auto &argument = *call->args[part.first + i];

        if (parameters[i + 1].reference) {
          args += ", " + referenceArgument(argument, parameters[i + 1], false);
          continue;
        }

        auto v = expression(argument);
        values.push_back(v);
        args += ", " + irType(v.type) + " " + v.value;
      }

      emit("call void @duck." + part.key + "(" + args + ")");

      for (const auto &v : values) release(v);
    }
  }
  // Runs the parts in calls that belong to the record at `relative` itself,
  // on that record's storage at address.
  void runOwnParts(const Type &type, const std::string &address, const std::vector<PartCall> &calls,
                   const Expr *call, const std::vector<size_t> &relative) {
    std::vector<PartCall> own;

    for (const auto &part : calls)
      if (part.path == relative) {
        own.push_back(part);
        own.back().path.clear();
      }

    runParts(type, address, own, call);
  }
  // Builds a record in fresh storage: its constant defaults, then its runtime
  // defaults and fully built nested containers in layout order, then its
  // construction parts: parents' first, then its own partial and regular.
  std::string constructRecord(const Type &type, const std::vector<PartCall> &calls, const Expr *call,
                              const std::set<std::vector<size_t>> &skip = {}, std::string base = "") {
    if (base.empty()) {
      base = "zeroinitializer";

      if (hasDefaults(functions, type.record)) {
        base = fresh();
        emit(base + " = load " + irType(type) + ", ptr @duck$default." + type.record);
      }
    }

    auto slot = fresh();
    allocations << "  " << slot << " = alloca " << irType(type) << '\n';
    emit("store " + irType(type) + " " + base + ", ptr " + slot);
    runDefaults(type.record, slot, skip, {}, calls, call, {});
    runOwnParts(type, slot, calls, call, {});
    auto value = fresh();
    emit(value + " = load " + irType(type) + ", ptr " + slot);
    return value;
  }
  static std::string ownerFlag(Type type) { return type.role == Type::Owner ? "true" : "false"; }
  // Whether this owner-role value can turn out to be borrowed at run time.
  bool mayBeBorrowed(const Expr &source) const {
    if (source.kind == "name") return findBinding(source.token).lent;
    if (source.kind != "call") return false;

    const auto target = analysis.callTargets.find(&source);
    return target != analysis.callTargets.end() && functions.at(target->second).lends;
  }
  std::string moveOwner(const Expr &source) {
    auto slot = location(source).value;
    auto word = fresh();
    emit(word + " = load ptr, ptr " + slot);

    if (mayBeBorrowed(source)) emit("call void @duck.heap$owned(ptr " + word + ")");

    emit("store ptr null, ptr " + slot);
    return word;
  }
  std::string aliasWord(const Expr &source) {
    if (source.kind == "null") return "null";

    usesHeapRuntime = true;
    auto binding = location(source);
    auto word = fresh();
    emit(word + " = call ptr @duck.heap$alias(ptr " + binding.value + ", i1 " + ownerFlag(binding.type) + ")");
    return word;
  }
  // A new heap object holding value, as an owner word.
  std::string allocate(const Type &type, const std::string &value) {
    usesHeapRuntime = true;
    auto word = fresh();
    emit(word + " = call ptr @duck.heap$alloc(i64 " + sizeOf(type) + ")");
    emit("store " + irType(type) + " " + value + ", ptr " + word);
    return word;
  }
  // @duck$reach.<record>(object, slot): whether slot lies inside the object
  // or inside anything the object owns. Only owning fields, containers held
  // by value, and dynamic arrays of containers can lead to more storage.
  std::string reachHelper(const std::string &record) {
    const auto name = "@duck$reach." + record;

    if (helpers.count(name)) return name;

    helpers[name] = ""; // Containers can own their own type.
    usesHeapRuntime = usesCollectionRuntime = true;
    Type type = Type::Record;
    type.record = record;
    std::string body = "define internal i1 " + name + "(ptr %self, ptr %slot) {\nentry:\n"
                       "  %none = icmp eq ptr %self, null\n  br i1 %none, label %no, label %range\nrange:\n"
                       "  %a = ptrtoint ptr %self to i64\n  %b = ptrtoint ptr %slot to i64\n"
                       "  %offset = sub i64 %b, %a\n  %inside = icmp ult i64 %offset, " + sizeOf(type) +
                       "\n  br i1 %inside, label %yes, label %f0\n";
    const auto &fields = functions.records.at(record).fields;

    for (size_t i = 0; i < fields.size(); ++i) {
      const auto n = std::to_string(i), following = "f" + std::to_string(i + 1);
      const auto &field = fields[i];
      body += "f" + n + ":\n";
      std::string part = "  %p" + n + " = getelementptr " + irType(type) + ", ptr %self, i32 0, i32 " + n + "\n";
      auto test = [&](const std::string &target, const std::string &object, const std::string &id,
                      const std::string &otherwise) {
        return "  %r" + id + " = call i1 " + reachHelper(target) + "(ptr " + object + ", ptr %slot)\n  br i1 %r" + id +
               ", label %yes, label %" + otherwise + "\n";
      };

      if (field.type.role == Type::Owner && recordValue(field.type.pointee())) {
        body += part + "  %w" + n + " = load ptr, ptr %p" + n + "\n  %o" + n +
                " = call ptr @duck.heap$payload(ptr %w" + n + ", i1 true)\n" +
                test(field.type.record, "%o" + n, n, following);
      } else if (recordValue(field.type) && functions.destructible(field.type.record)) {
        body += part + test(field.type.record, "%p" + n, n, following);
      } else if (dynamicArray(field.type) && recordValue(*field.type.element) &&
                 functions.destructible(field.type.element->record)) {
        body += part + "  %c" + n + " = load ptr, ptr %p" + n + "\n  %n" + n + " = call i64 @duck.seq$length(ptr %c" + n +
                ")\n  br label %h" + n + "\nh" + n + ":\n  %i" + n + " = phi i64 [ 0, %f" + n + " ], [ %k" + n + ", %g" + n +
                " ]\n  %m" + n + " = icmp slt i64 %i" + n + ", %n" + n + "\n  br i1 %m" + n + ", label %b" + n + ", label %" +
                following + "\nb" + n + ":\n  %e" + n + " = call ptr @duck.seq$index(ptr %c" + n + ", i64 %i" + n + ")\n" +
                test(field.type.element->record, "%e" + n, n, "g" + n) + "g" + n + ":\n  %k" + n + " = add i64 %i" + n +
                ", 1\n  br label %h" + n + "\n";
      } else
        body += "  br label %" + following + "\n";
    }

    body += "f" + std::to_string(fields.size()) + ":\n  br label %no\nyes:\n  ret i1 true\nno:\n  ret i1 false\n}\n";
    helpers[name] = body;
    return name;
  }
  // i1 true when moving the object (payload) into the owning field at slot
  // would make it own itself.
  std::string ownershipCycle(const Expr &target, const std::string &slot, const std::string &payload,
                             const std::string &record) {
    const auto base = analysis.expressionTypes.at(target.args[0].get());
    const auto holder = base.depth ? base.pointee().record : base.record;

    if (!functions.canOwn(record, holder)) return "false";

    // Follow embedded containers down to the object holding the slot.
    const Expr *root = target.args[0].get();

    while (root->kind == "." && !analysis.expressionTypes.at(root).depth) root = root->args[0].get();

    if (root->kind == "name") {
      const auto type = analysis.expressionTypes.at(root);
      const auto binding = findBinding(root->token);

      // A local is owned by no field, so nothing can own what it holds.
      if (!type.depth && binding.owned) return "false";

      if (type.role == Type::Owner && binding.owned && !binding.lent && !analysis.receiverStorage.count(root)) {
        auto word = fresh(), own = fresh(), same = fresh();
        emit(word + " = load ptr, ptr " + binding.value);
        emit(own + " = call ptr @duck.heap$payload(ptr " + word + ", i1 true)");
        emit(same + " = icmp eq ptr " + own + ", " + payload);
        return same;
      }
    }

    auto cycle = fresh();
    emit(cycle + " = call i1 " + reachHelper(record) + "(ptr " + payload + ", ptr " + slot + ")");
    return cycle;
  }
  // target :: source. The cycle check runs before anything changes, so a
  // rejected move leaves the source's owner intact. With result, gives false
  // for a rejected move; otherwise a rejected move traps.
  Value moveIntoField(const Expr &target, const Expr &source, bool result) {
    usesHeapRuntime = true;
    const auto type = analysis.expressionTypes.at(&target);
    const auto from = analysis.expressionTypes.at(&source);
    const bool place = from.depth && source.kind != "call";
    std::string word, sourceSlot;

    if (place) sourceSlot = location(source).value;
    else if (from.depth) word = expression(source).value;
    else word = allocate(type.pointee(), expression(source).value);

    const auto slot = location(target).value;
    std::string cycle = "false";

    if (from.depth) {
      auto current = word;

      if (place) {
        current = fresh();
        emit(current + " = load ptr, ptr " + sourceSlot);
      }

      if (recordValue(type.pointee())) {
        auto payload = fresh();
        emit(payload + " = call ptr @duck.heap$payload(ptr " + current + ", i1 true)");
        cycle = ownershipCycle(target, slot, payload, type.record);
      }

      // A borrowed result has no ownership to give.
      if (mayBeBorrowed(source)) {
        auto borrowed = fresh(), refused = fresh();
        emit(borrowed + " = call i1 @duck.heap$borrowed(ptr " + current + ")");
        emit(refused + " = or i1 " + cycle + ", " + borrowed);
        cycle = refused;
      }
    }

    std::string rejected, done, accepted;

    if (result) {
      rejected = label();
      done = label();
      accepted = label();
      emit("br i1 " + cycle + ", label %" + rejected + ", label %" + accepted);
      startBlock(rejected);

      // Only a call result has no other owner to stay with.
      if (!place) {
        auto temporary = fresh();
        allocations << "  " << temporary << " = alloca ptr\n";
        emit("store ptr " + word + ", ptr " + temporary);
        releaseBinding({type, temporary});
      }

      emit("br label %" + done);
      startBlock(accepted);
    } else {
      auto valid = fresh();
      emit(valid + " = xor i1 " + cycle + ", true");
      require(valid);
    }

    if (place) word = moveOwner(source);

    // The old child may run a destructor, so the new one is stored after it.
    releaseBinding({type, slot});
    emit("store ptr " + word + ", ptr " + slot);

    if (!result) return {};

    const auto acceptedEnd = currentBlock;
    emit("br label %" + done);
    startBlock(done);
    auto outcome = fresh();
    emit(outcome + " = phi i1 [ false, %" + rejected + " ], [ true, %" + acceptedEnd + " ]");
    return {Type::Bool, outcome};
  }
  // destroy is false for a value moved out by `return local`: its storage
  // still drops its references, but the object lives on in the caller.
  void releaseBinding(const Binding &binding, bool destroy = true) {
    if (binding.orderedLock) return emit("call void @duck.lock$exit(i32 " + binding.value + ")");
    if (binding.scopeLoop) return emit("call void @duck.scope$leaveLoop(ptr " + binding.value + ")");
    if (binding.pin) return emit("call void @duck.heap$unpin(ptr " + binding.value + ")");

    if (!binding.box.empty()) {
      // The payload dies with the box's last reference, which may be a lambda's.
      auto cell = fresh();
      emit(cell + " = load ptr, ptr " + binding.box);
      emit("call void @duck.cell$release(ptr " + cell + ")");
      return;
    }

    if (binding.scopedBorrow) return;

    if (binding.type.role != Type::Raw) {
      // An owner frees the payload, so it supplies the payload's cleanup.
      std::string cleanup = "null";
      const auto payload = binding.type.pointee();

      if (binding.type.role == Type::Owner && recordValue(payload) &&
          (functions.managed(payload) || functions.destructible(payload.record))) {
        cleanups.insert(payload.record);
        usesTextRuntime = true;
        usesCollectionRuntime = true; // Its destroy routine may free collection fields.
        cleanup = "@duck$cleanup." + payload.record;
      }

      emit("call void @duck.heap$drop(ptr " + binding.value + ", i1 " + ownerFlag(binding.type) +
           ", ptr " + cleanup + ")");
      return;
    }

    if (fileValue(binding.type)) {
      if (destroy) release({binding.type, load(binding.value)});

      return;
    }

    if (collectionValue(binding.type) || dynamicArray(binding.type)) {
      if (!destroy) return;

      auto old = fresh();
      emit(old + " = load ptr, ptr " + binding.value);

      if (dynamicArray(binding.type)) return freeArray(old, binding.type);

      return freeCollection(old, binding.type);
    }

    if (fixedArray(binding.type)) {
      if (destroy && functions.needsCleanup(binding.type)) cleanupElements(binding.value, binding.type);

      return;
    }

    const bool destructible = recordValue(binding.type) && functions.destructible(binding.type.record);

    if (destroy && destructible) destroyAt(binding.type.record, binding.value);
    if (!functions.managed(binding.type)) return;

    auto old = fresh();
    emit(old + " = load " + irType(binding.type) + ", ptr " + binding.value);

    // The object was destroyed in place; only its references remain to drop.
    if (destructible) eachManaged({binding.type, old}, "release");
    else release({binding.type, old});
  }
  void cleanupScopes(size_t first, const std::string &moved = "") {
    // Locals die in reverse declaration order, innermost scope first.
    for (size_t i = scopes.size(); i > first; --i) {
      std::vector<const Binding *> owned;

      for (const auto &[name, binding] : scopes[i - 1]) {
        (void)name;

        if (binding.owned) owned.push_back(&binding);
      }

      std::sort(owned.begin(), owned.end(),
                [](const Binding *a, const Binding *b) { return a->order > b->order; });

      for (const auto *binding : owned) releaseBinding(*binding, binding->value != moved);
    }
  }
  // Loads the underlying value of an enum member index. A constant index
  // selects the constant directly.
  Value enumUnderlying(Value value) {
    const auto &info = functions.enums.at(value.type.record);
    const auto type = irType(info.type);

    if (!value.value.empty() && std::isdigit(static_cast<unsigned char>(value.value[0]))) {
      const auto index = std::stoul(value.value);
      return {info.type, managedText(info.type) ? enumTextSymbol(value.type.record, index)
                                                : info.values.at(index)};
    }

    auto slot = fresh(), loaded = fresh();
    emit(slot + " = getelementptr [" + std::to_string(info.members.size()) + " x " + type +
         "], ptr @enum." + value.type.record + ".values, i32 0, i32 " + value.value);
    emit(loaded + " = load " + type + ", ptr " + slot);
    return {info.type, loaded};
  }
  static Type enumType(const std::string &record) {
    Type type = Type::Enum;
    type.record = record;
    return type;
  }
  // The index of the first member of `record` whose value equals v, both
  // compared as `compared`, and whether one was found: a linear scan of the
  // module's value table. Consumes v.
  std::pair<std::string, std::string> enumLookup(Value v, const std::string &record, const Type &compared) {
    const auto &info = functions.enums.at(record);
    const auto count = std::to_string(info.members.size());
    v = convert(v, compared);
    const auto before = currentBlock;
    const auto head = label(), body = label(), step = label(), done = label();
    const auto i = fresh(), next = fresh(), end = fresh();
    emit("br label %" + head);
    startBlock(head);
    emit(i + " = phi i32 [0, %" + before + "], [" + next + ", %" + step + "]");
    emit(end + " = icmp eq i32 " + i + ", " + count);
    emit("br i1 " + end + ", label %" + done + ", label %" + body);
    startBlock(body);
    const auto element = convert(enumUnderlying({enumType(record), i}), compared);
    auto equal = fresh();

    if (managedText(compared)) {
      usesTextRuntime = true;
      emit(equal + " = call i1 @duck.text.equal(ptr " + v.value + ", ptr " + element.value + ", i64 " +
           textWidth(compared) + ")");
    } else if (floating(compared))
      emit(equal + " = fcmp oeq " + irType(compared) + " " + v.value + ", " + element.value);
    else
      emit(equal + " = icmp eq " + irType(compared) + " " + v.value + ", " + element.value);

    const auto matched = currentBlock;
    emit("br i1 " + equal + ", label %" + done + ", label %" + step);
    startBlock(step);
    emit(next + " = add i32 " + i + ", 1");
    emit("br label %" + head);
    startBlock(done);
    const auto index = fresh(), found = fresh();
    emit(index + " = phi i32 [" + count + ", %" + head + "], [" + i + ", %" + matched + "]");
    emit(found + " = icmp ult i32 " + index + ", " + count);
    release(v);
    return {index, found};
  }
  // (Day)x: a member constant, the value itself, or a run-time lookup that
  // traps when no member holds x.
  Value lowerEnumCast(const Expr &e) {
    if (const auto member = analysis.enumMembers.find(&e); member != analysis.enumMembers.end())
      return {e.castType, std::to_string(member->second)};

    const auto &compared = analysis.enumCasts.at(&e);
    auto v = expression(*e.args[0]);

    if (enumValue(compared)) return {e.castType, v.value};

    const auto [index, found] = enumLookup(v, e.castType.record, compared);
    require(found);
    return {e.castType, index};
  }
  // d.Name gives the member's name and d.Value its underlying value.
  Value lowerEnumProperty(const Expr &e, const std::string &property) {
    const auto v = expression(*e.args[0]);

    if (property == "Value") return enumUnderlying(v);

    const auto &record = v.type.record;

    if (std::isdigit(static_cast<unsigned char>(v.value[0])))
      return {Type::Name, enumLabelSymbol(record, std::stoul(v.value))};

    auto slot = fresh(), loaded = fresh();
    emit(slot + " = getelementptr [" + std::to_string(functions.enums.at(record).members.size()) +
         " x ptr], ptr @enum." + record + ".labels, i32 0, i32 " + v.value);
    emit(loaded + " = load ptr, ptr " + slot);
    return {Type::Name, loaded};
  }
  Value convert(Value value, Type target) {
    if (value.type == target)
      return value;

    if (enumValue(value.type))
      return convert(enumUnderlying(value), target);

    if (arbitraryNumber(value.type) || arbitraryNumber(target))
      return convertArbitrary(value, target);

    if (value.type == Type::Char && managedText(target)) {
      usesTextRuntime = true;
      auto text = fresh();
      emit(text + " = call ptr @duck.char.text(i32 " + value.value + ", i64 " +
           textWidth(target) + ")");
      return {target, text};
    }

    if (value.type == Type::Char)
      value.type = Type::UInt32; // A scalar value is a nonnegative 21-bit integer.

    if (floating(target) && needsWideOperation(value.type))
      value = convert(value, wideOperationType(value.type));

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
      const auto direct = needsWideOperation(target) ? wideOperationType(target) : target;
      emit(id + (unsignedInteger(target) ? " = fptoui double " : " = fptosi double ") +
           rounded + " to " + irType(direct));
      return convert({direct, id}, target);
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
  Value convertArbitrary(Value value, Type target) {
    const auto source = value.type;
    auto id = fresh();

    if (bigInteger(source) && bigInteger(target)) {
      if (target == Type::Big) return {target, value.value}; // Storage is shared, not copied.

      usesBigRuntime = true;
      emit(id + " = call ptr @duck.big.requireUnsigned(ptr " + value.value + ")");
      return {target, id};
    }

    if (decimalValue(source) && decimalValue(target)) {
      if (target == Type::Tiny) return {target, value.value};

      usesDecimalRuntime = true;
      emit(id + " = call { i256, i32 } @duck.tiny.requireUnsigned({ i256, i32 } " + value.value + ")");
      return {target, id};
    }

    if (bigInteger(source)) {
      // Like other narrowing integer casts, keep the low two's-complement bits.
      usesBigRuntime = true;
      emit(id + " = call i64 @duck.big.low64(ptr " + value.value + ")");
      release(value);
      return convert({Type::Long, id}, target);
    }

    const bool isUnsigned = unsignedInteger(source);
    const auto word = convert(value, isUnsigned ? Type::UInt64 : Type::Long).value;
    Value result;

    if (bigInteger(target)) {
      usesBigRuntime = true;
      emit(id + (isUnsigned ? " = call ptr @duck.big.fromUnsigned(i64 " + word + ", i1 false)"
                            : " = call ptr @duck.big.fromSigned(i64 " + word + ")"));
      result = {Type::Big, id};
    } else {
      usesDecimalRuntime = true;
      auto wide = fresh();
      emit(wide + (isUnsigned ? " = zext i64 " : " = sext i64 ") + word + " to i256");
      emit(id + " = insertvalue { i256, i32 } zeroinitializer, i256 " + wide + ", 0");
      result = {Type::Tiny, id};
    }

    return convert(result, target); // Checks a signed source converted to ubig/utiny.
  }
  // Operands already have the common big/ubig type. Unsigned results that
  // would be negative trap.
  Value bigBinary(const std::string &op, Value a, Value b) {
    auto id = fresh();
    const std::map<std::string, std::string> predicates{
        {"<", "slt"}, {">", "sgt"}, {"<=", "sle"}, {">=", "sge"}, {"==", "eq"}, {"!=", "ne"}};
    auto predicate = predicates.find(op);
    const bool big = bigInteger(a.type);
    (big ? usesBigRuntime : usesDecimalRuntime) = true;
    const auto type = big ? std::string("ptr") : irType(a.type);
    const auto prefix = std::string(big ? "@duck.big." : "@duck.tiny.");
    const auto operands = type + " " + a.value + ", " + type + " " + b.value;

    if (predicate != predicates.end()) {
      auto order = fresh();
      emit(order + " = call i32 " + prefix + "compare(" + operands + ")");
      emit(id + " = icmp " + predicate->second + " i32 " + order + ", 0");
      release(a);
      release(b);
      return {Type::Bool, id};
    }

    if (op == "+" || op == "-")
      emit(id + " = call " + type + " " + prefix + "add(" + operands + ", i1 " +
           (op == "-" ? "true" : "false") + ")");
    else if (op == "*")
      emit(id + " = call " + type + " " + prefix + "multiply(" + operands + ")");
    else if (big)
      emit(id + " = call ptr @duck.big.divide(" + operands + ", i1 " + (op == "%" ? "true" : "false") + ")");
    else
      emit(id + " = call " + type + " @duck.tiny.divide(" + operands + ")");

    if (op == "-" && (a.type == Type::UBig || a.type == Type::UTiny)) {
      auto checked = fresh();
      emit(checked + " = call " + type + " " + prefix + "requireUnsigned(" + type + " " + id + ")");
      id = checked;
    }

    release(a);
    release(b);
    return {a.type, id};
  }
  // A big literal is immutable constant storage with the literal reference count.
  Value bigLiteral(const std::string &decimal, Type type) {
    usesBigRuntime = true;
    const bool negative = decimal[0] == '-';
    auto limbs = Magnitude::parse(negative ? decimal.substr(1) : decimal, 10).limbs;

    while (!limbs.empty() && !limbs.back()) limbs.pop_back();

    const auto count = std::to_string(limbs.size());
    const auto symbol = "@big." + symbolPrefix + "." + std::to_string(strings++);
    const auto array = "[" + count + " x i32]";
    globals << symbol << " = private unnamed_addr constant { i64, i64, i64, " << array
            << " } { i64 -1, i64 " << count << ", i64 " << (negative && !limbs.empty() ? 1 : 0)
            << ", " << array << " ";

    if (limbs.empty()) globals << "zeroinitializer";
    else {
      globals << "[";

      for (size_t i = 0; i < limbs.size(); ++i) globals << (i ? ", " : "") << "i32 " << limbs[i];

      globals << "]";
    }

    globals << " }, align 8\n";
    return {type, symbol};
  }
  Value textBinary(const std::string &op, const Value &a, const Value &b) {
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
  void checkShiftCount(const Value &a, Value &b) {
    // Check at the original count width before any truncation. Comparing
    // as unsigned rejects negative signed counts as well.
    // A narrow count type may not hold a custom width such as 512; widening
    // first keeps a negative signed count out of range.
    auto count = integerWidth(a.type) > 64 && integerWidth(b.type) < 64
                     ? convert(b, unsignedInteger(b.type) ? Type::UInt64 : Type::Long)
                     : b;
    auto valid = fresh();
    emit(valid + " = icmp ult " + irType(count.type) + " " + count.value + ", " +
         std::to_string(integerWidth(a.type)));
    require(valid);
    b = convert(b, a.type);
  }
  void checkDivision(const Value &a, const Value &b) {
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
           Magnitude::powerOfTwo(unsigned(integerWidth(a.type) - 1)).decimal());
      emit(negativeOne + " = icmp eq " + type + " " + b.value + ", -1");
      emit(overflow + " = and i1 " + minimum + ", " + negativeOne);
      emit(safe + " = xor i1 " + overflow + ", true");
      emit(valid + " = and i1 " + nonzero + ", " + safe);
    }

    require(valid);
  }
  static const std::map<std::string, std::string> &integerInstructions() {
    static const std::map<std::string, std::string> ints{
        {"+", "add"},      {"-", "sub"},       {"*", "mul"},
        {"/", "sdiv"},     {"%", "srem"},      {"<", "icmp slt"},
        {">", "icmp sgt"}, {"<=", "icmp sle"}, {">=", "icmp sge"},
        {"==", "icmp eq"}, {"!=", "icmp ne"},
        {"&", "and"}, {"|", "or"}, {"^", "xor"},
        {"<<", "shl"}, {">>", "ashr"}};
    return ints;
  }
  static const std::map<std::string, std::string> &floatingInstructions() {
    static const std::map<std::string, std::string> reals{
        {"+", "fadd"},      {"-", "fsub"},      {"*", "fmul"},
        {"/", "fdiv"},      {"<", "fcmp olt"},  {">", "fcmp ogt"},
        {"<=", "fcmp ole"}, {">=", "fcmp oge"}, {"==", "fcmp oeq"},
        {"!=", "fcmp une"}};
    return reals;
  }
  Value compareDateTimes(const std::string &op, const Value &a, const Value &b, const std::string &id) {
    // Compare dates first and use the time of day only for equal dates.
    auto ad = fresh(), at = fresh(), bd = fresh(), bt = fresh();
    auto sameDay = fresh(), dayOrder = fresh(), timeOrder = fresh();
    emit(ad + " = extractvalue { i32, i64 } " + a.value + ", 0");
    emit(at + " = extractvalue { i32, i64 } " + a.value + ", 1");
    emit(bd + " = extractvalue { i32, i64 } " + b.value + ", 0");
    emit(bt + " = extractvalue { i32, i64 } " + b.value + ", 1");
    emit(sameDay + " = icmp eq i32 " + ad + ", " + bd);
    emit(dayOrder + " = " + integerInstructions().at(op) + " i32 " + ad + ", " + bd);
    emit(timeOrder + " = " + integerInstructions().at(op) + " i64 " + at + ", " + bt);
    emit(id + " = select i1 " + sameDay + ", i1 " + timeOrder + ", i1 " + dayOrder);
    return {Type::Bool, id};
  }
  std::string binaryInstruction(const std::string &op, const Value &a) {
    auto instruction = (floating(a.type) ? floatingInstructions() : integerInstructions()).at(op);

    if (unsignedInteger(a.type)) {
      if (op == "/") instruction = "udiv";
      else if (op == "%") instruction = "urem";
      else if (op == "<") instruction = "icmp ult";
      else if (op == ">") instruction = "icmp ugt";
      else if (op == "<=") instruction = "icmp ule";
      else if (op == ">=") instruction = "icmp uge";
      else if (op == ">>") instruction = "lshr";
    }

    return instruction;
  }
  Value binary(const std::string &op, Value a, Value b) {
    if (nativeMath(a.type) || nativeMath(b.type)) return nativeBinary(op, a, b);
    if (floating(a.type) && (op == "==" || op == "!=")) return floatEquality(op, a, b);

    if (funcValue(a.type)) {
      auto compared = fresh();
      emit(compared + " = icmp " + (op == "==" ? "eq" : "ne") + " ptr " + a.value + ", null");
      release(a);
      return {Type::Bool, compared};
    }

    if (arbitraryNumber(a.type)) return bigBinary(op, a, b);
    if (managedText(a.type)) return textBinary(op, a, b);
    if (op == "<<" || op == ">>") checkShiftCount(a, b);
    if (integral(a.type) && (op == "/" || op == "%")) checkDivision(a, b);

    auto id = fresh();

    if (a.type == Type::DateTime) return compareDateTimes(op, a, b, id);

    const auto instruction = binaryInstruction(op, a);

    if ((op == "/" || op == "%") && needsWideOperation(a.type)) {
      const auto wide = wideOperationType(a.type);
      auto x = convert(a, wide), y = convert(b, wide);
      emit(id + " = " + instruction + " " + irType(wide) + " " + x.value + ", " + y.value);
      return convert({wide, id}, a.type);
    }

    emit(id + " = " + instruction + " " +
         irType(a.type) + " " + a.value + ", " + b.value);
    return {isComparison(op) ? Type(Type::Bool) : a.type, id};
  }
  void startBlock(const std::string &name) {
    code << name << ":\n";
    currentBlock = name;
  }
  Binding findBinding(const Token &t) const {
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
      auto found = it->find(t.text);

      if (found != it->end())
        return found->second;
    }

    throw std::logic_error(
        "lowering requires a successfully analyzed function");
  }
  Binding variable(const Token &t) {
    auto binding = findBinding(t);

    if (binding.iteration) binding.value = iterationSlot(*binding.iteration, binding.iterationKey);
    if (binding.scoped) binding.value = scopedData(binding.value, binding.scoped->scope, binding.type);

    return binding;
  }
  std::string scopeDescriptor(const std::string &name) {
    usesScopeRuntime = true;
    scopeNames.insert(name);
    return "@duck$scope." + name;
  }
  // Where a new scoped value of this type gets its default, or null for zero.
  std::string scopeInitial(const Type &type) {
    if (recordValue(type) && hasDefaults(functions, type.record)) return "@duck$default." + type.record;
    if (!managedValue(type)) return "null";

    const auto key = typeName(type);

    if (const auto known = scopeInitials.find(key); known != scopeInitials.end()) return known->second;

    const auto symbol = "@duck$initial." + symbolPrefix + "." + std::to_string(strings++);
    globals << symbol << " = private unnamed_addr constant ptr " << managedZero(type) << "\n";
    return scopeInitials[key] = symbol;
  }
  // The arena storage behind a scoped variable's slot.
  std::string scopedData(const std::string &slot, const std::string &scope, const Type &type) {
    const bool guarded = std::exchange(guardScopedPlace, false);
    auto data = fresh();
    emit(data + " = call ptr @duck.scope$data(ptr " + scopeDescriptor(scope) + ", ptr " + slot + ", i64 " + sizeOf(type) +
         ", ptr " + scopeInitial(type) + ", ptr " + elementCleanup(type) + ", i1 " + (guarded ? "true" : "false") + ")");
    return data;
  }
  // After a call that may have run @end: when an end is in progress, leave
  // through the innermost enclosing loop of that scope, or return to the caller
  // with a default result so the caller can do the same.
  void unwindCheck() {
    usesScopeRuntime = true;
    auto target = fresh(), pending = fresh();
    const auto landing = label(), proceed = label();
    emit(target + " = load ptr, ptr @duck$scope.unwind");
    emit(pending + " = icmp ne ptr " + target + ", null");
    emit("br i1 " + pending + ", label %" + landing + ", label %" + proceed);
    startBlock(landing);

    for (const auto &use : activeUses) emit("call void @duck.heap$unpin(ptr " + use + ")");

    for (size_t i = loops.size(); i-- > 0;) {
      if (loops[i].scope.empty()) continue;

      auto same = fresh();
      const auto hit = label(), miss = label();
      emit(same + " = icmp eq ptr " + target + ", " + loops[i].scope);
      emit("br i1 " + same + ", label %" + hit + ", label %" + miss);
      startBlock(hit);
      emit("store ptr null, ptr @duck$scope.unwind");
      cleanupScopes(loops[i].scopeDepth);
      emit("br label %" + loops[i].exit);
      startBlock(miss);
    }

    cleanupScopes(0);

    if (functionResult == Type::Void) emit("ret void");
    else emit("ret " + irType(functionResult) + " " + defaultValue(functionResult));

    startBlock(proceed);
  }
  bool mayUnwind(const std::string &target) const { return analysis.unwinders && analysis.unwinders->count(target); }
  // The live number of elements of the collection a for-each iterates.
  std::string iterationLength(const Iteration &iteration) {
    if (fixedArray(iteration.type)) return std::to_string(iteration.type.extent);

    const auto holder = location(*iteration.collection).value;
    auto c = fresh(), n = fresh();
    emit(c + " = load ptr, ptr " + holder);
    emit(n + " = call i64 @duck." + std::string(iteration.type.base == Type::Pair ? "pair" : "seq") + "$length(ptr " +
         c + ")");
    return n;
  }
  // The storage of a for-each's current element, or of its key. Traps once
  // that element has been removed or is no longer inside the collection.
  std::string iterationSlot(const Iteration &iteration, bool key) {
    const auto &type = iteration.type;
    const auto holder = location(*iteration.collection).value;
    auto position = fresh(), slot = fresh();
    emit(position + " = load i64, ptr " + iteration.current);

    if (fixedArray(type)) {
      emit(slot + " = getelementptr " + irType(type) + ", ptr " + holder + ", i64 0, i64 " + position);
      return slot;
    }

    auto c = fresh();
    emit(c + " = load ptr, ptr " + holder);

    if (type.base == Type::Pair) {
      auto entry = fresh();
      emit(entry + " = call ptr @duck.pair$nth(ptr " + c + ", i64 " + position + ")");

      if (key) return entry;

      emit(slot + " = call ptr @duck.pair$value(ptr " + c + ", ptr " + entry + ")");
      return slot;
    }

    emit(slot + " = call ptr @duck.seq$" + (dynamicArray(type) ? "index" : "nth") + "(ptr " + c + ", i64 " + position +
         ")");
    return slot;
  }
  // Ends one step of a range whose values `counter` holds: leaves for exit
  // at or past the endpoint, before a step could overflow, and otherwise
  // steps toward it and returns to body. Both endpoints were already
  // evaluated, once, and `ascending` compared them.
  void rangeAdvance(const std::string &counter, const Value &end, const std::string &ascending,
                    const std::string &body, const std::string &exit) {
    const auto type = irType(end.type);
    const std::string sign = unsignedInteger(end.type) ? "u" : "s";
    auto value = fresh(), above = fresh(), below = fresh(), done = fresh();
    emit(value + " = load " + type + ", ptr " + counter);
    emit(above + " = icmp " + sign + "ge " + type + " " + value + ", " + end.value);
    emit(below + " = icmp " + sign + "le " + type + " " + value + ", " + end.value);
    emit(done + " = select i1 " + ascending + ", i1 " + above + ", i1 " + below);
    auto step = label();
    emit("br i1 " + done + ", label %" + exit + ", label %" + step);
    startBlock(step);
    auto up = fresh(), down = fresh(), following = fresh();
    emit(up + " = add " + type + " " + value + ", 1");
    emit(down + " = sub " + type + " " + value + ", 1");
    emit(following + " = select i1 " + ascending + ", " + type + " " + up + ", " + type + " " + down);
    emit("store " + type + " " + following + ", ptr " + counter);
    emit("br label %" + body);
  }
  std::string rangeAscending(const Value &first, const Value &last) {
    auto ascending = fresh();
    emit(ascending + " = icmp " + (unsignedInteger(first.type) ? "ule " : "sle ") + irType(first.type) + " " +
         first.value + ", " + last.value);
    return ascending;
  }
  // `for i : a...b`: i is the counter itself, so a body that changes it
  // changes which values follow. A range always holds at least one value.
  // `for item : Day` counts through member indices; the key, when there is
  // one, is the same position as an int.
  void enumFor(const Stmt &s, const std::string &record) {
    const auto count = functions.enums.at(record).members.size();

    if (count == 0) return;

    const auto first = s.reverse ? std::to_string(count - 1) : "0";
    const auto last = s.reverse ? "0" : std::to_string(count - 1);
    Type type = Type::Enum;
    type.record = record;
    scopes.emplace_back();
    auto name = s.token;
    name.text = s.name;
    declare(name, {type, first}, true);
    auto key = s.token;
    key.text = s.key;

    if (!s.key.empty()) declare(key, {Type::Int, first}, true);

    auto body = label(), step = label(), advance = label(), end = label();
    emit("br label %" + body);
    startBlock(body);
    loops.push_back({end, step, scopes.size()});

    if (!block(s.body))
      emit("br label %" + step);

    loops.pop_back();
    startBlock(step);
    const auto slot = findBinding(name).value;
    auto current = fresh(), done = fresh(), next = fresh();
    emit(current + " = load i32, ptr " + slot);
    emit(done + " = icmp eq i32 " + current + ", " + last);
    emit("br i1 " + done + ", label %" + end + ", label %" + advance);
    startBlock(advance);
    emit(next + " = " + (s.reverse ? "sub" : "add") + " i32 " + current + ", 1");
    emit("store i32 " + next + ", ptr " + slot);

    if (!s.key.empty()) emit("store i32 " + next + ", ptr " + findBinding(key).value);

    emit("br label %" + body);
    startBlock(end);
    cleanupScopes(scopes.size() - 1);
    scopes.pop_back();
  }
  void rangeFor(const Stmt &s) {
    const auto first = condition(*s.expr->args[0]), last = condition(*s.expr->args[1]);
    scopes.emplace_back();
    auto name = s.token;
    name.text = s.name;
    declare(name, first);
    const auto ascending = rangeAscending(first, last);
    auto body = label(), step = label(), end = label();
    emit("br label %" + body);
    startBlock(body);
    loops.push_back({end, step, scopes.size()});

    if (!block(s.body))
      emit("br label %" + step);

    loops.pop_back();
    startBlock(step);
    rangeAdvance(findBinding(name).value, last, ascending, body, end);
    startBlock(end);
    cleanupScopes(scopes.size() - 1);
    scopes.pop_back();
  }
  // A range used as a value: a new dynamic array of its values.
  Value rangeArray(const Expr &e) {
    usesCollectionRuntime = true;
    const auto type = analysis.expressionTypes.at(&e);
    const auto &element = *type.element;
    const auto first = expression(*e.args[0]), last = expression(*e.args[1]);
    auto holder = fresh(), counter = fresh();
    allocations << "  " << holder << " = alloca ptr\n";
    allocations << "  " << counter << " = alloca " << irType(element) << '\n';
    emit("store ptr null, ptr " + holder);
    emit("store " + irType(element) + " " + first.value + ", ptr " + counter);
    const auto ascending = rangeAscending(first, last);
    auto body = label(), end = label();
    emit("br label %" + body);
    startBlock(body);
    auto value = fresh(), slot = fresh();
    emit(value + " = load " + irType(element) + ", ptr " + counter);
    emit(slot + " = call ptr @duck.seq$push(ptr " + holder + ", i64 " + sizeOf(element) + ")");
    emit("store " + irType(element) + " " + value + ", ptr " + slot);
    rangeAdvance(counter, last, ascending, body, end);
    startBlock(end);
    auto result = fresh();
    emit(result + " = load ptr, ptr " + holder);
    return {type, result};
  }
  // `for x : c` and `for -x : c` lower to an indexed loop over the live
  // length. The check picks the next position, so `continue` needs no step.
  // Reverse starts at the largest position and is clamped to the last
  // element, which also keeps it inside a collection a callee has shrunk.
  void forEach(const Stmt &s) {
    usesCollectionRuntime = true;
    const auto type = analysis.expressionTypes.at(s.expr.get());
    const Iteration iteration{s.expr.get(), type, s.reverse, fresh(), fresh()};
    allocations << "  " << iteration.current << " = alloca i64\n";
    allocations << "  " << iteration.next << " = alloca i64\n";
    emit("store i64 " + std::string(s.reverse ? "9223372036854775807" : "0") + ", ptr " + iteration.next);
    scopes.emplace_back();
    auto name = s.token;
    name.text = s.name;
    bind(name, {*type.element, "", false, false, false, 0, "", &iteration, false});

    if (!s.key.empty()) {
      name.text = s.key;
      bind(name, {*type.key, "", true, false, false, 0, "", &iteration, true});
    }

    auto check = label(), body = label(), end = label();
    emit("br label %" + check);
    startBlock(check);
    const auto length = iterationLength(iteration);
    auto position = fresh(), more = fresh();
    emit(position + " = load i64, ptr " + iteration.next);

    if (s.reverse) {
      auto last = fresh(), beyond = fresh(), clamped = fresh();
      emit(last + " = sub i64 " + length + ", 1");
      emit(beyond + " = icmp sgt i64 " + position + ", " + last);
      emit(clamped + " = select i1 " + beyond + ", i64 " + last + ", i64 " + position);
      position = clamped;
      emit(more + " = icmp sge i64 " + position + ", 0");
    } else
      emit(more + " = icmp slt i64 " + position + ", " + length);

    emit("br i1 " + more + ", label %" + body + ", label %" + end);
    startBlock(body);
    auto following = fresh();
    emit(following + " = add i64 " + position + ", " + (s.reverse ? "-1" : "1"));
    emit("store i64 " + position + ", ptr " + iteration.current);
    emit("store i64 " + following + ", ptr " + iteration.next);
    loops.push_back({end, check, scopes.size()});

    if (!block(s.body))
      emit("br label %" + check);

    loops.pop_back();
    startBlock(end);
    scopes.pop_back();
  }
  size_t declarations = 0;
  void bind(const Token &t, Binding binding) {
    binding.order = declarations++;
    scopes.back()[t.text] = binding;
  }
  void declare(const Token &t, Value value, bool constant = false, bool scopedBorrow = false) {
    auto address = fresh();
    std::string boxSlot;

    if (analysis.captured.count(captureKey(t))) {
      // Lambdas share this local, so it lives in a box that outlives the scope.
      usesFuncRuntime = true;
      boxSlot = fresh();
      allocations << "  " << boxSlot << " = alloca ptr\n";
      auto box = fresh();
      emit(box + " = call ptr @duck.box$new(i64 " + sizeOf(value.type) + ", ptr " + elementCleanup(value.type) + ")");
      emit("store ptr " + box + ", ptr " + boxSlot);
      emit(address + " = getelementptr i8, ptr " + box + ", i64 32");
    } else
      allocations << "  " << address << " = alloca " << irType(value.type) << '\n';

    if (recordValue(value.type) && value.value.empty()) value.value = defaultValue(value.type);

    if ((managedValue(value.type) || funcValue(value.type) || collectionValue(value.type) ||
         fileValue(value.type) || dynamicArray(value.type)) &&
        value.value.empty())
      value.value = "null";

    const bool defaults = fixedArray(value.type) && value.value.empty();

    if (defaults) value.value = "zeroinitializer";

    if (!value.value.empty())
      emit("store " + irType(value.type) + " " + value.value + ", ptr " +
           address);

    if (defaults) fillDefaults(address, value.type);

    Binding binding{value.type, address, constant, true, scopedBorrow};
    binding.box = boxSlot;
    bind(t, binding);
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
    auto symbol = "@str." + symbolPrefix + "." + std::to_string(strings++);
    globals << managedTextGlobal(symbol, e.token, type);
    return {type, symbol};
  }
  Value expression(const Expr &e) {
    // The analysis artifact is required, including in release builds.
    auto expected = analysis.expressionTypes.at(&e);
    auto conversion = analysis.conversions.find(&e);

    // Storing a reference in an alias slot takes a counted reference, while
    // reading one gives its payload.
    if (conversion != analysis.conversions.end() && conversion->second.role == Type::Alias)
      return {conversion->second, aliasWord(e)};

    // A tracked reference borrowed into a raw parameter stays in use.
    if (conversion != analysis.conversions.end() && expected.role != Type::Raw && pinning && pinnable(e, expected))
      return convert({expected, pinUse(e)}, conversion->second);

    auto value = lowerExpression(e);

    if (value.type != expected)
      throw std::logic_error("lowering disagrees with semantic analysis");

    if (analysis.pointerTests.count(&e)) {
      auto test = fresh();
      emit(test + " = icmp ne ptr " + value.value + ", null");
      return {Type::Bool, test};
    }

    return conversion == analysis.conversions.end()
               ? value
               : convert(value, conversion->second);
  }
  Value dereference(const Expr &e) {
    const auto type = analysis.expressionTypes.at(&e);

    if (auto link = guardedLinks.find(&e); link != guardedLinks.end()) return {type, link->second};

    if (type.role != Type::Raw) {
      if (auto use = analysis.checkedUses.find(&e); use != analysis.checkedUses.end())
        if (auto payload = checkedPayloads.find(use->second); payload != checkedPayloads.end())
          return {type, payload->second};

      if (pinning && pinnable(e, type)) return {type, pinUse(e)};
    }

    auto v = expression(e);

    if (v.type.role != Type::Raw) {
      auto valid = fresh();
      emit(valid + " = icmp ne ptr " + v.value + ", null");
      require(valid);
    }

    return v;
  }
  // ", i32 a, i32 b" for a field path through parent views.
  static std::string indexList(const std::vector<size_t> &indices) {
    std::string list;

    for (auto index : indices) list += ", i32 " + std::to_string(index);

    return list;
  }
  // The element a GetChildren() call is on, or the address of its child storage.
  std::string childrenSlot(const Expr &call) {
    usesMarkupRuntime = true;
    const auto &base = *call.receiver;
    const auto type = analysis.expressionTypes.at(&base);

    if (elementValue(type)) {
      auto id = fresh();
      emit(id + " = call ptr @duck.markup$children(ptr " + load(location(base).value) + ")");
      return id;
    }

    return memberAddress(base, analysis.markupChildren.at(&call));
  }
  std::string load(const std::string &slot) {
    auto id = fresh();
    emit(id + " = load ptr, ptr " + slot);
    return id;
  }
  // Binds every field an instance method can name directly to its address
  // inside the receiver at self.
  void bindVisibleFields(const Type &record, const std::string &self) {
    for (const auto &path : functions.visibleFields(record.record)) {
      auto address = fresh();
      emit(address + " = getelementptr " + irType(record) + ", ptr " + self + ", i32 0" + indexList(path.indices));
      Binding field{path.field->scope.empty() ? path.field->type : path.field->scopedType, address};

      if (!path.field->scope.empty()) field.scoped = path.field;

      bind(path.field->name, field);
    }
  }
  // The address of the field at indices inside the container base names.
  std::string memberAddress(const Expr &base, const std::vector<size_t> &indices) {
    auto record = analysis.expressionTypes.at(&base);
    std::string address;

    if (!record.depth) address = location(base).value;
    else {
      address = dereference(base).value;
      record = record.pointee();
    }

    auto id = fresh();
    emit(id + " = getelementptr " + irType(record) + ", ptr " + address + ", i32 0" + indexList(indices));
    return id;
  }
  // The word holding the child `x.Label` names, or `x.Label[i]` with index.
  std::string componentSlot(const Expr &lookup, const Expr *index, bool guarded) {
    usesMarkupRuntime = true;
    const auto &info = analysis.markupLookups.at(&lookup);
    const auto holder = load(memberAddress(*lookup.args[0], info.holder));
    const auto name = textConstant(info.child);
    auto slot = fresh();

    if (!index) {
      emit(slot + " = call ptr @duck.markup$find(ptr " + holder + ", ptr " + name + ")");
      return slot;
    }

    const auto position = expression(*index);
    emit(slot + " = call ptr @duck.markup$nth(ptr " + holder + ", ptr " + name + ", i64 " + position.value + ", i1 " +
         (guarded ? "true" : "false") + ")");
    return slot;
  }
  Binding location(const Expr &e) {
    if (auto found = guardedSlots.find(&e); found != guardedSlots.end()) return {analysis.expressionTypes.at(&e), found->second};
    if (analysis.markupChildren.count(&e)) return {analysis.expressionTypes.at(&e), childrenSlot(e)};
    if (analysis.markupIndexed.count(&e)) return {analysis.expressionTypes.at(&e), elementSlot(e, false)};
    if (analysis.markupLookups.count(&e)) return {analysis.expressionTypes.at(&e), componentSlot(e, nullptr, false)};

    auto field = analysis.staticFields.find(&e);

    if (field != analysis.staticFields.end()) {
      const std::string address = "@duck$static." + field->second;

      if (auto place = analysis.scopedPlaces.find(&e); place != analysis.scopedPlaces.end())
        return {analysis.expressionTypes.at(&e), scopedData(address, place->second.scope, place->second.type)};

      return {analysis.expressionTypes.at(&e), address};
    }

    if (e.kind == "name") return variable(e.token);

    auto type = analysis.expressionTypes.at(&e);

    if (e.kind == "unary") return {type, dereference(*e.args[0]).value};
    if (e.kind == "index") return {type, elementSlot(e, false)};

    const auto id = memberAddress(*e.args[0], analysis.fieldIndices.at(&e));

    if (auto place = analysis.scopedPlaces.find(&e); place != analysis.scopedPlaces.end())
      return {type, scopedData(id, place->second.scope, place->second.type)};

    return {type, id};
  }
  // The slot of a[i] or p[key]. The collection and the index or key are
  // evaluated once. A missing element traps, or with guarded gives null.
  std::string elementSlot(const Expr &e, bool guarded) {
    if (analysis.scopedPlaces.count(&e)) {
      guardScopedPlace = guarded;
      return location(e).value;
    }

    if (analysis.markupIndexed.count(&e)) {
      const auto &base = *e.args[0];

      if (analysis.markupLookups.count(&base)) return componentSlot(base, e.args[1].get(), guarded);

      usesCollectionRuntime = true;
      const auto children = load(location(base).value);
      const auto position = expression(*e.args[1]);
      auto slot = fresh();
      emit(slot + " = call ptr @duck.seq$nth(ptr " + children + ", i64 " + position.value + ")");
      return slot;
    }

    if (std::vector<const Expr *> coordinates; const auto *grid = gridRoot(e, coordinates))
      return gridSlot(*grid, coordinates, !guarded);

    const auto holderType = analysis.expressionTypes.at(e.args[0].get());
    const auto &base = *e.args[0];

    if (guarded && base.kind == "index") {
      // ?a[i][j]: a missing outer element skips the inner index.
      const auto outer = elementSlot(base, true);
      auto ok = fresh(), inside = label(), done = label(), slot = fresh();
      emit(ok + " = icmp ne ptr " + outer + ", null");
      const auto from = currentBlock;
      emit("br i1 " + ok + ", label %" + inside + ", label %" + done);
      startBlock(inside);
      const auto inner = slotIn(e, holderType, outer, true);
      const auto end = currentBlock;
      emit("br label %" + done);
      startBlock(done);
      emit(slot + " = phi ptr [null, %" + from + "], [" + inner + ", %" + end + "]");
      return slot;
    }

    return slotIn(e, holderType, location(base).value, guarded);
  }
  // The slot of e's element in the array or pair stored at holder.
  std::string slotIn(const Expr &e, const Type &holderType, const std::string &holder, bool guarded) {
    // Packed scalars or columns, so the storage indexes as a plain array.
    if (const auto shape = nativeMath(holderType))
      return slotIn(e, Type::array(shape.element(), shape.matrix() ? shape.columns : shape.rows), holder, guarded);

    auto slot = fresh();

    if (holderType.base == Type::Pair) {
      usesCollectionRuntime = true;
      auto c = fresh();
      emit(c + " = load ptr, ptr " + holder);
      const auto key = expression(*e.args[1]);
      emit(slot + " = call ptr @duck.pair$lookup(ptr " + c + ", ptr " + keySlot(key) + ", ptr " +
           keyEquality(*holderType.key) + ")");
      release(key);

      if (!guarded) {
        auto found = fresh();
        emit(found + " = icmp ne ptr " + slot + ", null");
        require(found);
      }

      return slot;
    }

    const auto index = expression(*e.args[1]);

    if (holderType.extent) {
      auto valid = fresh(), address = guarded ? fresh() : slot;
      emit(valid + " = icmp ult i64 " + index.value + ", " + std::to_string(holderType.extent));

      if (!guarded) require(valid);

      emit(address + " = getelementptr " + irType(holderType) + ", ptr " + holder + ", i64 0, i64 " + index.value);

      if (guarded) emit(slot + " = select i1 " + valid + ", ptr " + address + ", ptr null");

      return slot;
    }

    usesCollectionRuntime = true;
    auto c = fresh();
    emit(c + " = load ptr, ptr " + holder);
    emit(slot + " = call ptr @duck.seq$" + (guarded ? "find" : "index") + "(ptr " + c + ", i64 " + index.value + ")");
    return slot;
  }
  // The element at slot, or the element type's default when slot is null.
  Value readOrDefault(const std::string &slot, const Type &type) {
    auto result = fresh(), found = fresh(), hit = label(), miss = label(), done = label();
    allocations << "  " << result << " = alloca " << irType(type) << '\n';
    emit(found + " = icmp ne ptr " + slot + ", null");
    emit("br i1 " + found + ", label %" + hit + ", label %" + miss);
    startBlock(hit);
    emit("store " + irType(type) + " " + loadSlot(slot, type).value + ", ptr " + result);
    emit("br label %" + done);
    startBlock(miss);
    emit("store " + irType(type) + " " + missingElement(type).value + ", ptr " + result);
    emit("br label %" + done);
    startBlock(done);
    auto value = fresh();
    emit(value + " = load " + irType(type) + ", ptr " + result);
    return {type, value};
  }
  // grid[x][y]: the stored grid it indexes and its coordinates, outermost
  // first; null when e does not index into a grid.
  const Expr *gridRoot(const Expr &e, std::vector<const Expr *> &coordinates) const {
    std::vector<const Expr *> found;
    const Expr *root = &e;

    while (root->kind == "index") {
      found.insert(found.begin(), root->args[1].get());
      root = root->args[0].get();
    }

    const auto type = analysis.expressionTypes.find(root);

    if (type == analysis.expressionTypes.end() || !gridValue(type->second)) return nullptr;

    coordinates = std::move(found);
    return root;
  }
  // The [N x int] key of a grid cell.
  Value gridKey(const Type &grid, const std::vector<const Expr *> &coordinates) {
    std::string value = "zeroinitializer";

    for (size_t i = 0; i < coordinates.size(); ++i) {
      const auto coordinate = expression(*coordinates[i]);
      auto next = fresh();
      emit(next + " = insertvalue " + irType(*grid.key) + " " + value + ", i32 " + coordinate.value + ", " +
           std::to_string(i));
      value = next;
    }

    return {*grid.key, value};
  }
  // The value slot of a grid cell. A missing cell gives null, or with
  // insert is stored with the element default first.
  std::string gridSlot(const Expr &root, const std::vector<const Expr *> &coordinates, bool insert) {
    usesCollectionRuntime = true;
    const auto type = analysis.expressionTypes.at(&root);
    const auto holder = location(root).value;
    const auto key = gridKey(type, coordinates);
    auto c = fresh(), slot = fresh();
    emit(c + " = load ptr, ptr " + holder);
    emit(slot + " = call ptr @duck.pair$lookup(ptr " + c + ", ptr " + keySlot(key) + ", ptr null)");

    if (!insert) return slot;

    auto missing = fresh(), add = label(), done = label(), result = fresh();
    emit(missing + " = icmp eq ptr " + slot + ", null");
    const auto from = currentBlock;
    emit("br i1 " + missing + ", label %" + add + ", label %" + done);
    startBlock(add);
    const auto entry = pairPut(holder, type, key, missingElement(*type.element));
    auto current = fresh(), added = fresh();
    emit(current + " = load ptr, ptr " + holder);
    emit(added + " = call ptr @duck.pair$value(ptr " + current + ", ptr " + entry + ")");
    const auto end = currentBlock;
    emit("br label %" + done);
    startBlock(done);
    emit(result + " = phi ptr [" + slot + ", %" + from + "], [" + added + ", %" + end + "]");
    return result;
  }
  // The value a guarded read gives for a missing element: zero, empty
  // text, a zero big, or a container's default.
  Value missingElement(const Type &type) {
    if (recordValue(type)) return {type, defaultValue(type)};

    if (fixedArray(type)) {
      auto slot = fresh(), value = fresh();
      allocations << "  " << slot << " = alloca " << irType(type) << '\n';
      emit("store " + irType(type) + " zeroinitializer, ptr " + slot);
      fillDefaults(slot, type);
      emit(value + " = load " + irType(type) + ", ptr " + slot);
      return {type, value};
    }

    if (bigInteger(type)) return bigLiteral("0", type);

    if (managedText(type)) {
      Value empty{type, "@str." + symbolPrefix + "." + std::to_string(strings++)};
      globals << managedTextGlobal(empty.value, Token{}, type);
      return empty;
    }

    return {type, "zeroinitializer"};
  }
  Value lowerRecordInitializer(const Expr &e) {
    auto type = analysis.expressionTypes.at(&e);
    // Unnamed fields take their defaults; a named field's default is never
    // evaluated, and its constant base needs no release when overwritten.
    std::set<std::vector<size_t>> named;

    for (const auto &field : e.args) named.insert(analysis.fieldIndices.at(field.get()));

    std::string value = defaultValue(type, named);

    for (const auto &field : e.args) {
      auto v = expression(*field->args.front());
      auto aggregate = fresh();
      std::string indices; // insertvalue takes plain constant indices.

      for (auto index : analysis.fieldIndices.at(field.get())) indices += ", " + std::to_string(index);

      emit(aggregate + " = insertvalue " + irType(type) + " " + value + ", " + irType(v.type) +
           " " + v.value + indices);
      value = aggregate;
    }

    return {type, value};
  }
  Value lowerCollectionMember(const Expr &e, const std::string &name) {
    if (name == "IsOpen") {
      auto open = fresh();
      emit(open + " = icmp ne ptr " + load(location(*e.args[0]).value) + ", null");
      return {Type::Bool, open};
    }

    usesCollectionRuntime = true;
    const auto type = analysis.expressionTypes.at(e.args[0].get());
    auto c = fresh();
    const auto holder = location(*e.args[0]).value;

    if (fixedArray(type)) return {Type::Long, std::to_string(type.extent)};

    emit(c + " = load ptr, ptr " + holder);

    if (name == "Len") {
      auto n = fresh();
      emit(n + " = call i64 @duck." + std::string(type.base == Type::Pair ? "pair" : "seq") + "$length(ptr " + c +
           ")");
      return {Type::Long, n};
    }

    auto slot = fresh();

    if (type.base == Type::Pair) {
      auto entry = fresh();
      emit(entry + " = call ptr @duck.pair$last(ptr " + c + ")");

      if (name == "LastKey") return loadSlot(entry, *type.key);

      emit(slot + " = call ptr @duck.pair$value(ptr " + c + ", ptr " + entry + ")");
    } else
      emit(slot + " = call ptr @duck.seq$at(ptr " + c + ", i1 " + (name == "Top" ? "1" : "0") + ")");

    return loadSlot(slot, *type.element);
  }
  // A file's methods; the receiver is always stored, so they work on its slot.
  Value lowerFileCall(const Expr &e, const std::string &name) {
    usesFileRuntime = true;
    usesTextRuntime = true; // ReadText and the runtime's text helpers.
    const auto slot = location(*e.receiver).value;

    if (name == "Close") {
      emit("call void @duck.file$drop(ptr " + slot + ")");
      return {Type::Void, ""};
    }

    if (name == "Open" || name == "Create" || name == "Append") {
      const auto path = convert(expression(*e.args[0]), Type::String);
      const auto mode = name == "Open" ? "0" : name == "Create" ? "1" : "2";
      auto ok = fresh();
      emit(ok + " = call i1 @duck.file$open(ptr " + slot + ", ptr " + path.value + ", i32 " + mode + ")");
      release(path);
      return {Type::Bool, ok};
    }

    const auto handle = load(slot);

    if (name == "ReadText") {
      auto text = fresh();
      emit(text + " = call ptr @duck.file$text(ptr " + handle + ")");
      return convert({Type::Name, text}, Type::String);
    }

    const std::string call = name == "Read" ? "@duck.file$read" : "@duck.file$write";
    auto moved = fresh();

    if (e.args.size() == 2) {
      const auto buffer = expression(*e.args[0]);
      const auto bytes = expression(*e.args[1]);
      emit(moved + " = call i64 " + call + "(ptr " + handle + ", ptr " + buffer.value + ", i64 1, i64 " +
           bytes.value + ")");
      return {Type::Long, moved};
    }

    const auto &argument = *e.args[0];
    const auto given = analysis.expressionTypes.at(&argument);

    if (managedText(given)) {
      // Text is written as UTF-8 bytes.
      const auto text = convert(expression(argument), Type::Name);
      auto length = fresh(), data = fresh();
      emit(length + " = call i64 @duck.text.length(ptr " + text.value + ")");
      emit(data + " = getelementptr i8, ptr " + text.value + ", i64 16");
      emit(moved + " = call i64 " + call + "(ptr " + handle + ", ptr " + data + ", i64 1, i64 " + length + ")");
      release(text);
      return {Type::Long, moved};
    }

    // A fixed array of numbers moves as raw bytes; the result counts elements.
    Type leaf = given;
    unsigned long long count = 1;

    for (; fixedArray(leaf); leaf = *leaf.element) count *= leaf.extent;

    emit(moved + " = call i64 " + call + "(ptr " + handle + ", ptr " + location(argument).value + ", i64 " +
         sizeOf(leaf) + ", i64 " + std::to_string(count) + ")");
    return {Type::Long, moved};
  }
  Value lowerCall(const Expr &e) {
    const auto &op = e.token.text;

    if (auto file = analysis.fileCalls.find(&e); file != analysis.fileCalls.end()) return lowerFileCall(e, file->second);

    if (e.receiver && memberName(op) == "Clear")
      if (const auto held = analysis.expressionTypes.find(e.receiver.get());
          held != analysis.expressionTypes.end() &&
          (dynamicArray(held->second) || collectionValue(held->second))) {
        // Frees the elements and the storage; the collection is empty again.
        const auto holder = location(*e.receiver).value;
        auto c = fresh();
        emit(c + " = load ptr, ptr " + holder);
        release({held->second, c});
        emit("store ptr null, ptr " + holder);
        return {Type::Void, ""};
      }

    if (auto constructed = analysis.constructions.find(&e); constructed != analysis.constructions.end()) {
      const auto type = analysis.expressionTypes.at(&e);

      if (functions.parts(type.record).empty()) return {type, defaultValue(type)};

      return {type, constructRecord(type, constructed->second, &e)};
    }

    if (auto re = analysis.reinitializations.find(&e); re != analysis.reinitializations.end())
        return lowerReinitialization(e, re->second);

    if (auto completed = analysis.completions.find(&e); completed != analysis.completions.end()) {
      const auto binding = variable(e.token);
      runParts(binding.type, binding.value, completed->second, &e);
      return {Type::Void, ""};
    }

    if (nativeMath(e.castType)) return lowerNativeConstruct(e);
    if (analysis.nativeMethods.count(&e)) return lowerNativeMethod(e);
    if (analysis.mathCalls.count(&e)) return lowerMath(e);

    if (op == "Length" || op == "CodeUnitAt") return lowerTextQuery(e);

    if (op == "IsValid" && !e.receiver && e.args[0]->kind == "cast") {
      const auto &cast = *e.args[0];
      const auto &compared = analysis.enumCasts.at(&cast);
      auto v = expression(*cast.args[0]);

      if (enumValue(compared)) return {Type::Bool, "true"};

      return {Type::Bool, enumLookup(v, cast.castType.record, compared).second};
    }

    if (op == "IsValid" && !e.receiver) {
      auto valid = fresh();
      emit(valid + " = icmp ne ptr " + elementSlot(*e.args[0], true) + ", null");
      return {Type::Bool, valid};
    }

    if (op == "Exec" && !e.receiver) {
      // A file with only declarations has nothing to run.
      if (auto script = analysis.callTargets.find(&e); script != analysis.callTargets.end())
        emit("call void @duck." + script->second + "()");

      return {Type::Void, ""};
    }

    if (analysis.elementNames.count(&e)) {
      usesMarkupRuntime = true;
      return {Type::Text, load(load(location(*e.receiver).value))};
    }

    if (op == "$T.fail") {
      emit("call i32 (ptr, ...) @printf(ptr " + textConstant("%s\n") + ", ptr " +
           textConstant(e.args[0]->token.text) + ")");
      emit("store i1 true, ptr @duck$test.failed");
      return {Type::Void, ""};
    }

    if (op == "$T.show") {
      if (!printable(analysis.expressionTypes.at(e.args[0].get()))) return {Type::Void, ""};

      emit("call i32 (ptr, ...) @printf(ptr " + textConstant("%s") + ", ptr " +
           textConstant(e.args[1]->token.text) + ")");
    }

    if (op == "Print" || op == "$T.show") return lowerPrint(e);

    if (analysis.calendarCalls.count(&e))
      return calendarCall(e);

    if (!analysis.callTargets.count(&e))
      return {analysis.expressionTypes.at(&e), defaultValue(analysis.expressionTypes.at(&e))};

    return lowerDirectCall(e);
  }
  Value lowerReinitialization(const Expr &e, const FunctionAnalysis::Reinitialization &target) {
    // The new state is built first, so its arguments may read the old one.
    const auto value = functions.parts(target.type.record).empty()
                           ? defaultValue(target.type)
                           : constructRecord(target.type, target.calls, &e);
    const auto address = reinitializationAddress(e, target);
    // The old object is destroyed and releases its fields in place; the
    // storage, and so every alias and reference to it, stays the same.
    releaseBinding({target.type, address});
    emit("store " + irType(target.type) + " " + value + ", ptr " + address);
    return {Type::Void, ""};
  }
  std::string reinitializationAddress(const Expr &e, const FunctionAnalysis::Reinitialization &target) {
    std::string address;

    if (e.receiver) {
      const auto &base = *e.receiver;
      address = analysis.expressionTypes.at(&base).depth ? dereference(base).value : location(base).value;
      Type owner = Type::Record;
      owner.record = target.owner;
      auto field = fresh();
      emit(field + " = getelementptr " + irType(owner) + ", ptr " + address + ", i32 0" +
           indexList(target.field));
      address = field;
    } else {
      const auto binding = variable(e.token);
      requireWhole(binding);
      address = binding.value;

      if (binding.type.depth) {
        address = fresh();
        emit(address + " = load ptr, ptr " + binding.value);

        if (binding.type.role != Type::Raw) {
          if (!binding.scopedBorrow) {
            auto payload = fresh();
            emit(payload + " = call ptr @duck.heap$payload(ptr " + address + ", i1 " +
                 ownerFlag(binding.type) + ")");
            address = payload;
          }

          auto valid = fresh();
          emit(valid + " = icmp ne ptr " + address + ", null");
          require(valid);
        }
      }
    }

    return address;
  }
  // A vector's or matrix's scalars, column by column.
  std::vector<std::string> nativeParts(const Value &value) {
    const auto shape = nativeMath(value.type);
    std::vector<std::string> parts;

    for (unsigned column = 0; column < shape.columns; ++column)
      for (unsigned row = 0; row < shape.rows; ++row) {
        auto id = fresh();
        emit(id + " = extractvalue " + irType(value.type) + " " + value.value +
             (shape.matrix() ? ", " + std::to_string(column) : "") + ", " + std::to_string(row));
        parts.push_back(id);
      }

    return parts;
  }
  Value nativeBuild(const Type &type, const std::vector<std::string> &parts) {
    const auto shape = nativeMath(type);
    const Type scalar = shape.scalar;
    std::string result = "poison";

    for (unsigned column = 0; column < shape.columns; ++column)
      for (unsigned row = 0; row < shape.rows; ++row) {
        auto id = fresh();
        emit(id + " = insertvalue " + irType(type) + " " + result + ", " + irType(scalar) + " " +
             parts[column * shape.rows + row] + (shape.matrix() ? ", " + std::to_string(column) : "") + ", " +
             std::to_string(row));
        result = id;
      }

    return {type, result};
  }
  Value lowerNativeConstruct(const Expr &e) {
    const auto type = e.castType;
    const auto shape = nativeMath(type);
    const Type scalar = shape.scalar;
    const auto total = shape.columns * shape.rows;

    if (e.args.empty() && !shape.quaternion) return {type, "zeroinitializer"};
    if (e.args.empty()) return nativeBuild(type, {"0.0", "0.0", "0.0", "1.0"});

    const std::string zero = floating(scalar) ? "0.0" : "0", one = floating(scalar) ? "1.0" : "1";
    std::vector<Value> values;

    for (const auto &argument : e.args) values.push_back(expression(*argument));

    std::vector<std::string> parts(total, zero);
    const auto from = nativeMath(values[0].type);

    if (values.size() == 1 && !from) { // Fill a vector, or a matrix's diagonal.
      for (unsigned i = 0; i < total; ++i)
        if (!shape.matrix() || i % (shape.rows + 1) == 0) parts[i] = values[0].value;
    } else if (from.matrix()) { // The overlap is copied, and the rest is identity.
      const auto source = nativeParts(values[0]);

      for (unsigned column = 0; column < shape.columns; ++column)
        for (unsigned row = 0; row < shape.rows; ++row)
          parts[column * shape.rows + row] =
              column < from.columns && row < from.rows
                  ? convert({Type(from.scalar), source[column * from.rows + row]}, scalar).value
              : column == row ? one : zero;
    } else {
      unsigned next_part = 0;

      for (const auto &value : values) {
        const auto source = nativeMath(value.type);

        if (!source) {
          parts[next_part++] = value.value;
          continue;
        }

        for (const auto &part : nativeParts(value))
          if (next_part < total) parts[next_part++] = convert({Type(source.scalar), part}, scalar).value;
      }
    }

    return nativeBuild(type, parts);
  }
  Value nativeBinary(const std::string &op, const Value &a, const Value &b) {
    const auto left = nativeMath(a.type), right = nativeMath(b.type);
    const auto &shape = left ? left : right;
    const Type scalar = shape.scalar;
    const auto x = left ? nativeParts(a) : std::vector<std::string>(), y = right ? nativeParts(b) : std::vector<std::string>();
    auto apply = [&](const char *code, const std::string &p, const std::string &q) {
      return binary(code, {scalar, p}, {scalar, q}).value;
    };

    if (op == "==" || op == "!=") { // Equal only if every component is.
      auto same = std::string("true");

      for (size_t i = 0; i < x.size(); ++i) same = mathInstruction("and", Type::Bool, same, apply("==", x[i], y[i]));

      return {Type::Bool, op == "==" ? same : mathInstruction("xor", Type::Bool, same, "true")};
    }

    if (left.quaternion) return right.quaternion ? nativeBuild(a.type, quatProduct(x, y)) : nativeBuild(b.type, quatRotate(x, y));

    if (op == "*" && left.matrix() && right) { // Linear algebra, not componentwise.
      const auto n = left.rows;
      const auto columns = right.columns;
      std::vector<std::string> result;

      for (unsigned column = 0; column < columns; ++column)
        for (unsigned row = 0; row < n; ++row) {
          auto sum = apply("*", x[row], y[column * n]);

          for (unsigned k = 1; k < n; ++k) sum = apply("+", sum, apply("*", x[k * n + row], y[column * n + k]));

          result.push_back(sum);
        }

      return nativeBuild(b.type, result);
    }

    std::vector<std::string> result;
    const auto count = shape.columns * shape.rows;

    for (unsigned i = 0; i < count; ++i)
      result.push_back(apply(op.c_str(), left ? x[i] : a.value, right ? y[i] : b.value));

    return nativeBuild(left ? a.type : b.type, result);
  }
  // The determinant of an n by n matrix of scalars in column order.
  std::string nativeDeterminant(const Type &scalar, const std::vector<std::string> &m, unsigned n) {
    auto times = [&](const std::string &a, const std::string &b) { return mathInstruction("fmul", scalar, a, b); };

    if (n == 2) return mathInstruction("fsub", scalar, times(m[0], m[3]), times(m[2], m[1]));

    std::string sum;

    for (unsigned row = 0; row < n; ++row) {
      const auto term = times(m[row], nativeDeterminant(scalar, nativeMinor(m, n, 0, row), n - 1));

      if (sum.empty()) sum = term;
      else sum = mathInstruction(row % 2 ? "fsub" : "fadd", scalar, sum, term);
    }

    return sum;
  }
  static std::vector<std::string> nativeMinor(const std::vector<std::string> &m, unsigned n, unsigned column, unsigned row) {
    std::vector<std::string> minor;

    for (unsigned c = 0; c < n; ++c)
      for (unsigned r = 0; r < n; ++r)
        if (c != column && r != row) minor.push_back(m[c * n + r]);

    return minor;
  }
  // The adjugate over the determinant; a singular matrix gives infinities or
  // NaN, as any other floating division by zero does.
  Value nativeInverse(const Value &value) {
    const auto shape = nativeMath(value.type);
    const Type scalar = shape.scalar;
    const auto n = shape.rows;
    const auto m = nativeParts(value);
    std::vector<std::string> cofactors(n * n);

    for (unsigned column = 0; column < n; ++column)
      for (unsigned row = 0; row < n; ++row) {
        auto cofactor = nativeDeterminant(scalar, nativeMinor(m, n, column, row), n - 1);

        if ((column + row) % 2) {
          auto negated = fresh();
          emit(negated + " = fneg " + irType(scalar) + " " + cofactor);
          cofactor = negated;
        }

        cofactors[column * n + row] = cofactor;
      }

    auto determinant = mathInstruction("fmul", scalar, m[0], cofactors[0]);

    for (unsigned row = 1; row < n; ++row)
      determinant = mathInstruction("fadd", scalar, determinant, mathInstruction("fmul", scalar, m[row], cofactors[row]));

    std::vector<std::string> result(n * n);

    for (unsigned column = 0; column < n; ++column)
      for (unsigned row = 0; row < n; ++row)
        result[column * n + row] = mathInstruction("fdiv", scalar, cofactors[row * n + column], determinant);

    return nativeBuild(value.type, result);
  }
  std::string floatOp(const char *code, const std::string &a, const std::string &b) {
    return mathInstruction(code, Type::Float, a, b);
  }
  std::string floatMath(const char *name, const std::string &a) {
    return mathScalar(name, Type::Float, {{Type::Float, a}}).value;
  }
  std::string floatNegate(const std::string &a) {
    auto id = fresh();
    emit(id + " = fneg float " + a);
    return id;
  }
  // Quaternions are X, Y, Z, W. In a product, the right operand applies first.
  std::vector<std::string> quatProduct(const std::vector<std::string> &a, const std::vector<std::string> &b) {
    auto mul = [&](size_t i, size_t j) { return floatOp("fmul", a[i], b[j]); };
    auto add = [&](const std::string &p, const std::string &q) { return floatOp("fadd", p, q); };
    auto sub = [&](const std::string &p, const std::string &q) { return floatOp("fsub", p, q); };
    const auto x = sub(add(add(mul(3, 0), mul(0, 3)), mul(1, 2)), mul(2, 1));
    const auto y = add(add(sub(mul(3, 1), mul(0, 2)), mul(1, 3)), mul(2, 0));
    const auto z = add(sub(add(mul(3, 2), mul(0, 1)), mul(1, 0)), mul(2, 3));
    const auto w = sub(sub(sub(mul(3, 3), mul(0, 0)), mul(1, 1)), mul(2, 2));
    return {x, y, z, w};
  }
  std::vector<std::string> floatCross(const std::vector<std::string> &a, const std::vector<std::string> &b) {
    auto term = [&](size_t i, size_t j) {
      return floatOp("fsub", floatOp("fmul", a[i], b[j]), floatOp("fmul", a[j], b[i]));
    };
    const auto x = term(1, 2), y = term(2, 0), z = term(0, 1);
    return {x, y, z};
  }
  // v + w*t + u x t, where u is the vector part and t is 2 (u x v).
  std::vector<std::string> quatRotate(const std::vector<std::string> &q, const std::vector<std::string> &v) {
    auto t = floatCross(q, v);

    for (auto &part : t) part = floatOp("fmul", part, "2.0");

    const auto c = floatCross(q, t);
    std::vector<std::string> result;

    for (size_t i = 0; i < 3; ++i) result.push_back(floatOp("fadd", floatOp("fadd", v[i], floatOp("fmul", q[3], t[i])), c[i]));

    return result;
  }
  // Pitch about X, yaw about Y and roll about Z, in radians: roll applies
  // first, then pitch, then yaw.
  std::vector<std::string> quatFromEuler(const std::vector<std::string> &angles) {
    auto half = [&](size_t i) { return floatOp("fmul", angles[i], "0.5"); };
    const std::string zero = "0.0";
    const auto pitch = half(0), yaw = half(1), roll = half(2);
    const std::vector<std::string> x{floatMath("Sin", pitch), zero, zero, floatMath("Cos", pitch)};
    const std::vector<std::string> y{zero, floatMath("Sin", yaw), zero, floatMath("Cos", yaw)};
    const std::vector<std::string> z{zero, zero, floatMath("Sin", roll), floatMath("Cos", roll)};
    return quatProduct(quatProduct(y, x), z);
  }
  std::vector<std::string> eulerAngles(const std::vector<Value> &values, size_t first) {
    if (values.size() == first + 1) return nativeParts(values[first]);

    return {values[first].value, values[first + 1].value, values[first + 2].value};
  }
  std::vector<std::string> quatFromAxisAngle(const std::vector<Value> &values, size_t first) {
    const auto axis = nativeParts(lowerMathVector("Normalize", {values[first]}));
    const auto half = floatOp("fmul", values[first + 1].value, "0.5");
    const auto sine = floatMath("Sin", half);
    std::vector<std::string> result;

    for (const auto &part : axis) result.push_back(floatOp("fmul", part, sine));

    result.push_back(floatMath("Cos", half));
    return result;
  }
  // Column order, for a unit quaternion.
  std::vector<std::string> quatToMat3(const std::vector<std::string> &q) {
    auto twice = [&](size_t i, size_t j) { return floatOp("fmul", floatOp("fmul", q[i], q[j]), "2.0"); };
    auto add = [&](const std::string &p, const std::string &q2) { return floatOp("fadd", p, q2); };
    auto sub = [&](const std::string &p, const std::string &q2) { return floatOp("fsub", p, q2); };
    auto one = [&](const std::string &p, const std::string &q2) { return sub("1.0", add(p, q2)); };
    const auto xx = twice(0, 0), yy = twice(1, 1), zz = twice(2, 2);
    const auto xy = twice(0, 1), xz = twice(0, 2), yz = twice(1, 2);
    const auto wx = twice(3, 0), wy = twice(3, 1), wz = twice(3, 2);
    return {one(yy, zz), add(xy, wz), sub(xz, wy),
            sub(xy, wz), one(xx, zz), add(yz, wx),
            add(xz, wy), sub(yz, wx), one(xx, yy)};
  }
  // Inverts the order quatFromEuler composes in. At straight up or down, yaw
  // and roll turn the same axis, so all of it is reported as yaw.
  std::vector<std::string> quatToEuler(const std::vector<std::string> &q) {
    const auto m = quatToMat3(q);
    const auto m00 = m[0], m10 = m[1], m20 = m[2], m11 = m[4], m02 = m[6], m12 = m[7], m22 = m[8];
    const auto sine = mathScalar("Clamp", Type::Float, {{Type::Float, floatNegate(m12)}, {Type::Float, "-1.0"}, {Type::Float, "1.0"}}).value;
    const auto pitch = floatMath("Asin", sine);
    auto atan2 = [&](const std::string &y, const std::string &x) {
      return mathScalar("Atan2", Type::Float, {{Type::Float, y}, {Type::Float, x}}).value;
    };
    const auto magnitude = mathCall("llvm.fabs.f32", Type::Float, {sine});
    const auto locked = floatOp("fcmp oge", magnitude, "0.999755859375");
    const auto yaw = mathSelect(locked, Type::Float, atan2(floatNegate(m20), m00), atan2(m02, m22));
    const auto roll = mathSelect(locked, Type::Float, "0.0", atan2(m10, m11));
    return {pitch, yaw, roll};
  }
  // Takes the shorter way round, and falls back to a normalized straight line
  // when the two are so close that the angle cannot be divided by.
  Value quatSlerp(const Value &from, const Value &to, const std::string &amount) {
    const auto a = nativeParts(from);
    auto b = nativeParts(to);
    auto cosine = floatOp("fmul", a[0], b[0]);

    for (size_t i = 1; i < 4; ++i) cosine = floatOp("fadd", cosine, floatOp("fmul", a[i], b[i]));

    const auto opposite = floatOp("fcmp olt", cosine, "0.0");
    cosine = mathSelect(opposite, Type::Float, floatNegate(cosine), cosine);

    for (auto &part : b) part = mathSelect(opposite, Type::Float, floatNegate(part), part);

    const auto theta = floatMath("Acos", cosine);
    const auto sine = floatMath("Sin", theta);
    const auto fromWeight = floatOp("fdiv", floatMath("Sin", floatOp("fmul", floatOp("fsub", "1.0", amount), theta)), sine);
    const auto toWeight = floatOp("fdiv", floatMath("Sin", floatOp("fmul", amount, theta)), sine);
    std::vector<std::string> curved, straight;

    for (size_t i = 0; i < 4; ++i) {
      curved.push_back(floatOp("fadd", floatOp("fmul", a[i], fromWeight), floatOp("fmul", b[i], toWeight)));
      straight.push_back(floatOp("fadd", a[i], floatOp("fmul", floatOp("fsub", b[i], a[i]), amount)));
    }

    const auto line = nativeParts(lowerMathVector("Normalize", {nativeBuild(from.type, straight)}));
    const auto close = floatOp("fcmp ogt", cosine, "0.99951171875");
    std::vector<std::string> result;

    for (size_t i = 0; i < 4; ++i) result.push_back(mathSelect(close, Type::Float, line[i], curved[i]));

    return nativeBuild(from.type, result);
  }
  // The methods only quat and rot have; the rest are shared with vectors.
  std::optional<Value> lowerRotationMethod(const std::string &name, const Value &self, const std::vector<Value> &values) {
    const auto shape = nativeMath(self.type);
    const auto quat = nativeNamed("quat");

    if (name == "FromEuler") return nativeBuild(quat, quatFromEuler(eulerAngles(values, 1)));
    if (name == "FromAxisAngle") return nativeBuild(quat, quatFromAxisAngle(values, 1));
    if (name == "Slerp") return quatSlerp(self, values[1], values[2].value);

    if (name != "Rotate" && name != "ToMat3" && name != "ToMat4" && name != "ToQuat" && name != "ToEuler" &&
        name != "Conjugate" && name != "Inverse")
      return std::nullopt;

    const auto q = shape.euler ? quatFromEuler(nativeParts(self)) : nativeParts(self);

    if (name == "Rotate") return nativeBuild(values[1].type, quatRotate(q, nativeParts(values[1])));
    if (name == "ToQuat") return nativeBuild(quat, q);
    if (name == "ToEuler") return nativeBuild(nativeNamed("rot"), quatToEuler(q));
    if (name == "ToMat3") return nativeBuild(nativeNamed("mat3"), quatToMat3(q));

    if (name == "ToMat4") {
      const auto m = quatToMat3(q);
      return nativeBuild(nativeNamed("mat4"), {m[0], m[1], m[2], "0.0", m[3], m[4], m[5], "0.0",
                                               m[6], m[7], m[8], "0.0", "0.0", "0.0", "0.0", "1.0"});
    }

    std::string lengthSquared = floatOp("fmul", q[0], q[0]);

    for (size_t i = 1; i < 4; ++i) lengthSquared = floatOp("fadd", lengthSquared, floatOp("fmul", q[i], q[i]));

    std::vector<std::string> result{floatNegate(q[0]), floatNegate(q[1]), floatNegate(q[2]), q[3]};

    if (name == "Inverse")
      for (auto &part : result) part = floatOp("fdiv", part, lengthSquared);

    return nativeBuild(self.type, result);
  }
  Value lowerNativeMethod(const Expr &e) {
    const auto name = memberName(e.token.text);

    if (!e.receiver) { // vec3.Identity(), mat4.Zero() and the like.
      const auto type = analysis.expressionTypes.at(&e);
      const auto shape = nativeMath(type);

      if (name == "FromEuler" || name == "FromAxisAngle") {
        std::vector<Value> values;

        for (const auto &argument : e.args) values.push_back(expression(*argument));

        return nativeBuild(type, name == "FromEuler" ? quatFromEuler(eulerAngles(values, 0)) : quatFromAxisAngle(values, 0));
      }

      const std::string zero = floating(shape.scalar) ? "0.0" : "0", one = floating(shape.scalar) ? "1.0" : "1";
      std::vector<std::string> parts(shape.columns * shape.rows, name == "Zero" || shape.euler ? zero : one);

      if (name == "Identity" && shape.matrix())
        for (unsigned i = 0; i < parts.size(); ++i)
          if (i % (shape.rows + 1)) parts[i] = zero;

      if (shape.quaternion) parts = {zero, zero, zero, one};

      return nativeBuild(type, parts);
    }

    const auto held = analysis.expressionTypes.at(e.receiver.get());
    const bool inPlace = analysis.nativeInPlace.count(&e) != 0;
    std::string address;
    Value self;

    if (held.depth || inPlace) { // Taken once, so an index in the receiver runs once.
      const Type stored = held.depth ? held.pointee() : held;
      address = held.depth ? dereference(*e.receiver).value : location(*e.receiver).value;
      auto loaded = fresh();
      emit(loaded + " = load " + irType(stored) + ", ptr " + address);
      self = {stored, loaded};
    } else
      self = expression(*e.receiver);

    const auto shape = nativeMath(self.type);
    const Type scalar = shape.scalar;
    std::vector<Value> values{self};

    for (const auto &argument : e.args) values.push_back(expression(*argument));

    if (shape.quaternion || shape.euler)
      if (const auto rotation = lowerRotationMethod(name, self, values)) {
        if (inPlace) emit("store " + irType(rotation->type) + " " + rotation->value + ", ptr " + address);

        return *rotation;
      }

    if (name == "Inverse") return nativeInverse(self);
    if (name == "Determinant") return {scalar, nativeDeterminant(scalar, nativeParts(self), shape.rows)};

    if (name == "Transpose") {
      const auto parts = nativeParts(self);
      std::vector<std::string> flipped(parts.size());

      for (unsigned column = 0; column < shape.columns; ++column)
        for (unsigned row = 0; row < shape.rows; ++row) flipped[column * shape.rows + row] = parts[row * shape.rows + column];

      return nativeBuild(self.type, flipped);
    }

    if (name == "Abs" || name == "Min" || name == "Max") {
      auto parts = nativeParts(self);
      const auto other = name == "Abs" ? std::vector<std::string>() : nativeParts(values[1]);

      for (size_t i = 0; i < parts.size(); ++i) {
        std::vector<Value> operands{{scalar, parts[i]}};

        if (!other.empty()) operands.push_back({scalar, other[i]});

        parts[i] = mathScalar(name, scalar, operands).value;
      }

      return nativeBuild(self.type, parts);
    }

    if (name == "Lerp") return binary("+", self, binary("*", binary("-", values[1], self), values[2]));

    const auto result = lowerMathVector(name, values);

    if (inPlace) emit("store " + irType(result.type) + " " + result.value + ", ptr " + address);

    return result;
  }
  Value printNative(const Value &value) {
    const auto shape = nativeMath(value.type);
    const auto parts = nativeParts(value);
    const std::string spec = floating(shape.scalar) ? "%g" : "%d";
    std::string format = shape.matrix() ? "[" : "", arguments;

    for (unsigned column = 0; column < shape.columns; ++column) {
      format += column ? ", (" : "(";

      for (unsigned row = 0; row < shape.rows; ++row) {
        format += (row ? ", " : "") + spec;
        const auto part = convert({Type(shape.scalar), parts[column * shape.rows + row]},
                                  floating(shape.scalar) ? Type(Type::Double) : Type(Type::Int));
        arguments += ", " + irType(part.type) + " " + part.value;
      }

      format += ")";
    }

    emit("call i32 (ptr, ...) @printf(ptr " + textConstant(format + (shape.matrix() ? "]\n" : "\n")) + arguments + ")");
    return {Type::Void, ""};
  }
  std::string mathCall(const std::string &symbol, const Type &type, const std::vector<std::string> &operands) {
    const auto ir = irType(type);
    std::string parameters, arguments;

    for (const auto &operand : operands) {
      parameters += (parameters.empty() ? "" : ", ") + ir;
      arguments += (arguments.empty() ? "" : ", ") + ir + " " + operand;
    }

    mathImports[symbol] = "declare " + ir + " @" + symbol + "(" + parameters + ")\n";
    auto id = fresh();
    emit(id + " = call " + ir + " @" + symbol + "(" + arguments + ")");
    return id;
  }
  std::string mathInstruction(const std::string &opcode, const Type &type, const std::string &a, const std::string &b) {
    auto id = fresh();
    emit(id + " = " + opcode + " " + irType(type) + " " + a + ", " + b);
    return id;
  }
  std::string mathLess(const Type &type, const std::string &a, const std::string &b) {
    return mathInstruction(floating(type) ? "fcmp olt" : unsignedInteger(type) ? "icmp ult" : "icmp slt", type, a, b);
  }
  std::string mathSelect(const std::string &condition, const Type &type, const std::string &yes, const std::string &no) {
    auto id = fresh();
    emit(id + " = select i1 " + condition + ", " + irType(type) + " " + yes + ", " + irType(type) + " " + no);
    return id;
  }
  // The address path of a vector container's field, as an extractvalue list.
  std::string mathField(const Type &vector, const std::string &name) {
    MemberPath path;
    functions.findField(vector.record, name, path);
    std::string list;

    for (auto index : path.indices) list += ", " + std::to_string(index);

    return list;
  }
  Value lowerMathVector(const std::string &name, const std::vector<Value> &operands) {
    const auto vector = operands[0].type;
    const auto &fields = functions.records.at(vector.record).fields;
    const Type component = fields[0].type;
    using Components = std::vector<std::string>;
    auto read = [&](const Value &value) {
      Components parts;

      for (const auto &field : fields) {
        auto id = fresh();
        emit(id + " = extractvalue " + irType(vector) + " " + value.value + mathField(vector, field.name.text));
        parts.push_back(id);
      }

      return parts;
    };
    // Starting from an operand keeps anything the layout holds besides fields.
    auto build = [&](const Components &parts) {
      auto result = operands[0].value;

      for (size_t i = 0; i < fields.size(); ++i) {
        auto id = fresh();
        emit(id + " = insertvalue " + irType(vector) + " " + result + ", " + irType(component) + " " + parts[i] +
             mathField(vector, fields[i].name.text));
        result = id;
      }

      return Value{vector, result};
    };
    auto dot = [&](const Components &a, const Components &b) {
      auto sum = mathInstruction("fmul", component, a[0], b[0]);

      for (size_t i = 1; i < a.size(); ++i)
        sum = mathInstruction("fadd", component, sum, mathInstruction("fmul", component, a[i], b[i]));

      return sum;
    };
    const auto sqrt = "llvm.sqrt." + std::string(component == Type::Float ? "f32" : "f64");
    const auto a = read(operands[0]);

    if (name == "Length") return {component, mathCall(sqrt, component, {dot(a, a)})};
    if (name == "LengthSquared") return {component, dot(a, a)};

    if (name == "Normalize") {
      const auto length = mathCall(sqrt, component, {dot(a, a)});
      const auto nonzero = mathInstruction("fcmp une", component, length, "0.0");
      Components parts;

      // A zero vector has no direction and stays zero instead of becoming NaN.
      for (const auto &part : a)
        parts.push_back(mathSelect(nonzero, component, mathInstruction("fdiv", component, part, length), "0.0"));

      return build(parts);
    }

    const auto b = read(operands[1]);

    if (name == "Dot") return {component, dot(a, b)};

    if (name == "Distance") {
      Components difference;

      for (size_t i = 0; i < a.size(); ++i) difference.push_back(mathInstruction("fsub", component, a[i], b[i]));

      return {component, mathCall(sqrt, component, {dot(difference, difference)})};
    }

    auto cross = [&](size_t i, size_t j) {
      return mathInstruction("fsub", component, mathInstruction("fmul", component, a[i], b[j]),
                             mathInstruction("fmul", component, a[j], b[i]));
    };
    const auto x = cross(1, 2), y = cross(2, 0), z = cross(0, 1);
    return build({x, y, z});
  }
  Value lowerMath(const Expr &e) {
    const auto name = memberName(e.token.text);
    std::vector<Value> values;

    for (const auto &argument : e.args) values.push_back(expression(*argument));

    if (recordValue(values[0].type)) return lowerMathVector(name, values);

    return mathScalar(name, analysis.expressionTypes.at(&e), values);
  }
  Value mathScalar(const std::string &name, const Type &type, const std::vector<Value> &values) {
    const std::string suffix = type == Type::Float ? ".f32" : ".f64";
    const auto &a = values[0].value;
    const auto zero = floating(type) ? std::string("0.0") : std::string("0");
    const auto one = floating(type) ? std::string("1.0") : std::string("1");
    const auto minusOne = "-" + one;

    if (name == "Abs") {
      if (floating(type)) return {type, mathCall("llvm.fabs" + suffix, type, {a})};
      if (unsignedInteger(type)) return values[0];

      return {type, mathSelect(mathLess(type, a, zero), type, mathInstruction("sub", type, zero, a), a)};
    }

    if (name == "Sign") {
      const auto negative = mathSelect(mathLess(type, a, zero), type, unsignedInteger(type) ? zero : minusOne,
                                       floating(type) ? a : zero); // NaN and signed zeros stay as they are.
      return {type, mathSelect(mathLess(type, zero, a), type, one, negative)};
    }

    if (name == "Min" || name == "Max") {
      const auto &b = values[1].value;

      if (floating(type)) return {type, mathCall((name == "Min" ? "llvm.minnum" : "llvm.maxnum") + suffix, type, {a, b})};

      return {type, name == "Min" ? mathSelect(mathLess(type, b, a), type, b, a)
                                  : mathSelect(mathLess(type, a, b), type, b, a)};
    }

    if (name == "Clamp") {
      const auto &low = values[1].value, &high = values[2].value;
      const auto capped = mathSelect(mathLess(type, high, a), type, high, a);
      return {type, mathSelect(mathLess(type, a, low), type, low, capped)};
    }

    if (name == "Lerp") {
      const auto span = mathInstruction("fsub", type, values[1].value, a);
      return {type, mathInstruction("fadd", type, a, mathInstruction("fmul", type, span, values[2].value))};
    }

    // Exact operations round the same at either width, so float stays float.
    static const std::map<std::string, std::string> exact{
        {"Sqrt", "llvm.sqrt"}, {"Floor", "llvm.floor"}, {"Ceil", "llvm.ceil"}, {"Round", "llvm.round"}, {"Trunc", "llvm.trunc"}};

    if (auto intrinsic = exact.find(name); intrinsic != exact.end())
      return {type, mathCall(intrinsic->second + suffix, type, {a})};

    // The rest run at double and round once, the same result #-evaluation gets.
    static const std::map<std::string, std::string> inDouble{
        {"Sin", "llvm.sin.f64"},    {"Cos", "llvm.cos.f64"},     {"Exp", "llvm.exp.f64"},   {"Exp2", "llvm.exp2.f64"},
        {"Log", "llvm.log.f64"},    {"Log2", "llvm.log2.f64"},   {"Log10", "llvm.log10.f64"}, {"Pow", "llvm.pow.f64"},
        {"Tan", "tan"},             {"Asin", "asin"},            {"Acos", "acos"},          {"Atan", "atan"},
        {"Sinh", "sinh"},           {"Cosh", "cosh"},            {"Tanh", "tanh"},          {"Cbrt", "cbrt"},
        {"Atan2", "atan2"},         {"Hypot", "hypot"},          {"Mod", "fmod"}};
    std::vector<std::string> operands;

    for (const auto &value : values) operands.push_back(convert(value, Type::Double).value);

    return convert({Type::Double, mathCall(inDouble.at(name), Type::Double, operands)}, type);
  }
  Value lowerTextQuery(const Expr &e) {
    const auto &op = e.token.text;
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
  Value lowerPrint(const Expr &e) {
    auto v = expression(*e.args[0]);

    if (nativeMath(v.type)) return printNative(v);

    if (bigInteger(v.type)) {
      usesBigRuntime = true;
      emit("call void @duck.big.print(ptr " + v.value + ")");
      release(v);
      return {Type::Void, ""};
    }

    if (decimalValue(v.type)) {
      usesDecimalRuntime = true;
      emit("call void @duck.tiny.print({ i256, i32 } " + v.value + ")");
      return {Type::Void, ""};
    }

    if (enumValue(v.type)) {
      const auto count = functions.enums.at(v.type.record).members.size();
      auto slot = fresh(), label = fresh();
      emit(slot + " = getelementptr [" + std::to_string(count) + " x ptr], ptr @enum." +
           v.type.record + ".names, i32 0, i32 " + v.value);
      emit(label + " = load ptr, ptr " + slot);
      emit("call i32 (ptr, ...) @printf(ptr " + textConstant("%s\n") + ", ptr " + label + ")");
      return {Type::Void, ""};
    }

    if (v.type == Type::Char) v = convert(v, Type::Name);

    if (managedText(v.type)) {
      usesTextRuntime = true;
      emit("call void @duck.text.print(ptr " + v.value + ", i64 " + textWidth(v.type) + ")");
      release(v);
      return {Type::Void, ""};
    }

    if (calendarValue(v.type)) {
          printCalendar(v);
          return {Type::Void, ""};
        }

    return printScalar(v);
  }
  void printCalendar(const Value &v) {
    usesCalendarRuntime = true;
    auto days = v.type == Type::Date ? v.value : "0";
    auto nanos = v.type == Type::Time ? v.value : "0";

    if (v.type == Type::DateTime) {
      days = part(v, 0);
      nanos = part(v, 1);
    }

    const auto mode = v.type == Type::Date ? "1" : v.type == Type::Time ? "2" : "3";
    emit("call void @duck.calendar.print(i32 " + days + ", i64 " + nanos + ", i32 " +
         mode + ")");
  }
  Value printScalar(Value v) {
    if (v.type == Type::Bool) {
      auto id = fresh();
      emit(id + " = zext i1 " + v.value + " to i32");
      v = {Type::Int, id};
    }

    if (v.type == Type::Float)
      v = convert(v, Type::Double);

    if (integral(v.type) && integerWidth(v.type) > 64) {
      printWide(v);
      return {Type::Void, ""};
    }

    // All 8/16/24-bit values fit C's int varargs promotion; 40..56-bit
    // values print through the 64-bit formats.
    if (integral(v.type) && integerWidth(v.type) < 32)
      v = convert(v, Type::Int);
    else if (integral(v.type) && integerWidth(v.type) < 64)
      v = convert(v, unsignedInteger(v.type) ? Type::UInt64 : Type::Long);

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
  Value lowerDirectCall(const Expr &e) {
    const auto &target = analysis.callTargets.at(&e);
    auto found = functions.find(target);

    const auto &f = found->second;

    std::string callee = "@" + (f.nativeSymbol.empty() ? "duck." + target : f.nativeSymbol);
    Value temporaryReceiver{Type::Void, ""};
    std::vector<Value> arguments;
    const auto args = callArguments(e, target, f, callee, temporaryReceiver, arguments);
    // A native enum result arrives as its underlying value and must name a member.
    const bool nativeEnum = !f.nativeSymbol.empty() && enumValue(f.result);
    const auto result = nativeEnum ? functions.enums.at(f.result.record).type : f.result;
    const auto id = emitCall(result, callee, args);

    for (auto argument : arguments) release(argument);
    // A temporary receiver dies after the call, holding whatever the method left in it.
    if (!temporaryReceiver.value.empty()) releaseBinding({temporaryReceiver.type, temporaryReceiver.value});
    if (mayUnwind(target)) unwindCheck();

    if (nativeEnum) {
      const auto [index, found] = enumLookup({result, id}, f.result.record, result);
      require(found);
      return {f.result, index};
    }

    return {f.result, id};
  }
  std::optional<Value> lowerLiteral(const Expr &e) {
    const auto &op = e.token.text;
    auto member = analysis.enumMembers.find(&e);

    if (member != analysis.enumMembers.end())
      return Value{analysis.expressionTypes.at(&e), std::to_string(member->second)};

    // Day.Count.
    if (e.kind == ".")
      if (const auto count = analysis.integerConstants.find(&e); count != analysis.integerConstants.end())
        return Value{analysis.expressionTypes.at(&e), count->second};

    if (e.kind == "char")
      return Value{Type::Char, std::to_string(decodeUTF8(e.token).front())};

    if (e.kind == "construct" && nativeMath(e.castType)) return lowerNativeConstruct(e);

    if (e.kind == "construct")
      return construct(e);

    if (e.kind == "real" || e.kind == "typeMember")
      return Value{analysis.expressionTypes.at(&e),
              analysis.floatingConstants.at(&e)};

    if (e.kind == "null")
      return Value{analysis.expressionTypes.at(&e), "null"};

    if (e.kind == "bool")
      return Value{Type::Bool, op == "true" ? "1" : "0"};

    if (e.kind == "text") {
      auto type = analysis.expressionTypes.at(&e);

      if (managedText(type)) return managedLiteral(e, type);

      return Value{Type::Text, textConstant(op)};
    }

    return std::nullopt;
  }
  Value lowerConditional(const Expr &e, const Value &a, const std::string &id) {
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
  Value lowerShortCircuit(const Expr &e, const Value &a, const std::string &id) {
    const auto &op = e.token.text;
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
  Value lowerExpression(const Expr &e) {
    const auto &op = e.token.text;

    if (e.kind == "compileTime") {
      const auto type = analysis.expressionTypes.at(&e);

      if (type == Type::Void) return {type, ""};
      if (floating(type)) return {type, analysis.floatingConstants.at(&e)};

      return {type, analysis.integerConstants.at(&e)};
    }

    if (e.kind == "mutation") return mutate(*e.args[0], op, e.args.size() > 1 ? e.args[1].get() : nullptr, true);
    if (e.kind == "moveCheck") return moveIntoField(*e.args[0], *e.args[1], true);
    if (e.kind == "lambda") return closure(e);

    if (e.kind == "invoke" || analysis.indirectCalls.count(&e)) {
      const auto value = indirectCall(e);

      if (analysis.unwindIndirect) unwindCheck();

      return value;
    }

    if (e.kind == "duckCall" || e.kind == "duckCheck" || e.kind == "duckExists") {
      const auto value = duckCall(e, false);

      if (e.kind == "duckCall" && analysis.unwindIndirect) unwindCheck();

      return value;
    }

    if (e.kind == "guard" && e.args[0]->kind == "duckCall") return duckCall(*e.args[0], true);
    if (analysis.reflections.count(&e)) return reflect(e);

    if (auto function = analysis.functionValues.find(&e); function != analysis.functionValues.end())
      return functionClosure(function->second, analysis.expressionTypes.at(&e));

    if (e.kind == "guard" && e.args[0]->kind == "call") return guardedCall(e);
    if (e.kind == "guard") return readOrDefault(elementSlot(*e.args[0], true), analysis.expressionTypes.at(&e));

    // Reading an unwritten grid cell gives the default without storing it.
    if (std::vector<const Expr *> coordinates; e.kind == "index")
      if (const auto *grid = gridRoot(e, coordinates))
        return readOrDefault(gridSlot(*grid, coordinates, false), analysis.expressionTypes.at(&e));

    auto integer = analysis.integerConstants.find(&e);

    if (integer != analysis.integerConstants.end())
    {
      const auto type = analysis.expressionTypes.at(&e);

      if (bigInteger(type)) return bigLiteral(integer->second, type);

      return {type, integer->second};
    }

    auto decimal = analysis.decimalConstants.find(&e);

    if (decimal != analysis.decimalConstants.end())
      return {analysis.expressionTypes.at(&e),
              "{ i256 " + decimal->second.first + ", i32 " + decimal->second.second + " }"};

    if (e.kind == "range") return rangeArray(e);

    if (e.kind == "initializer" && !recordValue(analysis.expressionTypes.at(&e)))
      return literal(e, analysis.expressionTypes.at(&e));

    if (e.kind == "initializer") return lowerRecordInitializer(e);

    if (e.kind == "cast" && enumValue(e.castType)) return lowerEnumCast(e);

    if (e.kind == "cast")
      return convert(expression(*e.args[0]), e.castType);

    if (e.kind == "scopeActive") {
      auto id = fresh();
      emit(id + " = call i1 @duck.scope$active(ptr " + scopeDescriptor(e.token.text) + ")");
      return {Type::Bool, id};
    }

    if (e.kind == "markupTake") {
      const auto binding = variable(e.token);
      const auto value = load(binding.value);
      emit("store ptr null, ptr " + binding.value);
      return {binding.type, value};
    }

    if (e.kind == "macroValue") {
      // The result holds its own reference, so the bindings can end first.
      scopes.emplace_back();

      for (const auto &binding : e.bindings) statement(*binding);

      const auto value = expression(*e.args[0]);
      cleanupScopes(scopes.size() - 1);
      scopes.pop_back();
      return value;
    }

    if (auto value = lowerLiteral(e)) return std::move(*value);

    if (e.kind == "unary" && op == "&") {
      auto target = analysis.callTargets.find(&e);

      if (target != analysis.callTargets.end()) {
        const auto &native = functions.at(target->second).nativeSymbol;
        return {Type::Pointer, native.empty() ? "@duck." + target->second : "@" + native};
      }

      return {analysis.expressionTypes.at(&e), location(*e.args[0]).value};
    }

    if (auto property = analysis.properties.find(&e); property != analysis.properties.end()) {
      const auto &access = property->second;
      const auto r = receiver(e.kind == "." ? e.args[0].get() : nullptr, e.token);
      auto v = accessorCall(r, access.owner, access.getter, access.getterPath);
      releaseTemporary(r);
      return v;
    }

    if (auto member = analysis.collectionMembers.find(&e); member != analysis.collectionMembers.end())
      return lowerCollectionMember(e, member->second);

    if (auto property = analysis.enumProperties.find(&e); property != analysis.enumProperties.end())
      return lowerEnumProperty(e, property->second);

    if (e.kind == "index" && analysis.expressionTypes.at(e.args[0].get()).base == Type::Pair &&
        !analysis.expressionTypes.at(e.args[0].get()).depth) {
      return loadSlot(elementSlot(e, false), analysis.expressionTypes.at(&e));
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

    if (e.kind == "call") return lowerCall(e);

    auto a = expression(*e.args[0]);
    auto id = fresh();

    if (e.kind == "conditional") return lowerConditional(e, a, id);
    if (op == "&&" || op == "||") return lowerShortCircuit(e, a, id);

    if (e.kind == "unary" && bigInteger(a.type)) {
      usesBigRuntime = true;
      emit(id + " = call ptr @duck.big.negate(ptr " + a.value + ")");
      release(a);
      return {a.type, id};
    }

    if (e.kind == "unary" && decimalValue(a.type)) {
      auto coefficient = fresh(), negated = fresh();
      emit(coefficient + " = extractvalue { i256, i32 } " + a.value + ", 0");
      emit(negated + " = sub i256 0, " + coefficient);
      emit(id + " = insertvalue { i256, i32 } " + a.value + ", i256 " + negated + ", 0");
      return {a.type, id};
    }

    if (e.kind == "unary" && nativeMath(a.type)) {
      const Type scalar = nativeMath(a.type).scalar;
      auto parts = nativeParts(a);

      for (auto &part : parts) {
        auto negated = fresh();
        emit(negated + (floating(scalar) ? " = fneg " + irType(scalar) + " " + part
                                         : " = sub " + irType(scalar) + " 0, " + part));
        part = negated;
      }

      return nativeBuild(a.type, parts);
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

    if (e.args.size() == 3) {
      const auto outer = tolerance;
      tolerance = expression(*e.args[2]).value;
      const auto result = binary(op, a, b);
      tolerance = outer;
      return result;
    }

    return binary(op, a, b);
  }
  // Within the written tolerance, or by default within a small margin that
  // grows with the operands' size. Equal infinities match; NaN never does.
  Value floatEquality(const std::string &op, const Value &a, const Value &b) {
    const auto &type = a.type;
    const std::string suffix = type == Type::Float ? ".f32" : ".f64";
    const auto fabs = "llvm.fabs" + suffix, maxnum = "llvm.maxnum" + suffix;
    const auto exact = mathInstruction("fcmp oeq", type, a.value, b.value);
    const auto gap = mathCall(fabs, type, {mathInstruction("fsub", type, a.value, b.value)});
    std::string limit;

    if (tolerance) limit = *tolerance;
    else {
      const auto larger = mathCall(maxnum, type, {mathCall(fabs, type, {a.value}), mathCall(fabs, type, {b.value})});
      limit = mathInstruction("fmul", type, floatEpsilon(type), mathCall(maxnum, type, {larger, "1.0"}));
    }

    const auto close = mathInstruction("fcmp ole", type, gap, limit);
    const auto finite = mathInstruction("fcmp one", type, gap, "0x7FF0000000000000");
    const auto equal = mathInstruction("or", Type::Bool, exact, mathInstruction("and", Type::Bool, close, finite));
    return {Type::Bool, op == "==" ? equal : mathInstruction("xor", Type::Bool, equal, "true")};
  }
  static std::string floatEpsilon(const Type &type) {
    const double value = type == Type::Float ? double(1e-5f) : 1e-9;
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    std::ostringstream text;
    text << "0x" << std::uppercase << std::hex << std::setfill('0') << std::setw(16) << bits;
    return text.str();
  }
  // The word capture `index` occupies in the closure at `base`.
  std::string captureSlot(const std::string &base, size_t index) {
    auto slot = fresh();
    emit(slot + " = getelementptr i8, ptr " + base + ", i64 " + std::to_string(24 + 8 * index));
    return slot;
  }
  // A lambda expression: a constant closure, or one holding its captures.
  Value closure(const Expr &e) {
    using Capture = FunctionAnalysis::Capture;
    usesFuncRuntime = true;
    const auto &info = analysis.lambdas.at(&e);
    const auto type = analysis.expressionTypes.at(&e);
    const auto id = std::to_string(lambdaCount++);
    const auto function = "@duck$lambda." + symbolPrefix + "." + id;
    Generator inner(functions, analysis, symbolPrefix + "$l" + id);
    thunks << inner.lambdaBody(*e.lambda, info, function);
    absorb(inner);

    if (info.captures.empty()) {
      const auto symbol = "@duck$closure." + symbolPrefix + "." + id;
      globals << symbol << " = private unnamed_addr constant { i64, ptr, ptr } { i64 "
              << (info.bound ? "-2" : "-1") << ", ptr null, ptr " << function << " }\n";
      return {type, symbol};
    }

    size_t words = 0;

    for (const auto &capture : info.captures)
      words += capture.kind == Capture::This || (capture.kind == Capture::Borrow && recordValue(capture.type)) ? 2 : 1;

    auto cell = fresh(), slot = fresh();
    emit(cell + " = call ptr @duck.cell$new(i64 " + std::to_string(24 + 8 * words) + ", ptr " + function + ".drop)");
    emit(slot + " = getelementptr i8, ptr " + cell + ", i64 16");
    emit("store ptr " + function + ", ptr " + slot);
    // Dispose releases what the closure holds; borrowed storage needs nothing.
    std::string dispose = "define internal void " + function + ".drop(ptr %c) {\nentry:\n";
    size_t word = 0;

    for (const auto &capture : info.captures) {
      const auto binding = variable(capture.token);
      const auto target = captureSlot(cell, word);
      const auto offset = std::to_string(24 + 8 * word);

      if (capture.kind == Capture::Box) {
        auto box = fresh();
        emit(box + " = load ptr, ptr " + binding.box);
        emit("call void @duck.cell$retain(ptr " + box + ")");
        emit("store ptr " + box + ", ptr " + target);
        dispose += "  %s" + offset + " = getelementptr i8, ptr %c, i64 " + offset + "\n  %b" + offset +
                   " = load ptr, ptr %s" + offset + "\n  call void @duck.cell$release(ptr %b" + offset + ")\n";
        ++word;
      } else if (capture.kind == Capture::Alias) {
        usesHeapRuntime = true;
        auto alias = fresh();
        emit(alias + " = call ptr @duck.heap$alias(ptr " + binding.value + ", i1 " + ownerFlag(binding.type) + ")");
        emit("store ptr " + alias + ", ptr " + target);
        dispose += "  %s" + offset + " = getelementptr i8, ptr %c, i64 " + offset +
                   "\n  call void @duck.heap$drop(ptr %s" + offset + ", i1 false, ptr null)\n";
        ++word;
      } else {
        emit("store ptr " + binding.value + ", ptr " + target);
        ++word;

        if (capture.kind == Capture::This || recordValue(capture.type)) {
          emit("store ptr " + binding.table + ", ptr " + captureSlot(cell, word));
          ++word;
        }
      }
    }

    thunks << dispose << "  ret void\n}\n\n";
    return {type, cell};
  }
  void absorb(Generator &inner) {
    globals << inner.globals.str();
    usesTextRuntime = usesTextRuntime || inner.usesTextRuntime;
    usesHeapRuntime = usesHeapRuntime || inner.usesHeapRuntime;
    usesCalendarRuntime = usesCalendarRuntime || inner.usesCalendarRuntime;
    usesWideRuntime = usesWideRuntime || inner.usesWideRuntime;
    usesBigRuntime = usesBigRuntime || inner.usesBigRuntime;
    usesDecimalRuntime = usesDecimalRuntime || inner.usesDecimalRuntime;
    usesCollectionRuntime = usesCollectionRuntime || inner.usesCollectionRuntime;
    usesMetaRuntime = usesMetaRuntime || inner.usesMetaRuntime;
    usesMarkupRuntime = usesMarkupRuntime || inner.usesMarkupRuntime;
    usesScopeRuntime = usesScopeRuntime || inner.usesScopeRuntime;
    usesFileRuntime = usesFileRuntime || inner.usesFileRuntime;
    usesLockRuntime = usesLockRuntime || inner.usesLockRuntime;
    scopeNames.insert(inner.scopeNames.begin(), inner.scopeNames.end());
    mathImports.insert(inner.mathImports.begin(), inner.mathImports.end());
    cleanups.insert(inner.cleanups.begin(), inner.cleanups.end());
    destroys.insert(inner.destroys.begin(), inner.destroys.end());
    tables.insert(inner.tables.begin(), inner.tables.end());
    types.insert(inner.types.begin(), inner.types.end());
    helpers.insert(inner.helpers.begin(), inner.helpers.end());
  }
  // The generated function of a lambda. Its first parameter is the closure,
  // or for a member lambda the object it was called on.
  std::string lambdaBody(const Function &f, const FunctionAnalysis::LambdaInfo &info, const std::string &symbol) {
    using Capture = FunctionAnalysis::Capture;
    scopes.emplace_back();
    size_t word = 0;
    std::string self, vt;
    auto load = [&](const std::string &slot) {
      auto value = fresh();
      emit(value + " = load ptr, ptr " + slot);
      return value;
    };

    for (const auto &capture : info.captures) {
      const auto slot = captureSlot("%env", word++);

      if (capture.kind == Capture::Box) {
        auto payload = fresh();
        const auto box = load(slot);
        emit(payload + " = getelementptr i8, ptr " + box + ", i64 32");
        Binding binding{capture.type, payload};
        binding.box = slot;
        bind(capture.token, binding);
      } else if (capture.kind == Capture::Alias) {
        bind(capture.token, {capture.type, slot});
      } else if (capture.kind == Capture::This) {
        self = load(slot);
        vt = load(captureSlot("%env", word++));
      } else {
        Binding binding{capture.type, load(slot)};

        if (recordValue(capture.type)) binding.table = load(captureSlot("%env", word++));

        bind(capture.token, binding);
      }
    }

    if (info.bound) {
      self = "%env";
      vt = table({info.record, {}});
    }

    if (info.usesThis) {
      Type record = Type::Record;
      record.record = info.record;
      selfRecord = info.record;
      bindVisibleFields(record, self);
      Token token = f.name;
      token.text = "this";
      Binding binding{record, self};
      binding.table = vt;
      bind(token, binding);
    }

    scopes.emplace_back();
    std::ostringstream output;
    output << "define internal " << irType(f.result) << " " << symbol << "(ptr %env";

    for (size_t i = 0; i < f.params.size(); ++i) {
      const auto &parameter = f.params[i];
      const auto arg = "%arg" + std::to_string(i);
      output << ", " << (parameter.reference ? "ptr" : irType(parameter.type)) << " " << arg;

      if (parameter.reference) {
        Binding binding{parameter.type, arg};

        if (carriesTable(parameter)) {
          binding.table = "%vt" + std::to_string(i);
          output << ", ptr " << binding.table;
        }

        bind(parameter.name, binding);
      } else {
        retain({parameter.type, arg});
        declare(parameter.name, {parameter.type, arg});
      }
    }

    functionResult = f.result;

    if (!sequence(f.body)) {
      cleanupScopes(0);
      emit("ret void");
    }

    output << ") {\nentry:\n" << allocations.str() << code.str() << "}\n\n" << thunks.str();
    return output.str();
  }
  // A global function as a value: a constant closure around a thunk that
  // drops the environment parameter.
  Value functionClosure(const std::string &target, const Type &type) {
    usesFuncRuntime = true;
    const auto symbol = "@duck$fn." + target;

    if (!helpers.count(symbol)) {
      const auto &f = functions.at(target);
      std::string parameters, arguments;

      for (size_t i = 0; i < f.parameters.size(); ++i) {
        const auto &parameter = f.parameters[i];
        const auto arg = (parameter.reference ? std::string("ptr") : irType(parameter.type)) + " %a" + std::to_string(i);
        parameters += ", " + arg;
        arguments += (i ? ", " : "") + arg;

        if (carriesTable(parameter)) {
          parameters += ", ptr %t" + std::to_string(i);
          arguments += ", ptr %t" + std::to_string(i);
        }
      }

      const auto result = irType(f.result);
      std::string body = "define internal " + result + " " + symbol + "(ptr %env" + parameters + ") {\nentry:\n  ";

      if (f.result == Type::Void) body += "call void @duck." + target + "(" + arguments + ")\n  ret void\n}\n";
      else body += "%r = call " + result + " @duck." + target + "(" + arguments + ")\n  ret " + result + " %r\n}\n";

      body += "@duck$fc." + target + " = private unnamed_addr constant { i64, ptr, ptr } { i64 -1, ptr null, ptr " +
              symbol + " }\n";
      helpers[symbol] = body;
    }

    return {type, "@duck$fc." + target};
  }
  // A call through a func value. The closure is kept alive across the call,
  // since the callee may overwrite the variable it was read from.
  Value indirectCall(const Expr &e) {
    usesFuncRuntime = true;
    const auto &call = analysis.indirectCalls.at(&e);
    std::string closureValue, self;
    Value temporary{Type::Void, ""};

    if (e.kind == "invoke") {
      closureValue = expression(*e.receiver).value;
    } else {
      if (call.field) {
        const auto r = receiver(e.receiver.get(), e.token);
        temporary = r.temporary;
        Type owner = Type::Record;
        owner.record = call.owner;
        auto field = fresh();
        emit(field + " = getelementptr " + irType(owner) + ", ptr " + r.address + ", i32 0" + indexList(call.path));
        // A member lambda runs on the state that declares its field.
        self = r.address;

        if (call.path.size() > 1) {
          self = fresh();
          emit(self + " = getelementptr " + irType(owner) + ", ptr " + r.address + ", i32 0" +
               indexList({call.path.begin(), call.path.end() - 1}));
        }

        closureValue = fresh();
        emit(closureValue + " = load ptr, ptr " + field);
      } else {
        const auto binding = variable(e.token);
        closureValue = fresh();
        emit(closureValue + " = load ptr, ptr " + binding.value);
      }

      retain({call.type, closureValue});
    }

    auto present = fresh();
    emit(present + " = icmp ne ptr " + closureValue + ", null");
    require(present);
    auto functionSlot = fresh(), function = fresh();
    emit(functionSlot + " = getelementptr i8, ptr " + closureValue + ", i64 16");
    emit(function + " = load ptr, ptr " + functionSlot);
    auto env = closureValue;

    if (call.field) {
      auto count = fresh(), bound = fresh();
      env = fresh();
      emit(count + " = load i64, ptr " + closureValue);
      emit(bound + " = icmp eq i64 " + count + ", -2");
      emit(env + " = select i1 " + bound + ", ptr " + self + ", ptr " + closureValue);
    }

    std::string args = "ptr " + env;
    std::vector<Value> arguments;
    const auto &signature = *call.type.signature;

    for (size_t i = 0; i < e.args.size(); ++i) {
      Parameter parameter;
      parameter.type = signature.parameters[i].type;
      parameter.reference = signature.parameters[i].reference;
      appendArgument(*e.args[i], parameter, args, arguments);
    }

    const auto id = emitCall(signature.result, function, args);

    for (const auto &argument : arguments) release(argument);

    release({call.type, closureValue});

    if (!temporary.value.empty()) releaseBinding({temporary.type, temporary.value});

    return {signature.result, id};
  }
  // GetName, GetDeclaringName, and GetTags read the container's descriptor.
  Value reflect(const Expr &e) {
    const auto &info = analysis.reflections.at(&e);
    types.insert(info.record);
    const auto type = analysis.expressionTypes.at(&e);
    const auto symbol = "@duck$type." + info.record;
    const auto members = functions.members(info.record);
    const auto table = "[" + std::to_string(members.size()) + " x " + memberLayout + "]";

    if (info.query != "GetTags") {
      const bool bytes = type == Type::Name;
      auto slot = fresh(), value = fresh();

      if (info.member < 0)
        emit(slot + " = getelementptr " + std::string(typeLayout) + ", ptr " + symbol + ", i32 0, i32 " + (bytes ? "1" : "0"));
      else
        emit(slot + " = getelementptr " + table + ", ptr " + symbol + ".members, i32 0, i32 " +
             std::to_string(info.member) + ", i32 " +
             std::to_string(int(info.query == "GetName" ? MemberName : MemberDeclaring) + (bytes ? 1 : 0)));

      emit(value + " = load ptr, ptr " + slot);
      return {type, value};
    }

    usesCollectionRuntime = true;
    auto holder = fresh();
    allocations << "  " << holder << " = alloca ptr\n";
    emit("store ptr null, ptr " + holder);
    const auto &tags = members.at(size_t(info.member)).tags;
    const auto list = "[" + std::to_string(tags.size()) + " x " + tagLayout + "]";

    for (auto index : info.tags) {
      auto entry = fresh(), member = fresh(), slot = fresh();
      emit(entry + " = getelementptr " + list + ", ptr " + symbol + ".tags." + std::to_string(info.member) +
           ", i32 0, i32 " + std::to_string(index) + ", i32 1");
      emit(member + " = load i32, ptr " + entry);
      Type tagType = Type::Enum;
      tagType.record = tags[index].type;
      const auto value = enumValue(info.element) ? Value{info.element, member} : enumUnderlying({tagType, member});
      emit(slot + " = call ptr @duck.seq$push(ptr " + holder + ", i64 " + sizeOf(info.element) + ")");
      emit("store " + irType(info.element) + " " + value.value + ", ptr " + slot);
    }

    auto result = fresh();
    emit(result + " = load ptr, ptr " + holder);
    return {type, result};
  }
  void duckCallTarget(const FunctionAnalysis::DuckCall &info, const Receiver &r, bool isElement, std::string &descriptor, std::string &object) {
    if (isElement) {
      usesMarkupRuntime = true;
      descriptor = fresh();
      object = fresh();
      emit(descriptor + " = call ptr @duck.markup$descriptor(ptr " + r.address + ")");
      emit(object + " = call ptr @duck.markup$payload(ptr " + r.address + ")");
    } else if (r.rooted && r.view.empty()) {
      const auto &layout = tableLayout(r.root.type.record);
      auto ownSlot = fresh(), own = fresh(), typeSlot = fresh(), back = fresh();
      descriptor = fresh();
      object = fresh();
      emit(ownSlot + " = getelementptr " + layout + ", ptr " + r.root.table + ", i32 0, i32 0");
      emit(own + " = load i64, ptr " + ownSlot);
      emit(typeSlot + " = getelementptr " + layout + ", ptr " + r.root.table + ", i32 0, i32 2");
      emit(descriptor + " = load ptr, ptr " + typeSlot);
      emit(back + " = sub i64 0, " + own);
      emit(object + " = getelementptr i8, ptr " + r.address + ", i64 " + back);
    } else {
      types.insert(info.record.record);
      descriptor = "@duck$type." + info.record.record;
      object = r.address;
    }
  }
  std::string duckInvoke(const Expr &e, const FunctionAnalysis::DuckCall &info, const std::string &member, const std::string &object, const Type &resultType) {
    auto field = [&](MemberField index, const std::string &ir) {
      auto at = fresh(), value = fresh();
      emit(at + " = getelementptr " + std::string(memberLayout) + ", ptr " + member + ", i32 0, i32 " +
           std::to_string(int(index)));
      emit(value + " = load " + ir + ", ptr " + at);
      return value;
    };
    const auto function = field(MemberFunction, "ptr"), offset = field(MemberOffset, "i64"), table = field(MemberTable, "ptr");
    auto self = fresh();
    emit(self + " = getelementptr i8, ptr " + object + ", i64 " + offset);
    std::string args = "ptr " + self + ", ptr " + table;
    std::vector<Value> arguments;

    for (size_t i = 0; i < e.args.size(); ++i) appendArgument(*e.args[i], info.parameters[i], args, arguments);

    const auto id = emitCall(resultType, function, args);

    for (const auto &argument : arguments) release(argument);

    return id;
  }
  // `e..Foo(...)` finds the method on the receiver's runtime type. A required
  // call traps on a miss; `?e..Foo(...)` skips it, arguments included, and
  // reads as the default result; the checks give a bool and run nothing.
  Value duckCall(const Expr &e, bool guarded) {
    usesMetaRuntime = true;
    const auto &info = analysis.duckCalls.at(&e);
    const auto *base = e.receiver.get();
    const bool isElement = elementValue(analysis.expressionTypes.at(base));
    const bool pointer = !isElement && analysis.expressionTypes.at(base).depth != 0;
    const bool check = e.kind != "duckCall";
    const bool tolerant = guarded || check;
    Receiver r;

    if (isElement) r.address = load(location(*base).value);
    else if (pointer) r.address = (tolerant ? expression(*base) : dereference(*base)).value;
    else r = receiver(base, e.token);

    const Type resultType = check ? Type(Type::Bool) : info.result;
    const bool hasResult = resultType != Type::Void;
    std::string slot;

    if (tolerant && hasResult) {
      slot = fresh();
      allocations << "  " << slot << " = alloca " << irType(resultType) << '\n';
    }

    const auto miss = label(), done = label();

    if (tolerant && pointer) {
      auto alive = fresh();
      const auto present = label();
      emit(alive + " = icmp ne ptr " + r.address + ", null");
      emit("br i1 " + alive + ", label %" + present + ", label %" + miss);
      startBlock(present);
    }

    std::string descriptor, object;
    duckCallTarget(info, r, isElement, descriptor, object);
    const auto signature = e.kind == "duckExists" ? std::string("null") : textConstant(info.signature);
    auto member = fresh(), found = fresh();
    emit(member + " = call ptr @duck.meta$find(ptr " + descriptor + ", ptr " + textConstant(e.token.text) + ", ptr " +
         signature + ")");
    emit(found + " = icmp ne ptr " + member + ", null");

    if (check) {
      emit("store i1 " + found + ", ptr " + slot);
      emit("br label %" + done);
    } else {
      if (guarded) {
        const auto hit = label();
        emit("br i1 " + found + ", label %" + hit + ", label %" + miss);
        startBlock(hit);
      } else
        require(found);

      const auto id = duckInvoke(e, info, member, object, resultType);

      if (!tolerant) {
        releaseTemporary(r);
        return {resultType, id};
      }

      if (hasResult) emit("store " + irType(resultType) + " " + id + ", ptr " + slot);

      emit("br label %" + done);
    }

    startBlock(miss);

    if (hasResult)
      emit("store " + irType(resultType) + " " + (check ? std::string("0") : missingElement(resultType).value) + ", ptr " + slot);

    emit("br label %" + done);
    startBlock(done);
    releaseTemporary(r);

    if (!hasResult) return {Type::Void, ""};

    auto value = fresh();
    emit(value + " = load " + irType(resultType) + ", ptr " + slot);
    return {resultType, value};
  }
  // Store the magnitude as little-endian 32-bit limbs for @duck.wide.print.
  void printWide(Value v) {
    usesWideRuntime = true;
    const auto width = integerWidth(v.type);
    const auto type = irType(v.type);
    std::string magnitude = v.value, negative = "false";

    if (!unsignedInteger(v.type)) {
      negative = fresh();
      auto flipped = fresh();
      magnitude = fresh();
      emit(negative + " = icmp slt " + type + " " + v.value + ", 0");
      emit(flipped + " = sub " + type + " 0, " + v.value);
      // The minimum's negation wraps to itself, which is its unsigned magnitude.
      emit(magnitude + " = select i1 " + negative + ", " + type + " " + flipped + ", " +
           type + " " + v.value);
    }

    const auto rounded = (width + 31) / 32 * 32;
    const auto limbs = "i" + std::to_string(rounded);

    if (rounded != width) {
      auto extended = fresh();
      emit(extended + " = zext " + type + " " + magnitude + " to " + limbs);
      magnitude = extended;
    }

    auto buffer = fresh();
    allocations << "  " << buffer << " = alloca " << limbs << '\n';
    emit("store " + limbs + " " + magnitude + ", ptr " + buffer);
    emit("call void @duck.wide.print(ptr " + buffer + ", i64 " + std::to_string(rounded / 32) +
         ", i1 " + negative + ")");
  }
  // Field 0 (date) or 1 (time) of a datetime.
  std::string part(Value datetime, int field) {
    auto id = fresh();
    emit(id + " = extractvalue { i32, i64 } " + datetime.value + ", " + std::to_string(field));
    return id;
  }
  Value pack(const std::string &days, const std::string &nanos) {
    auto first = fresh(), second = fresh();
    emit(first + " = insertvalue { i32, i64 } poison, i32 " + days + ", 0");
    emit(second + " = insertvalue { i32, i64 } " + first + ", i64 " + nanos + ", 1");
    return {Type::DateTime, second};
  }
  // Every field is evaluated, left to right, before validation can trap.
  Value construct(const Expr &e) {
    usesCalendarRuntime = true;
    const auto type = e.castType;
    std::vector<std::string> fields;

    for (const auto &argument : e.args) fields.push_back(expression(*argument).value);
    if (type == Type::DateTime && fields.size() == 2) return pack(fields[0], fields[1]);

    auto id = fresh();

    if (type == Type::Char) {
      emit(id + " = call i32 @duck.char.make(i64 " + fields[0] + ")");
      return {type, id};
    }

    std::string days, nanos;
    size_t time = 0;

    if (type == Type::Date || type == Type::DateTime) {
      days = type == Type::Date ? id : fresh();
      emit(days + " = call i32 @duck.date.make(i64 " + fields[0] + ", i64 " + fields[1] +
           ", i64 " + fields[2] + ")");

      if (type == Type::Date) return {type, days};

      time = 3;
    }

    nanos = type == Type::Time ? id : fresh();
    emit(nanos + " = call i64 @duck.time.make(i64 " + fields[time] + ", i64 " +
         fields[time + 1] + ", i64 " + fields[time + 2] + ", i64 " +
         (fields.size() > time + 3 ? fields[time + 3] : "0") + ")");

    if (type == Type::Time) return {type, nanos};

    return pack(days, nanos);
  }
  Value calendarCall(const Expr &e) {
    usesCalendarRuntime = true;
    const auto &op = e.token.text;
    auto value = expression(*e.args[0]);
    auto days = [&] { return value.type == Type::DateTime ? part(value, 0) : value.value; };
    auto nanos = [&] { return value.type == Type::DateTime ? part(value, 1) : value.value; };
    auto id = fresh();

    if (op == "Year" || op == "Month" || op == "Day") {
      auto civil = fresh();
      emit(civil + " = call {i32, i32, i32} @duck.date.civil(i32 " + days() + ")");
      emit(id + " = extractvalue {i32, i32, i32} " + civil + ", " +
           (op == "Year" ? "0" : op == "Month" ? "1" : "2"));
      return {Type::Int, id};
    }

    if (op == "DayOfWeek") {
      emit(id + " = call i32 @duck.date.weekday(i32 " + days() + ")");
      return {Type::Int, id};
    }

    const std::map<std::string, std::pair<std::string, std::string>> timeParts{
        {"Hour", {"3600000000000", "24"}},
        {"Minute", {"60000000000", "60"}},
        {"Second", {"1000000000", "60"}},
        {"Nanosecond", {"1", "1000000000"}}};
    auto timePart = timeParts.find(op);

    if (timePart != timeParts.end()) {
      emit(id + " = call i32 @duck.time.part(i64 " + nanos() + ", i64 " +
           timePart->second.first + ", i64 " + timePart->second.second + ")");
      return {Type::Int, id};
    }

    if (op == "DateOf") return {Type::Date, days()};
    if (op == "TimeOf") return {Type::Time, nanos()};

    auto other = expression(*e.args[1]);

    if (op == "AddDays") {
      emit(id + " = call i32 @duck.date.add(i32 " + days() + ", i64 " + other.value + ")");

      if (value.type == Type::Date) return {Type::Date, id};

      auto moved = fresh();
      emit(moved + " = insertvalue { i32, i64 } " + value.value + ", i32 " + id + ", 0");
      return {Type::DateTime, moved};
    }

    // DaysBetween(a, b) = b - a, exact for every pair of valid dates.
    auto from = fresh(), to = fresh();
    emit(from + " = sext i32 " + value.value + " to i64");
    emit(to + " = sext i32 " + other.value + " to i64");
    emit(id + " = sub i64 " + to + ", " + from);
    return {Type::Long, id};
  }
  void join(const PendingTask &task) {
    auto slot = fresh(), handle = fresh();
    emit(slot + " = getelementptr " + task.layout + ", ptr " + task.record + ", i32 0, i32 0");
    emit(handle + " = load ptr, ptr " + slot);
    emit("call void @duck.task$join(ptr " + handle + ")");

    if (task.name.empty()) return;

    auto resultSlot = fresh(), result = fresh();
    emit(resultSlot + " = getelementptr " + task.layout + ", ptr " + task.record + ", i32 0, i32 " +
         std::to_string(task.result));
    emit(result + " = load " + irType(task.resultType) + ", ptr " + resultSlot);
    Value value{task.resultType, result};
    auto conversion = analysis.conversions.find(task.call);

    if (conversion != analysis.conversions.end()) value = convert(value, conversion->second);

    emit("store " + irType(value.type) + " " + value.value + ", ptr " + task.destination.value);
  }
  void joinAll() {
    for (const auto &task : pending.back()) join(task);

    pending.back().clear();
  }
  void joinBefore(const Stmt &s) {
    if (pending.back().empty()) return;
    if (analysis.taskBarriers.count(&s)) return joinAll();

    const auto &conflicts = analysis.taskConflicts.at(&s);
    std::vector<PendingTask> remaining;

    for (const auto &task : pending.back()) {
      if (conflicts.count(task.origin)) join(task);
      else remaining.push_back(task);
    }

    pending.back() = std::move(remaining);
  }
  // Arguments are evaluated here, in source order, before the task starts.
  // The record holds {handle, callee, arguments..., result}; a reference
  // argument stays valid because the task joins before its list ends.
  void spawn(const Stmt &s) {
    usesTaskRuntime = true;
    const auto &call = *s.expr;
    const auto &target = analysis.callTargets.at(&call);
    const auto &f = functions.at(target);
    std::string callee = "@duck." + target;
    Value temporaryReceiver{Type::Void, ""};
    std::vector<Value> values;
    const auto list = callArguments(call, target, f, callee, temporaryReceiver, values);
    std::vector<std::pair<std::string, std::string>> arguments;

    for (size_t begin = 0; begin < list.size();) {
      auto end = list.find(", ", begin);

      if (end == std::string::npos) end = list.size();

      const auto piece = list.substr(begin, end - begin);
      const auto space = piece.find(' ');
      arguments.emplace_back(piece.substr(0, space), piece.substr(space + 1));
      begin = end + 2;
    }

    const bool returns = f.result != Type::Void;
    std::string layout = "{ ptr, ptr";

    for (const auto &argument : arguments) layout += ", " + argument.first;
    if (returns) layout += ", " + irType(f.result);

    layout += " }";
    const auto result = arguments.size() + 2;
    auto record = fresh();
    allocations << "  " << record << " = alloca " << layout << '\n';
    auto store = [&](size_t index, const std::string &type, const std::string &value) {
      auto field = fresh();
      emit(field + " = getelementptr " + layout + ", ptr " + record + ", i32 0, i32 " + std::to_string(index));
      emit("store " + type + " " + value + ", ptr " + field);
    };
    store(1, "ptr", callee);

    for (size_t i = 0; i < arguments.size(); ++i) store(i + 2, arguments[i].first, arguments[i].second);

    auto entry = "@duck.task." + symbolPrefix + "." + std::to_string(tasks++);
    thunks << "define internal i32 " << entry << "(ptr %task) {\nentry:\n"
           << "  %c = getelementptr " << layout << ", ptr %task, i32 0, i32 1\n  %callee = load ptr, ptr %c\n";
    std::string args;

    for (size_t i = 0; i < arguments.size(); ++i) {
      const auto &type = arguments[i].first;
      const auto n = std::to_string(i + 2);
      thunks << "  %p" << n << " = getelementptr " << layout << ", ptr %task, i32 0, i32 " << n
             << "\n  %a" << n << " = load " << type << ", ptr %p" << n << '\n';
      args += (i ? ", " : "") + type + " %a" + n;
    }

    if (returns)
      thunks << "  %r = call " << irType(f.result) << " %callee(" << args << ")\n"
             << "  %p0 = getelementptr " << layout << ", ptr %task, i32 0, i32 " << result << '\n'
             << "  store " << irType(f.result) << " %r, ptr %p0\n";
    else
      thunks << "  call void %callee(" << args << ")\n";

    thunks << "  ret i32 0\n}\n\n";
    auto handle = fresh(), slot = fresh();
    emit(handle + " = call ptr @duck.task$spawn(ptr " + entry + ", ptr " + record + ")");
    emit(slot + " = getelementptr " + layout + ", ptr " + record + ", i32 0, i32 0");
    emit("store ptr " + handle + ", ptr " + slot);

    if (s.kind != "let") {
      pending.back().push_back({"", record, layout, result, f.result, &call, {}, &s});
      return;
    }

    auto name = s.token;
    name.text = s.name;
    declare(name, {analysis.localTypes.at(&s), ""}, s.constant);
    pending.back().push_back({s.name, record, layout, result, f.result, &call, variable(name), &s});
  }
  // Takes the block's locks before its first statement. They are held like a
  // local of the block, so every way out of it releases them.
  bool orderedBlock(const Stmt &s) {
    const auto found = analysis.orderedLocks.find(&s);
    const FunctionAnalysis::OrderedLocks locks = found != analysis.orderedLocks.end()
                                                     ? found->second
                                                     : FunctionAnalysis::OrderedLocks{true, {}};

    if (!locks.exclusive && locks.places.empty()) return block(s.body);

    usesLockRuntime = true;
    std::string mask = "0";

    for (const auto &place : locks.places) {
      std::string address;
      Type root;

      if (place.isStatic) {
        address = "@duck$static." + place.staticName;
        root = functions.statics.at(place.staticName).type;
      } else {
        const auto binding = variable(place.root);
        address = binding.value;
        root = binding.type;
      }

      if (!place.path.empty()) {
        auto field = fresh();
        emit(field + " = getelementptr " + irType(root) + ", ptr " + address + ", i32 0" + indexList(place.path));
        address = field;
      }

      auto bit = fresh(), combined = fresh();
      emit(bit + " = call i64 @duck.lock$bit(ptr " + address + ")");
      emit(combined + " = or i64 " + mask + ", " + bit);
      mask = combined;
    }

    auto token = fresh();
    emit(token + " = call i32 @duck.lock$enter(i64 " + mask + ", i1 " + (locks.exclusive ? "true" : "false") + ")");
    scopes.emplace_back();
    Binding held{Type::Int, token};
    held.owned = held.orderedLock = true;
    Token name = s.token;
    name.text = "$ordered";
    bind(name, held);
    const bool terminated = blockThenLeaveScope(s.body);
    return terminated;
  }
  bool sequence(const std::vector<std::unique_ptr<Stmt>> &body) {
    pending.emplace_back();
    bool terminated = false;

    for (const auto &s : body) {
      joinBefore(*s);

      if (analysis.taskSpawns.count(s.get())) spawn(*s);
      else terminated = statement(*s);
    }

    // Barriers precede every exit statement, so a terminated list has no tasks.
    if (!terminated) joinAll();

    pending.pop_back();
    return terminated;
  }
  bool block(const std::vector<std::unique_ptr<Stmt>> &body) {
    scopes.emplace_back();
    bool terminated = sequence(body);

    if (!terminated) cleanupScopes(scopes.size() - 1);

    scopes.pop_back();
    return terminated;
  }
  // The selector lives in a hidden local, so it is evaluated once. Labels are
  // compared in source order, and default runs only after all of them miss.
  bool switchStatement(const Stmt &s) {
    scopes.emplace_back();
    statement(*s.initializer);

    for (const auto &binding : s.alternate) statement(*binding);

    const auto end = label();
    std::vector<std::string> sections;
    std::string otherwise = end;

    for (const auto &section : s.body) {
      sections.push_back(label());

      if (section->isDefault) otherwise = sections.back();
    }

    for (size_t i = 0; i < s.body.size(); ++i)
      for (const auto &test : s.body[i]->labels) {
        const auto selected = expression(*s.target);
        const auto value = expression(*test);
        std::string matched;

        if (managedText(selected.type)) matched = binary("==", selected, value).value;
        else {
          matched = fresh();
          emit(matched + " = icmp eq " + irType(selected.type) + " " + selected.value + ", " + value.value);
        }

        const auto miss = label();
        emit("br i1 " + matched + ", label %" + sections[i] + ", label %" + miss);
        startBlock(miss);
      }

    emit("br label %" + otherwise);
    loops.push_back({end, "", scopes.size(), true});

    // Analysis has proved that every section ends control.
    for (size_t i = 0; i < s.body.size(); ++i) {
      startBlock(sections[i]);
      block(s.body[i]->body);
    }

    loops.pop_back();
    startBlock(end);

    if (analysis.terminatingLoops.count(&s)) {
      emit("unreachable");
      scopes.pop_back();
      return true;
    }

    cleanupScopes(scopes.size() - 1);
    scopes.pop_back();
    return false;
  }
  // Moves the element owned by a generated local into the markup at target.
  void markupAdd(const Stmt &s) {
    usesMarkupRuntime = usesMetaRuntime = true;
    const auto holder = location(*s.target).value;
    const auto record = analysis.expressionTypes.at(s.expr.get()).pointee();
    const auto word = moveOwner(*s.expr);
    types.insert(record.record);
    MemberPath children;
    std::string offset = "-1";

    if (functions.findField(record.record, "$children", children))
      offset = "ptrtoint (ptr getelementptr (" + irType(record) + ", ptr null, i32 0" + indexList(children.indices) + ") to i64)";

    emit("call void @duck.markup$attach(ptr " + holder + ", ptr " + textConstant(record.record) + ", ptr " + word + ", ptr " +
         elementCleanup(record) + ", ptr @duck$type." + record.record + ", i64 " + offset + ")");
  }
  void markupSplice(const Stmt &s) {
    usesMarkupRuntime = true;
    const auto holder = location(*s.target).value;
    const auto &content = *s.alternate.front()->expr;
    std::string source;

    if (content.addressable()) {
      const auto slot = location(content).value;
      source = load(slot);
      emit("store ptr null, ptr " + slot);
    } else source = expression(content).value;

    emit("call void @duck.markup$splice(ptr " + holder + ", ptr " + source + ")");
  }
  // Runs a body in the innermost scope, then cleans up and leaves that scope.
  bool blockThenLeaveScope(const std::vector<std::unique_ptr<Stmt>> &body) {
    const bool terminated = block(body);

    if (!terminated) cleanupScopes(scopes.size() - 1);

    scopes.pop_back();
    return terminated;
  }
  bool lowerScopedBody(const Stmt &s) {
    const auto scope = scopeDescriptor(s.name);
    emit("call void @duck.scope$enterLoop(ptr " + scope + ")");
    scopes.emplace_back();
    Binding iteration{Type::Pointer, scope};
    iteration.owned = iteration.scopeLoop = true;
    Token held = s.token;
    held.text = "$scope";
    bind(held, iteration);
    loops.back().scope = scope;
    const bool terminated = blockThenLeaveScope(s.body);
    return terminated;
  }
  bool lowerHandleReturn(const Stmt &s) {
    usesHeapRuntime = true;
    const auto uses = beginUses(runsCode(*s.expr));
    // The alias is taken before local cleanup, so a local owner's object
    // arrives already expired and a surviving owner's stays usable.
    std::string word = "null";

    if (s.expr->kind == "call") word = expression(*s.expr).value;
    else if (s.expr->kind != "null") {
      const auto alias = aliasWord(*s.expr);
      word = fresh();
      emit(word + " = call ptr @duck.heap$lend(ptr " + alias + ")");
    }

    endUses(uses);
    cleanupScopes(0);
    emit("ret ptr " + word);
    return true;
  }
  bool lowerIf(const Stmt &s) {
    const auto tested = analysis.expressionTypes.at(s.expr.get());
    // A bound condition holds a use of the object through its branch, so
    // the branch can keep using it after its owner lets it go.
    const bool bound = analysis.boundConditions.count(&s) &&
                       !(s.expr->kind == "name" && findBinding(s.expr->token).scopedBorrow);
    std::string truth, record, payload;

    if (bound) {
      record = useRecord();
      payload = pinCall(location(*s.expr), record);
      truth = fresh();
      emit(truth + " = icmp ne ptr " + payload + ", null");
    } else {
      const auto v = condition(*s.expr);
      truth = v.value;

      if (tested.depth) {
        truth = fresh();
        emit(truth + " = icmp ne ptr " + v.value + ", null");
      }
    }

    auto yes = label(), no = label(), end = label();
    emit("br i1 " + truth + ", label %" + yes + ", label %" + no);
    startBlock(yes);

    if (bound) {
      // Every exit from the branch cleans up this scope and ends the use.
      scopes.emplace_back();
      Binding use{Type::Pointer, record};
      use.owned = use.pin = true;
      Token held = s.token;
      held.text = "$use";
      bind(held, use);
      checkedPayloads[&s] = payload;
    }

    bool a = block(s.body);

    if (bound) {
      checkedPayloads.erase(&s);

      if (!a) cleanupScopes(scopes.size() - 1);

      scopes.pop_back();
    }

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
  bool lowerDo(const Stmt &s) {
    auto body = label(), check = label(), end = label();
    emit("br label %" + body);
    loops.push_back({end, check, scopes.size()});
    startBlock(body);

    if (!block(s.body))
      emit("br label %" + check);

    loops.pop_back();
    startBlock(check);

    if (s.expr) {
      auto tested = condition(*s.expr);
      emit("br i1 " + tested.value + ", label %" + body + ", label %" + end);
    } else
      emit("br label %" + body);

    startBlock(end);

    if (analysis.terminatingLoops.count(&s)) {
      emit("unreachable");
      return true;
    }

    return false;
  }
  bool lowerPreconditionLoop(const Stmt &s) {
    scopes.emplace_back();

    if (s.initializer)
      statement(*s.initializer);

    auto check = label(), body = label(), end = label(), step = label();
    emit("br label %" + check);
    startBlock(check);
    auto v = s.expr ? condition(*s.expr) : Value{Type::Bool, "1"};
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
  bool statement(const Stmt &s) {
    if (s.kind == "scopeStart") {
      emit("call void @duck.scope$start(ptr " + scopeDescriptor(s.name) + ")");
      return false;
    }

    if (s.kind == "scopeEnd") {
      const auto scope = scopeDescriptor(s.name);
      emit("call void @duck.scope$end(ptr " + scope + ")");
      emit("call void @duck.scope$requestUnwind(ptr " + scope + ")");
      unwindCheck();
      return false;
    }

    if (s.kind == "scopedBody") return lowerScopedBody(s);

    if (s.kind == "markupAdd") {
      markupAdd(s);
      return false;
    }

    if (s.kind == "markupSplice") {
      if (!analysis.markupSplices.count(&s)) return statement(*s.alternate.front());

      markupSplice(s);
      return false;
    }

    if (s.kind == "block")
      return block(s.body);

    if (s.kind == "ordered") return orderedBlock(s);
    if (s.kind == "switch") return switchStatement(s);

    if (s.kind == "break") {
      cleanupScopes(loops.back().scopeDepth);
      emit("br label %" + loops.back().exit);
      return true;
    }

    if (s.kind == "continue") {
      auto loop = loops.rbegin();

      while (loop->isSwitch) ++loop;

      cleanupScopes(loop->scopeDepth);
      emit("br label %" + loop->next);
      return true;
    }

    if (s.kind == "returnOwner") {
      auto word = moveOwner(*s.expr);
      cleanupScopes(0);
      emit("ret ptr " + word);
      return true;
    }

    if (analysis.handleReturns.count(&s)) return lowerHandleReturn(s);

    if (s.kind == "return") {
      const auto uses = beginUses(s.expr && runsCode(*s.expr));
      Value v = s.expr ? expression(*s.expr) : Value{Type::Void, ""};
      endUses(uses);
      cleanupScopes(0, analysis.moves.count(&s) ? variable(s.expr->token).value : "");
      emit("ret " + irType(v.type) +
           (v.type == Type::Void ? "" : " " + v.value));
      return true;
    }

    if (s.kind == "if") return lowerIf(s);
    if (s.kind == "do") return lowerDo(s);

    if (auto loop = analysis.enumLoops.find(&s); loop != analysis.enumLoops.end()) {
      enumFor(s, loop->second);
      return false;
    }

    if (s.kind == "foreach") {
      forEach(s);
      return false;
    }

    if (s.kind == "rangefor") {
      rangeFor(s);
      return false;
    }

    if (s.kind == "while" || s.kind == "for") return lowerPreconditionLoop(s);

    const auto uses = beginUses(statementRuns(s));
    const bool terminated = simpleStatement(s);
    endUses(uses);
    return terminated;
  }
  // Branches to skip unless ok holds, and continues in a new block.
  void continueIf(const std::string &ok, const std::string &skip) {
    auto proceed = label();
    emit("br i1 " + ok + ", label %" + proceed + ", label %" + skip);
    startBlock(proceed);
  }
  // Checks pointer e, once: it is valid from here on, and lowering the call
  // reuses the payload found here. A tracked object stays in use until the
  // statement ends, so a call that releases it still finishes first.
  void guardLink(const Expr &e, const std::string &skip, std::vector<const Expr *> &links) {
    const auto type = analysis.expressionTypes.at(&e);

    if (!type.depth) return;

    std::string payload;

    if (auto use = analysis.checkedUses.find(&e); use != analysis.checkedUses.end() && checkedPayloads.count(use->second))
      payload = checkedPayloads.at(use->second);
    else if (type.role != Type::Raw && pinnable(e, type)) {
      const auto slot = location(e);
      const auto record = useRecord();
      activeUses.push_back(record);
      payload = pinCall(slot, record);
    } else
      payload = expression(e).value;

    auto valid = fresh();
    emit(valid + " = icmp ne ptr " + payload + ", null");
    continueIf(valid, skip);
    guardedLinks[&e] = payload;
    links.push_back(&e);
  }
  // Checks each link of a receiver chain from its root outward.
  void guardChain(const Expr &e, const std::string &skip, std::vector<const Expr *> &links) {
    if (!analysis.expressionTypes.count(&e)) return; // A static path, not a value.
    if (analysis.staticFields.count(&e)) return guardLink(e, skip, links);

    if (e.kind == "." || (e.kind == "unary" && e.token.text == "*")) guardChain(*e.args[0], skip, links);
    else if (e.kind == "call" && e.receiver) guardChain(*e.receiver, skip, links);
    else if (e.kind == "index") {
      // The guarded slot lookup walks any inner indexes itself.
      const Expr *root = &e;

      while (root->kind == "index") root = root->args[0].get();

      guardChain(*root, skip, links);
      const auto slot = elementSlot(e, true);
      auto found = fresh();
      emit(found + " = icmp ne ptr " + slot + ", null");
      continueIf(found, skip);
      guardedSlots[&e] = slot;
      links.push_back(&e);
    }

    guardLink(e, skip, links);
  }
  // ?p.q.Foo(x): the arguments and the call run only if every link is valid;
  // otherwise the result is its type's default.
  Value guardedCall(const Expr &e) {
    const auto &call = *e.args[0];
    const auto type = analysis.expressionTypes.at(&e);
    const bool produces = type != Type::Void;
    auto skip = label(), done = label();
    std::string result;

    if (produces) {
      result = fresh();
      allocations << "  " << result << " = alloca " << irType(type) << '\n';
    }

    std::vector<const Expr *> links;
    guardChain(*call.receiver, skip, links);

    if (auto indirect = analysis.indirectCalls.find(&call);
        indirect != analysis.indirectCalls.end() && call.receiver->addressable() &&
        !analysis.properties.count(call.receiver.get())) {
      // A func field can be empty.
      const auto r = receiver(call.receiver.get(), call.token);
      Type owner = Type::Record;
      owner.record = indirect->second.owner;
      auto field = fresh(), closure = fresh(), present = fresh();
      emit(field + " = getelementptr " + irType(owner) + ", ptr " + r.address + ", i32 0" + indexList(indirect->second.path));
      emit(closure + " = load ptr, ptr " + field);
      emit(present + " = icmp ne ptr " + closure + ", null");
      continueIf(present, skip);
    }

    const auto value = expression(call);

    if (produces) emit("store " + irType(type) + " " + value.value + ", ptr " + result);

    emit("br label %" + done);
    startBlock(skip);

    if (produces) emit("store " + irType(type) + " " + missingElement(type).value + ", ptr " + result);

    emit("br label %" + done);
    startBlock(done);

    for (const auto *link : links) {
      guardedLinks.erase(link);
      guardedSlots.erase(link);
    }

    if (!produces) return {Type::Void, ""};

    auto loaded = fresh();
    emit(loaded + " = load " + irType(type) + ", ptr " + result);
    return {type, loaded};
  }
  void declareTrackedLocal(const Stmt &s, const Type &type) {
    usesHeapRuntime = true;
    std::string word;
    bool scoped = analysis.scopedBorrows.count(&s) != 0;

    if (scoped) word = expression(*s.expr).value;
    else if (type.role == Type::Alias) word = aliasWord(*s.expr);
    else if (s.expr && analysis.expressionTypes.at(s.expr.get()).role == Type::Owner) {
      word = s.expr->kind == "call" ? expression(*s.expr).value : moveOwner(*s.expr);

      // `::` needs ownership; a borrowed result has none to give.
      if (s.kind == "own" && s.expr->kind == "call" && mayBeBorrowed(*s.expr))
        emit("call void @duck.heap$owned(ptr " + word + ")");
    } else
      word = allocate(type.pointee(), s.expr ? expression(*s.expr).value : defaultValue(type.pointee()));

    auto name = s.token;
    name.text = s.name;
    declare(name, {type, word}, s.constant, scoped);

    if (s.kind == "let" && s.expr && s.expr->kind == "call" && mayBeBorrowed(*s.expr))
      scopes.back()[name.text].lent = true;
  }
  void assignProperty(const Stmt &s, const FunctionAnalysis::PropertyAccess &access) {
    const auto &target = *s.target;
    const auto r = receiver(target.kind == "." ? target.args[0].get() : nullptr, target.token);
    Value v;

    if (s.kind == "assign") v = expression(*s.expr);
    else {
      const auto old = accessorCall(r, access.owner, access.getter, access.getterPath);
      const auto type = analysis.expressionTypes.at(&target);
      auto operand = s.expr ? expression(*s.expr) : convert({Type::Int, "1"}, type);
      v = binary(s.kind.substr(0, s.kind.size() - 1), old, operand);
    }

    accessorCall(r, access.owner, access.setter, access.setterPath, &v);
    release(v);
  }
  void lowerAssignment(const Stmt &s) {
    if (const auto type = analysis.expressionTypes.at(s.target.get());
        (dynamicArray(type) || collectionValue(type)) && s.kind != "assign") {
      mutate(*s.target, s.kind, s.expr.get(), false);
      return;
    }

    if (auto property = analysis.properties.find(s.target.get()); property != analysis.properties.end()) {
        assignProperty(s, property->second);
        return;
      }

    // `?a[i] op= v` finds the element first and skips the whole
    // assignment, including evaluating v, when it is missing.
    std::string skip;
    const auto &target = s.target->kind == "guard" ? *s.target->args[0] : *s.target;

    if (&target != s.target.get()) {
      const auto slot = elementSlot(target, true);
      auto found = fresh(), hit = label();
      skip = label();
      emit(found + " = icmp ne ptr " + slot + ", null");
      emit("br i1 " + found + ", label %" + hit + ", label %" + skip);
      startBlock(hit);
      guardedSlots[&target] = slot;
    }

    auto address = location(target);
    address.box.clear(); // Assignment replaces the payload, not the shared box.

    if (address.type.role != Type::Raw) {
      auto word = aliasWord(*s.expr); // Retain first, including on self-assignment.
      releaseBinding(address);
      emit("store ptr " + word + ", ptr " + address.value);
      return;
    }

    Value v;

    if (s.kind == "assign") {
      v = expression(*s.expr);
      requireWhole(address);
    }
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

    releaseBinding(address, !initializer);
    emit("store " + irType(v.type) + " " + v.value + ", ptr " +
         address.value);

    if (!skip.empty()) {
      guardedSlots.erase(&target);
      emit("br label %" + skip);
      startBlock(skip);
    }
  }
  // A statement that evaluates expressions and has no branches of its own.
  bool simpleStatement(const Stmt &s) {
    if (s.kind == "moveInto") {
      moveIntoField(*s.target, *s.expr, false);
      return false;
    }

    if (s.kind == "own" || s.kind == "let") {
      const auto type = analysis.localTypes.at(&s);

      if (type.role != Type::Raw) {
        declareTrackedLocal(s, type);
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
      lowerAssignment(s);
    } else
      release(expression(*s.expr));

    return false;
  }

public:
  Generator(const Symbols &symbols, const FunctionAnalysis &analysis,
            const std::string &name)
      : functions(symbols), analysis(analysis), symbolPrefix(name) {}
  FunctionIR generate(const Function &f) {
    initializer = f.generated;

    if (f.method) {
      selfRecord = f.params.front().type.record;
      dispatching = !f.generated && dispatched(f.name.text, true);
    }

    scopes.emplace_back();

    if (f.method && !f.generated) {
      const auto &self = f.params.front().type;
      bindVisibleFields(self, "%arg0");
      scopes.emplace_back();
    }

    std::ostringstream output;
    // Script numbers restart in each module, so their symbols stay private.
    output << "define " << (f.script ? "internal " : "") << irType(f.result) << " @duck." << f.name.text << "(";

    for (size_t i = 0; i < f.params.size(); ++i) {
      if (i)
        output << ", ";

      const auto &parameter = f.params[i];
      auto arg = "%arg" + std::to_string(i);
      output << (parameter.reference ? "ptr" : irType(parameter.type)) << " "
             << arg;
      std::string vt;

      if (i == 0 && f.method) {
        if (dispatching) output << ", ptr %vt";

        vt = selfTable();
      } else if (carriesTable(parameter)) {
        vt = "%vt" + std::to_string(i);
        output << ", ptr " << vt;
      }

      if (parameter.reference) {
        Binding binding{parameter.type, arg};
        binding.table = vt;
        bind(parameter.name, binding);
      }
      else {
        retain({parameter.type, arg});
        declare(parameter.name, {parameter.type, arg});
      }
    }

    functionResult = f.result;
    bool terminated = sequence(f.body);

    if (!terminated) {
      cleanupScopes(0);
      emit("ret void");
    }

    output << ") {\nentry:\n" << allocations.str() << code.str() << "}\n\n" << thunks.str();
    FunctionIR result{globals.str(), output.str(), usesTextRuntime, usesHeapRuntime, usesTaskRuntime, usesLockRuntime,
            usesCalendarRuntime, usesWideRuntime, usesBigRuntime, usesDecimalRuntime, usesCollectionRuntime,
            usesFuncRuntime, usesMetaRuntime, usesMarkupRuntime, usesScopeRuntime, cleanups, destroys, tables, helpers, types, scopeNames, mathImports};
    result.usesFileRuntime = usesFileRuntime;
    return result;
  }
};
} // namespace
FunctionIR lowerFunction(const Function &function, const Symbols &symbols,
                         const FunctionAnalysis &analysis) {
  if (!function.nativeSymbol.empty()) return {};

  return Generator(symbols, analysis, function.name.text).generate(function);
}
// The definitions of the native vector and matrix types a module mentions.
// Matrices go first because defining one mentions its column vector.
std::string emitNativeMathTypes(const Symbols &symbols, const std::string &module) {
  std::string ir;

  for (const bool matrices : {true, false})
    for (const auto &[name, record] : symbols.records) {
      const auto shape = nativeMath(name);
      const auto symbol = "%struct." + name;

      if (!shape || shape.matrix() != matrices) continue;
      if (module.find(symbol) == std::string::npos && ir.find(symbol) == std::string::npos) continue;

      ir += symbol + " = type { ";

      for (size_t i = 0; i < record.fields.size(); ++i) ir += (i ? ", " : "") + irType(record.fields[i].type);

      ir += " }\n";

      if (module.find("@duck$default." + name) != std::string::npos)
        ir += "@duck$default." + name + " = private unnamed_addr constant " + symbol + " " + recordConstant(symbols, name) + "\n";
    }

  return ir;
}
std::string emitRecords(const Symbols &symbols) {
  std::string ir;

  for (const auto &[name, record] : symbols.records) {
    if (nativeMath(name)) continue; // emitNativeMathTypes adds the ones in use.

    ir += "%struct." + name + " = type { ";

    if (record.fields.empty()) ir += "i8"; // Give empty instances an addressable byte.

    for (size_t i = 0; i < record.fields.size(); ++i) {
      if (i) ir += ", ";

      ir += irType(record.fields[i].type);
    }

    ir += " }\n";
  }

  // Each module keeps private copies of the defaults it may construct.
  std::set<std::string> zeros;

  for (const auto &[name, record] : symbols.records)
    for (const auto &field : record.fields) {
      if (managedValue(field.type) && (!field.initializer || field.runtimeDefault))
        zeros.insert(managedZero(field.type));

      if (!field.scope.empty() && managedValue(field.scopedType)) zeros.insert(managedZero(field.scopedType));
    }

  for (const auto &[name, info] : symbols.statics) {
    if (managedValue(info.type) && (!info.initializer || info.runtime) && !info.imported)
      zeros.insert(managedZero(info.type));

    if (!info.scope.empty() && managedValue(info.scopedType)) zeros.insert(managedZero(info.scopedType));
  }

  for (const auto &zero : zeros) {
    if (zero == "@duck$big.zero")
      ir += zero + " = private unnamed_addr constant { i64, i64, i64, [0 x i32] } "
                   "{ i64 -1, i64 0, i64 0, [0 x i32] zeroinitializer }, align 8\n";
    else
      ir += managedTextGlobal(zero, Token{}, zero == "@duck$empty.string" ? Type::String : Type::Name);
  }

  for (const auto &[name, record] : symbols.records) {
    for (const auto &field : record.fields)
      if (field.initializer && !field.runtimeDefault && field.initialValue.empty())
        ir += managedTextGlobal(defaultTextSymbol(name, field), field.initializer->token, field.type);

    if (hasDefaults(symbols, name) && !nativeMath(name))
      ir += "@duck$default." + name + " = private unnamed_addr constant %struct." + name + " " +
            recordConstant(symbols, name) + "\n";
  }

  return ir;
}
// Per-module private tables: member names for Print, underlying values for
// decay. Text values are immortal managed literals, like string constants.
std::string emitEnums(const Symbols &symbols) {
  std::ostringstream ir;

  for (const auto &[name, info] : symbols.enums) {
    const auto count = std::to_string(info.members.size());
    const auto type = irType(info.type);

    for (size_t i = 0; i < info.members.size(); ++i) {
      const auto &label = info.members[i].text;
      ir << "@enum." << name << ".name." << i << " = private unnamed_addr constant ["
         << label.size() + 1 << " x i8] c\"" << label << "\\00\"\n";
      ir << managedTextGlobal(enumLabelSymbol(name, i), info.members[i], Type::Name);

      if (!managedText(info.type)) continue;

      Token text = info.members[i];
      text.text = info.values[i];
      ir << managedTextGlobal(enumTextSymbol(name, i), text, info.type);
    }

    ir << "@enum." << name << ".names = private unnamed_addr constant [" << count << " x ptr] [";

    for (size_t i = 0; i < info.members.size(); ++i)
      ir << (i ? ", " : "") << "ptr @enum." << name << ".name." << i;

    ir << "]\n@enum." << name << ".labels = private unnamed_addr constant [" << count << " x ptr] [";

    for (size_t i = 0; i < info.members.size(); ++i)
      ir << (i ? ", " : "") << "ptr " << enumLabelSymbol(name, i);

    ir << "]\n@enum." << name << ".values = private unnamed_addr constant [" << count << " x "
       << type << "] [";

    for (size_t i = 0; i < info.members.size(); ++i)
      ir << (i ? ", " : "") << type << " "
         << (managedText(info.type) ? enumTextSymbol(name, i) : info.values[i]);

    ir << "]\n";
  }

  return ir.str();
}
namespace {
// Releases each managed value inside `value`, a loaded record of `record`.
void releaseFields(const Symbols &symbols, const std::string &record, const std::string &value,
                   int &next, std::string &ir) {
  Type type = Type::Record;
  type.record = record;
  const auto &fields = symbols.records.at(record).fields;

  for (size_t i = 0; i < fields.size(); ++i) {
    if (!symbols.managed(fields[i].type)) continue;

    const auto part = "%v" + std::to_string(next++);
    ir += "  " + part + " = extractvalue " + irType(type) + " " + value + ", " + std::to_string(i) + "\n";

    if (recordValue(fields[i].type)) releaseFields(symbols, fields[i].type.record, part, next, ir);
    else if (funcValue(fields[i].type)) ir += "  call void @duck.cell$release(ptr " + part + ")\n";
    else if (trackedAlias(fields[i].type)) ir += "  call void @duck.heap$release(ptr " + part + ")\n";
    else ir += "  call void @duck.text.release(ptr " + part + ")\n";
  }
}
} // namespace
std::string emitCleanups(const Symbols &symbols, const std::set<std::string> &containers) {
  std::string ir;

  for (const auto &record : containers) {
    ir += "define internal void @duck$cleanup." + record + "(ptr %payload) {\nentry:\n";

    if (symbols.destructible(record)) ir += "  call void @duck$destroy." + record + "(ptr %payload)\n";

    if (!symbols.managedRecord(record)) {
      ir += "  ret void\n}\n";
      continue;
    }

    ir += "  %value = load %struct." + record + ", ptr %payload\n";
    int next = 0;
    releaseFields(symbols, record, "%value", next, ir);
    ir += "  ret void\n}\n";
  }

  return ir;
}
std::string emitDestroys(const Symbols &symbols, std::set<std::string> containers) {
  std::string ir;
  std::set<std::string> emitted;

  while (!containers.empty()) {
    const auto record = *containers.begin();
    containers.erase(containers.begin());

    if (!emitted.insert(record).second) continue;

    ir += "define internal void @duck$destroy." + record + "(ptr %self) {\nentry:\n";

    if (symbols.count(destructorKey(record)))
      ir += "  call void @duck." + destructorKey(record) + "(ptr %self)\n";

    const auto &fields = symbols.records.at(record).fields;

    for (size_t i = fields.size(); i-- > 0;) {
      const auto &field = fields[i];

      if (field.type.role == Type::Owner) {
        const auto payload = field.type.pointee();
        const std::string cleanup = symbols.needsCleanup(payload) ? "@duck$cleanup." + payload.record : "null";
        const auto part = "%field" + std::to_string(i);
        ir += "  " + part + " = getelementptr %struct." + record + ", ptr %self, i32 0, i32 " + std::to_string(i) +
              "\n  call void @duck.heap$drop(ptr " + part + ", i1 true, ptr " + cleanup + ")\n";
        continue;
      }

      if (dynamicArray(field.type)) {
        const auto part = "%field" + std::to_string(i), held = "%held" + std::to_string(i);
        const auto &element = *field.type.element;
        const std::string cleanup = managedValue(element) ? "@duck$slot.release"
                                    : funcValue(element) ? "@duck$slot.func"
                                    : trackedAlias(element) ? "@duck.heap$slot"
                                    : recordValue(element) && symbols.needsCleanup(element)
                                        ? "@duck$cleanup." + element.record
                                        : "null";
        ir += "  " + part + " = getelementptr %struct." + record + ", ptr %self, i32 0, i32 " +
              std::to_string(i) + "\n  " + held + " = load ptr, ptr " + part + "\n  call void @duck.seq$free(ptr " +
              held + ", ptr " + cleanup + ")\n  store ptr null, ptr " + part + "\n";
        continue;
      }

      if (fixedArray(field.type) && symbols.needsCleanup(field.type)) {
        // Only func and alias elements reach here: validateLifetimes rejects the others.
        const auto part = "%field" + std::to_string(i);
        unsigned long long count = 1;
        Type leaf = field.type;

        for (; fixedArray(leaf); leaf = *leaf.element) count *= leaf.extent;

        ir += "  " + part + " = getelementptr %struct." + record + ", ptr %self, i32 0, i32 " + std::to_string(i) +
              "\n  call void @duck.array$each(ptr " + part + ", i64 " + std::to_string(count) +
              ", i64 ptrtoint (ptr getelementptr (ptr, ptr null, i32 1) to i64), ptr " +
              (trackedAlias(leaf) ? "@duck.heap$slot" : "@duck$slot.func") + ")\n";
        continue;
      }

      if (fileValue(field.type)) {
        const auto part = "%field" + std::to_string(i);
        ir += "  " + part + " = getelementptr %struct." + record + ", ptr %self, i32 0, i32 " + std::to_string(i) +
              "\n  call void @duck.file$drop(ptr " + part + ")\n";
        continue;
      }

      if (collectionValue(field.type)) {
        const auto part = "%field" + std::to_string(i), held = "%held" + std::to_string(i);
        auto release = [](const Type &t) {
          return elementValue(t) ? "@duck.markup$node" : managedValue(t) ? "@duck$slot.release"
                 : trackedAlias(t) ? "@duck.heap$slot" : "null";
        };
        ir += "  " + part + " = getelementptr %struct." + record + ", ptr %self, i32 0, i32 " +
              std::to_string(i) + "\n  " + held + " = load ptr, ptr " + part + "\n";

        if (field.type.base == Type::Pair)
          ir += "  call void @duck.pair$free(ptr " + held + ", ptr " + release(*field.type.key) + ", ptr " +
                release(*field.type.element) + ")\n";
        else
          ir += "  call void @duck.seq$free(ptr " + held + ", ptr " + release(*field.type.element) + ")\n";

        ir += "  store ptr null, ptr " + part + "\n";
        continue;
      }

      if (!recordValue(field.type) || !symbols.destructible(field.type.record)) continue;

      const auto part = "%field" + std::to_string(i);
      ir += "  " + part + " = getelementptr %struct." + record + ", ptr %self, i32 0, i32 " +
            std::to_string(i) + "\n";
      ir += "  call void @duck$destroy." + field.type.record + "(ptr " + part + ")\n";
      containers.insert(field.type.record);
    }

    ir += "  ret void\n}\n";
  }

  return ir;
}
namespace {
std::string cstringGlobal(const std::string &symbol, const std::string &text) {
  const char *hex = "0123456789ABCDEF";
  std::string escaped;

  for (unsigned char c : text) {
    escaped += '\\';
    escaped += hex[c >> 4];
    escaped += hex[c & 15];
  }

  return symbol + " = private unnamed_addr constant [" + std::to_string(text.size() + 1) + " x i8] c\"" + escaped +
         "\\00\"\n";
}
// A container's descriptor: its names, and for each member its names,
// signature, declaring container, attributes, and, for a method, the final
// overrider with its receiver offset and table. Adds those tables to `tables`.
std::string typeDescriptor(const Symbols &symbols, const std::string &record, std::set<DispatchTable> &tables) {
  const auto symbol = "@duck$type." + record;
  const auto members = symbols.members(record);
  std::string ir;
  auto text = [&](const std::string &name, const std::string &value, Type type) {
    Token token;
    token.text = value;
    ir += managedTextGlobal(name, token, type);
    return name;
  };
  const auto typeName_ = text(symbol + ".name", record, Type::String);
  const auto typeBytes = text(symbol + ".bytes", record, Type::Name);
  std::string entries;

  for (size_t i = 0; i < members.size(); ++i) {
    const auto &member = members[i];
    const auto prefix = symbol + ".m" + std::to_string(i);
    const auto name = text(prefix + ".name", member.name, Type::String);
    const auto bytes = text(prefix + ".bytes", member.name, Type::Name);
    const auto declaring = text(prefix + ".from", member.declaring, Type::String);
    const auto declaringBytes = text(prefix + ".frombytes", member.declaring, Type::Name);
    const auto signature = prefix + ".signature";
    ir += cstringGlobal(signature, member.signature ? signatureText(member.signature->result, member.signature->parameters, 1)
                                                    : typeName(member.type));
    std::string tags = "null";

    // Every member with attributes has a list, named by its member index.
    if (!member.tags.empty()) {
      tags = symbol + ".tags." + std::to_string(i);
      std::string list;

      for (size_t t = 0; t < member.tags.size(); ++t) {
        const auto owner = prefix + ".tag" + std::to_string(t);
        ir += cstringGlobal(owner, member.tags[t].type);
        list += std::string(t ? ", " : "") + tagLayout + " { ptr " + owner + ", i32 " + std::to_string(member.tags[t].member) + " }";
      }

      ir += tags + " = private unnamed_addr constant [" + std::to_string(member.tags.size()) + " x " + tagLayout + "] [" + list + "]\n";
    }

    std::string function = "null", table = "null";

    if (member.signature) {
      function = "@duck." + member.key;
      table = tableSymbol({record, member.path});
      tables.insert({record, member.path});
    }

    entries += std::string(i ? ", " : "") + memberLayout + " { ptr " + name + ", ptr " + bytes + ", ptr " + signature +
               ", ptr " + declaring + ", ptr " + declaringBytes + ", i32 " +
               std::to_string((member.signature ? 1 : 0) | (member.isProtected ? 2 : 0)) + ", i32 " +
               std::to_string(member.tags.size()) + ", ptr " + tags + ", ptr " + function + ", i64 " +
               pathOffset(record, member.path) + ", ptr " + table + " }";
  }

  const auto count = std::to_string(members.size());
  ir += symbol + ".members = private unnamed_addr constant [" + count + " x " + memberLayout + "] ";
  ir += members.empty() ? std::string("zeroinitializer\n") : "[" + entries + "]\n";
  ir += symbol + " = internal constant " + typeLayout + " { ptr " + typeName_ + ", ptr " + typeBytes + ", i64 " + count +
        ", ptr " + symbol + ".members }\n";
  return ir;
}
} // namespace
std::string emitTables(const Symbols &symbols, std::set<DispatchTable> tables, std::set<std::string> types) {
  std::string ir;
  std::set<DispatchTable> emitted;
  std::set<std::string> described;

  while (!tables.empty() || !types.empty()) {
    if (!types.empty()) {
      const auto record = *types.begin();
      types.erase(types.begin());

      if (described.insert(record).second) ir += typeDescriptor(symbols, record, tables);

      continue;
    }

    const auto table = *tables.begin();
    tables.erase(tables.begin());

    if (!emitted.insert(table).second) continue;

    const auto &[record, path] = table;
    const auto seen = symbols.recordAt(record, path);
    ir += tableSymbol(table) + " = internal constant " + tableType(symbols, seen) + " { i64 " +
          pathOffset(record, path) + ", i64 " + std::to_string(path.size()) + ", ptr @duck$type." + record;
    types.insert(record);

    for (size_t i = 0; i < symbols.records.at(seen).parents.size(); ++i) {
      auto parent = path;
      parent.push_back(i);
      ir += ", ptr " + tableSymbol({record, parent});
      tables.insert({record, parent});
    }

    for (const auto &name : symbols.dispatchNames(seen)) {
      const auto [key, receiver] = symbols.overrider(record, path, name);
      ir += ", ptr @duck." + key + ", i64 " + pathOffset(record, receiver) + ", ptr " +
            tableSymbol({record, receiver});
      tables.insert({record, receiver});
    }

    ir += " }\n";
  }

  return ir;
}
std::string emitStatics(const Symbols &symbols) {
  std::string ir;

  for (const auto &[name, info] : symbols.statics) {
    ir += "@duck$static." + name + " = ";

    if (info.imported) {
      ir += "external global " + irType(info.type) + "\n";
      continue;
    }

    auto value = info.value;
    std::string literal;

    if (recordValue(info.type)) value = recordConstant(symbols, info.type.record);
    else if (managedValue(info.type) && (!info.initializer || info.runtime)) value = managedZero(info.type);
    else if (info.runtime) value = "zeroinitializer";
    else if (info.initializer && value.empty()) {
      value = "@duck$static." + name + "$text";
      literal = managedTextGlobal(value, info.initializer->token, info.type);
    }

    // A constant evaluated at run time is written once, before Main.
    ir += std::string(info.constant && !info.runtime ? "constant " : "global ") + irType(info.type) + " " +
          value + "\n" + literal;
  }

  // Initialization order: every imported static, then this module's statics
  // in declaration order. Each static's $init runs at most once and first
  // runs its predecessor, the first one running the imported $inits, so the
  // order holds whichever module's startup the linker runs first.
  std::vector<std::pair<size_t, std::string>> own;
  std::vector<std::string> imported;

  for (const auto &[name, info] : symbols.statics)
    if (info.imported) imported.push_back(name);
    else own.emplace_back(info.order, name);

  std::sort(own.begin(), own.end());

  for (const auto &name : imported) ir += "declare void @duck$static." + name + "$init()\n";

  for (size_t i = 0; i < own.size(); ++i) {
    const auto &name = own[i].second;
    const auto done = "@duck$static." + name + "$done";
    ir += done + " = internal global i1 false\n";
    ir += "define void @duck$static." + name + "$init() {\nentry:\n  %done = load i1, ptr " + done +
          "\n  br i1 %done, label %exit, label %run\nrun:\n  store i1 true, ptr " + done + "\n";

    if (i) ir += "  call void @duck$static." + own[i - 1].second + "$init()\n";
    else for (const auto &other : imported) ir += "  call void @duck$static." + other + "$init()\n";

    if (symbols.statics.at(name).runtime) ir += "  call void @duck." + name + "$eval()\n";

    ir += "  br label %exit\nexit:\n  ret void\n}\n";
  }

  if (!own.empty())
    ir += "define internal void @duck$startup() {\nentry:\n  call void @duck$static." + own.back().second +
          "$init()\n  ret void\n}\n@llvm.global_ctors = appending global [1 x { i32, ptr, ptr }] "
          "[{ i32, ptr, ptr } { i32 65535, ptr @duck$startup, ptr null }]\n";

  return ir;
}
std::string emitDeclarations(const Symbols &symbols) {
  std::string declarations;
  std::set<std::string> emitted;

  for (const auto &item : symbols) {
    const auto &signature = item.second;
    auto symbol = signature.nativeSymbol.empty() ? "duck." + item.first : signature.nativeSymbol;

    if (!emitted.insert(symbol).second) continue;

    // Native code sees an enum as its underlying value.
    const auto nativeType = [&](const Type &t) {
      return !signature.nativeSymbol.empty() && enumValue(t) ? symbols.enums.at(t.record).type : t;
    };
    declarations += "declare " + irType(nativeType(signature.result)) + " @" + symbol + "(";

    for (size_t i = 0; i < signature.parameters.size(); ++i) {
      if (i)
        declarations += ", ";

      const auto &p = signature.parameters[i];
      declarations += p.reference ? "ptr" : irType(nativeType(p.type));

      if (i == 0 && signature.method) {
        if (dispatched(item.first, true)) declarations += ", ptr";
      } else if (signature.nativeSymbol.empty() && carriesTable(p))
        declarations += ", ptr";
    }

    declarations += ")\n";
  }

  return declarations;
}
} // namespace duck

#include "Pipeline.h"
#include "Unicode.h"
#include "WideInteger.h"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <sstream>
namespace duck {
namespace {
std::string irType(Type t) {
  if (fixedArray(t)) return "[" + std::to_string(t.extent) + " x " + irType(*t.element) + "]";
  if (collectionValue(t) || dynamicArray(t)) return "ptr";
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
  std::string type = "{ i64, i64";
  for (size_t i = 0; i < symbols.records.at(record).parents.size(); ++i) type += ", ptr";
  for (size_t i = 0; i < symbols.dispatchNames(record).size(); ++i) type += ", ptr, i64, ptr";
  return type + " }";
}
// The byte offset of the parent state at path within record, as a constant.
std::string pathOffset(const std::string &record, const std::vector<size_t> &path) {
  if (path.empty()) return "0";
  std::string indices;
  for (auto index : path) indices += ", i32 " + std::to_string(index);
  return "ptrtoint (ptr getelementptr (%struct." + record + ", ptr null, i32 0" + indices + ") to i64)";
}
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
  // A task record is {handle, arguments..., result} in the caller's frame.
  struct PendingTask {
    std::string name, record, layout;
    size_t result;
    Type resultType;
    const Expr *call;
    Binding destination;
  };
  std::vector<std::vector<PendingTask>> pending;
  std::ostringstream thunks;
  std::string currentBlock = "entry";
  int next = 0, strings = 0, tasks = 0;
  bool usesTextRuntime = false, usesHeapRuntime = false, usesTaskRuntime = false;
  bool usesCalendarRuntime = false, usesWideRuntime = false;
  bool usesBigRuntime = false, usesDecimalRuntime = false, usesCollectionRuntime = false;
  std::set<std::string> cleanups, destroys;
  std::set<DispatchTable> tables;
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
      emit(slot + " = getelementptr " + tableType(functions, record) + ", ptr " + vt + ", i32 0, i32 " +
           std::to_string(2 + index));
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
      const auto type = tableType(functions, record);
      const auto names = functions.dispatchNames(record);
      const auto first = 2 + functions.records.at(record).parents.size() +
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
  // Calls a property accessor on r, passing `value` to a setter.
  Value accessorCall(const Receiver &r, const std::string &owner, const std::string &key,
                     const std::vector<size_t> &path, const Value *value = nullptr) {
    const auto &f = functions.at(key);
    std::string callee = "@duck." + key;
    const std::pair<std::string, std::vector<size_t>> inherited{owner, path};
    auto args = receiverArguments(r, key, &inherited, callee);
    if (value) args += ", " + irType(value->type) + " " + value->value;
    auto id = f.result == Type::Void ? "" : fresh();
    emit((id.empty() ? "" : id + " = ") + "call " + irType(f.result) + " " + callee + "(" + args + ")");
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
    emit(slot + " = getelementptr " + tableType(functions, binding.type.record) + ", ptr " + binding.table +
         ", i32 0, i32 1");
    emit(depth + " = load i64, ptr " + slot);
    emit(whole + " = icmp eq i64 " + depth + ", 0");
    require(whole);
  }
  // A generated initializer writes storage that was never constructed, so
  // overwriting it destroys nothing.
  bool initializer = false;
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
      else {
        usesTextRuntime = true;
        emit("call void @duck.text." + runtime + "(ptr " + part + ")");
      }
    }
  }
  void retain(Value value) {
    if (recordValue(value.type) && functions.managed(value.type)) return eachManaged(value, "retain");
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
    if (managedValue(type)) return slotRelease(type);
    if (recordValue(type) && functions.needsCleanup(type)) {
      cleanups.insert(type.record);
      usesTextRuntime = true;
      return "@duck$cleanup." + type.record;
    }
    return "null";
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
    const auto &element = *type.element;
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
    emit("call void @duck.array$fill(ptr " + address + ", i64 " + std::to_string(type.extent) + ", i64 " +
         sizeOf(element) + ", ptr " + source + ")");
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
    retain({type, value});
    return {type, value};
  }
  void release(Value value) {
    if (collectionValue(value.type)) return freeCollection(value.value, value.type);
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
  // destroy is false for a value moved out by `return local`: its storage
  // still drops its references, but the object lives on in the caller.
  void releaseBinding(const Binding &binding, bool destroy = true) {
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
  Value binary(const std::string &op, Value a, Value b) {
    if (arbitraryNumber(a.type)) return bigBinary(op, a, b);
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
             Magnitude::powerOfTwo(unsigned(integerWidth(a.type) - 1)).decimal());
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
    if (a.type == Type::DateTime) {
      // Compare dates first and use the time of day only for equal dates.
      auto ad = fresh(), at = fresh(), bd = fresh(), bt = fresh();
      auto sameDay = fresh(), dayOrder = fresh(), timeOrder = fresh();
      emit(ad + " = extractvalue { i32, i64 } " + a.value + ", 0");
      emit(at + " = extractvalue { i32, i64 } " + a.value + ", 1");
      emit(bd + " = extractvalue { i32, i64 } " + b.value + ", 0");
      emit(bt + " = extractvalue { i32, i64 } " + b.value + ", 1");
      emit(sameDay + " = icmp eq i32 " + ad + ", " + bd);
      emit(dayOrder + " = " + ints.at(op) + " i32 " + ad + ", " + bd);
      emit(timeOrder + " = " + ints.at(op) + " i64 " + at + ", " + bt);
      emit(id + " = select i1 " + sameDay + ", i1 " + timeOrder + ", i1 " + dayOrder);
      return {Type::Bool, id};
    }
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
    if ((op == "/" || op == "%") && needsWideOperation(a.type)) {
      const auto wide = wideOperationType(a.type);
      auto x = convert(a, wide), y = convert(b, wide);
      emit(id + " = " + instruction + " " + irType(wide) + " " + x.value + ", " + y.value);
      return convert({wide, id}, a.type);
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
  size_t declarations = 0;
  void bind(const Token &t, Binding binding) {
    binding.order = declarations++;
    scopes.back()[t.text] = binding;
  }
  void declare(const Token &t, Value value, bool constant = false, bool scopedBorrow = false) {
    auto address = fresh();
    allocations << "  " << address << " = alloca " << irType(value.type)
                << '\n';
    if (recordValue(value.type) && value.value.empty()) value.value = defaultValue(value.type);
    if ((managedValue(value.type) || collectionValue(value.type) || dynamicArray(value.type)) &&
        value.value.empty())
      value.value = "null";
    const bool defaults = fixedArray(value.type) && value.value.empty();
    if (defaults) value.value = "zeroinitializer";
    if (!value.value.empty())
      emit("store " + irType(value.type) + " " + value.value + ", ptr " +
           address);
    if (defaults) fillDefaults(address, value.type);
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
    auto symbol = "@str." + symbolPrefix + "." + std::to_string(strings++);
    globals << managedTextGlobal(symbol, e.token, type);
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
  // ", i32 a, i32 b" for a field path through parent views.
  static std::string indexList(const std::vector<size_t> &indices) {
    std::string list;
    for (auto index : indices) list += ", i32 " + std::to_string(index);
    return list;
  }
  Binding location(const Expr &e) {
    auto field = analysis.staticFields.find(&e);
    if (field != analysis.staticFields.end())
      return {analysis.expressionTypes.at(&e), "@duck$static." + field->second};
    if (e.kind == "name") return variable(e.token);
    auto type = analysis.expressionTypes.at(&e);
    if (e.kind == "unary") return {type, dereference(*e.args[0]).value};
    if (e.kind == "index") {
      // The array and the index are evaluated once; an index outside the
      // array traps.
      const auto array = analysis.expressionTypes.at(e.args[0].get());
      const auto holder = location(*e.args[0]).value;
      const auto index = expression(*e.args[1]);
      auto slot = fresh();
      if (array.extent) {
        auto valid = fresh();
        emit(valid + " = icmp ult i64 " + index.value + ", " + std::to_string(array.extent));
        require(valid);
        emit(slot + " = getelementptr " + irType(array) + ", ptr " + holder + ", i64 0, i64 " + index.value);
      } else {
        usesCollectionRuntime = true;
        auto c = fresh();
        emit(c + " = load ptr, ptr " + holder);
        emit(slot + " = call ptr @duck.seq$index(ptr " + c + ", i64 " + index.value + ")");
      }
      return {type, slot};
    }
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
         ", i32 0" + indexList(analysis.fieldIndices.at(&e)));
    return {type, id};
  }
  Value lowerExpression(const Expr &e) {
    const auto &op = e.token.text;
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
    if (e.kind == "initializer") {
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
    if (e.kind == "cast")
      return convert(expression(*e.args[0]), e.castType);
    auto member = analysis.enumMembers.find(&e);
    if (member != analysis.enumMembers.end())
      return {analysis.expressionTypes.at(&e), std::to_string(member->second)};
    if (e.kind == "char")
      return {Type::Char, std::to_string(decodeUTF8(e.token).front())};
    if (e.kind == "construct")
      return construct(e);
    if (e.kind == "real" || e.kind == "typeMember")
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
    if (auto property = analysis.properties.find(&e); property != analysis.properties.end()) {
      const auto &access = property->second;
      const auto r = receiver(e.kind == "." ? e.args[0].get() : nullptr, e.token);
      auto v = accessorCall(r, access.owner, access.getter, access.getterPath);
      if (!r.temporary.value.empty()) releaseBinding({r.temporary.type, r.temporary.value});
      return v;
    }
    if (auto member = analysis.collectionMembers.find(&e); member != analysis.collectionMembers.end()) {
      usesCollectionRuntime = true;
      const auto type = analysis.expressionTypes.at(e.args[0].get());
      auto c = fresh();
      if (!fixedArray(type)) emit(c + " = load ptr, ptr " + location(*e.args[0]).value);
      const auto &name = member->second;
      if (fixedArray(type)) return {Type::Long, std::to_string(type.extent)};
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
    if (e.kind == "index" && analysis.expressionTypes.at(e.args[0].get()).base == Type::Pair &&
        !analysis.expressionTypes.at(e.args[0].get()).depth) {
      usesCollectionRuntime = true;
      const auto type = analysis.expressionTypes.at(e.args[0].get());
      auto c = fresh(), entry = fresh(), found = fresh(), slot = fresh();
      emit(c + " = load ptr, ptr " + location(*e.args[0]).value);
      const auto key = expression(*e.args[1]);
      emit(entry + " = call ptr @duck.pair$find(ptr " + c + ", ptr " + keySlot(key) + ", ptr " +
           keyEquality(*type.key) + ")");
      release(key);
      emit(found + " = icmp ne ptr " + entry + ", null");
      require(found);
      emit(slot + " = call ptr @duck.pair$value(ptr " + c + ", ptr " + entry + ")");
      return loadSlot(slot, *type.element);
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
      if (auto constructed = analysis.constructions.find(&e); constructed != analysis.constructions.end()) {
        const auto type = analysis.expressionTypes.at(&e);
        if (functions.parts(type.record).empty()) return {type, defaultValue(type)};
        return {type, constructRecord(type, constructed->second, &e)};
      }
      if (auto re = analysis.reinitializations.find(&e); re != analysis.reinitializations.end()) {
        const auto &target = re->second;
        // The new state is built first, so its arguments may read the old one.
        const auto value = functions.parts(target.type.record).empty()
                               ? defaultValue(target.type)
                               : constructRecord(target.type, target.calls, &e);
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
        // The old object is destroyed and releases its fields in place; the
        // storage, and so every alias and reference to it, stays the same.
        releaseBinding({target.type, address});
        emit("store " + irType(target.type) + " " + value + ", ptr " + address);
        return {Type::Void, ""};
      }
      if (auto completed = analysis.completions.find(&e); completed != analysis.completions.end()) {
        const auto binding = variable(e.token);
        runParts(binding.type, binding.value, completed->second, &e);
        return {Type::Void, ""};
      }
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
          return {Type::Void, ""};
        }
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
      if (analysis.calendarCalls.count(&e))
        return calendarCall(e);
      if (!analysis.callTargets.count(&e))
        return {analysis.expressionTypes.at(&e), defaultValue(analysis.expressionTypes.at(&e))};
      const auto &target = analysis.callTargets.at(&e);
      auto found = functions.find(target);

      const auto &f = found->second;

      std::string args, callee = "@" + (f.nativeSymbol.empty() ? "duck." + target : f.nativeSymbol);
      Value temporaryReceiver{Type::Void, ""};
      if (f.method) {
        const auto r = receiver(e.receiver.get(), e.token);
        const auto inherited = analysis.receiverPaths.find(&e);
        args = receiverArguments(r, target, inherited != analysis.receiverPaths.end() ? &inherited->second : nullptr,
                                 callee);
        temporaryReceiver = r.temporary;
      }
      std::vector<Value> arguments;
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
          if (!f.nativeSymbol.empty() && managedText(v.type)) {
            auto data = fresh();
            emit(data + " = getelementptr i8, ptr " + v.value + ", i64 16");
            v.value = data; // Read-only native borrow; owner stays alive through the call.
          }
          args += irType(v.type) + " " + v.value;
        }
      }
      auto id = f.result == Type::Void ? "" : fresh();
      emit((id.empty() ? "" : id + " = ") + "call " + irType(f.result) + " " + callee + "(" + args + ")");
      for (auto argument : arguments) release(argument);
      // A temporary receiver dies after the call, holding whatever the method left in it.
      if (!temporaryReceiver.value.empty()) releaseBinding({temporaryReceiver.type, temporaryReceiver.value});
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
    auto slot = fresh(), handle = fresh(), resultSlot = fresh(), result = fresh();
    emit(slot + " = getelementptr " + task.layout + ", ptr " + task.record + ", i32 0, i32 0");
    emit(handle + " = load ptr, ptr " + slot);
    emit("call void @duck.task$join(ptr " + handle + ")");
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
    const auto &used = analysis.taskUses.at(&s);
    std::vector<PendingTask> remaining;
    for (const auto &task : pending.back()) {
      if (used.count(task.name)) join(task);
      else remaining.push_back(task);
    }
    pending.back() = std::move(remaining);
  }
  // Arguments are evaluated here, in source order, before the task starts.
  void spawn(const Stmt &s) {
    usesTaskRuntime = true;
    const auto &call = *s.expr;
    const auto &target = analysis.callTargets.at(&call);
    const auto &f = functions.at(target);
    std::vector<Value> arguments;
    for (const auto &argument : call.args) arguments.push_back(expression(*argument));
    std::string layout = "{ ptr";
    for (const auto &argument : arguments) layout += ", " + irType(argument.type);
    layout += ", " + irType(f.result) + " }";
    const auto result = arguments.size() + 1;
    auto record = fresh();
    allocations << "  " << record << " = alloca " << layout << '\n';
    for (size_t i = 0; i < arguments.size(); ++i) {
      auto field = fresh();
      emit(field + " = getelementptr " + layout + ", ptr " + record + ", i32 0, i32 " +
           std::to_string(i + 1));
      emit("store " + irType(arguments[i].type) + " " + arguments[i].value + ", ptr " + field);
    }
    auto entry = "@duck.task." + symbolPrefix + "." + std::to_string(tasks++);
    thunks << "define internal i32 " << entry << "(ptr %task) {\nentry:\n";
    std::string args;
    for (size_t i = 0; i < arguments.size(); ++i) {
      const auto type = irType(arguments[i].type), n = std::to_string(i + 1);
      thunks << "  %p" << n << " = getelementptr " << layout << ", ptr %task, i32 0, i32 " << n
             << "\n  %a" << n << " = load " << type << ", ptr %p" << n << '\n';
      args += (i ? ", " : "") + type + " %a" + n;
    }
    thunks << "  %r = call " << irType(f.result) << " @duck." << target << "(" << args << ")\n"
           << "  %p0 = getelementptr " << layout << ", ptr %task, i32 0, i32 " << result << '\n'
           << "  store " << irType(f.result) << " %r, ptr %p0\n  ret i32 0\n}\n\n";
    auto handle = fresh(), slot = fresh();
    emit(handle + " = call ptr @duck.task$spawn(ptr " + entry + ", ptr " + record + ")");
    emit(slot + " = getelementptr " + layout + ", ptr " + record + ", i32 0, i32 0");
    emit("store ptr " + handle + ", ptr " + slot);
    auto name = s.token;
    name.text = s.name;
    declare(name, {analysis.localTypes.at(&s), ""}, s.constant);
    pending.back().push_back({s.name, record, layout, result, f.result, &call, variable(name)});
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
      cleanupScopes(0, analysis.moves.count(&s) ? variable(s.expr->token).value : "");
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
          auto value = s.expr ? expression(*s.expr).value : defaultValue(type.pointee());
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
      if (const auto type = analysis.expressionTypes.at(s.target.get()); dynamicArray(type) && s.kind != "assign") {
        usesCollectionRuntime = true;
        if (s.kind == "--") {
          auto c = fresh();
          emit(c + " = load ptr, ptr " + location(*s.target).value);
          emit("call void @duck.seq$drop(ptr " + c + ", i1 1, ptr " + elementCleanup(*type.element) + ")");
          return false;
        }
        // The value moves into the new last slot; growing moves the
        // existing elements bytewise into the larger buffer.
        const auto value = expression(*s.expr);
        const auto holder = location(*s.target).value;
        auto slot = fresh();
        emit(slot + " = call ptr @duck.seq$push(ptr " + holder + ", i64 " + sizeOf(*type.element) + ")");
        emit("store " + irType(value.type) + " " + value.value + ", ptr " + slot);
        return false;
      }
      if (const auto type = analysis.expressionTypes.at(s.target.get()); collectionValue(type) && s.kind != "assign") {
        usesCollectionRuntime = true;
        if (s.kind == "--") {
          auto c = fresh();
          emit(c + " = load ptr, ptr " + location(*s.target).value);
          if (type.base == Type::Pair)
            emit("call void @duck.pair$dropLast(ptr " + c + ", ptr " + slotRelease(*type.key) + ", ptr " +
                 slotRelease(*type.element) + ")");
          else
            emit("call void @duck.seq$drop(ptr " + c + ", i1 " + (type.base == Type::Stack ? "1" : "0") +
                 ", ptr " + slotRelease(*type.element) + ")");
          return false;
        }
        if (type.base != Type::Pair) {
          const auto value = expression(*s.expr);
          const auto holder = location(*s.target).value;
          auto slot = fresh();
          emit(slot + " = call ptr @duck.seq$push(ptr " + holder + ", i64 " + sizeOf(*type.element) + ")");
          emit("store " + irType(value.type) + " " + value.value + ", ptr " + slot);
          return false;
        }
        // A new key is appended with its value; an existing key keeps its
        // place and gets the new value, and the duplicate key is released.
        const auto key = expression(*s.expr->args[0]);
        const auto value = expression(*s.expr->args[1]);
        const auto holder = location(*s.target).value;
        const auto probe = keySlot(key);
        auto c = fresh(), found = fresh(), missing = fresh();
        emit(c + " = load ptr, ptr " + holder);
        emit(found + " = call ptr @duck.pair$find(ptr " + c + ", ptr " + probe + ", ptr " +
             keyEquality(*type.key) + ")");
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
        if (managedValue(*type.element)) {
          auto previous = fresh(), old = fresh();
          emit(previous + " = call ptr @duck.pair$value(ptr " + c + ", ptr " + found + ")");
          emit(old + " = load ptr, ptr " + previous);
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
        return false;
      }
      if (auto property = analysis.properties.find(s.target.get()); property != analysis.properties.end()) {
        const auto &access = property->second;
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
        return false;
      }
      auto address = location(*s.target);
      if (address.type.role != Type::Raw) {
        auto word = aliasWord(*s.expr); // Retain first, including on self-assignment.
        releaseBinding(address);
        emit("store ptr " + word + ", ptr " + address.value);
        return false;
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
      for (const auto &path : functions.visibleFields(self.record)) {
        auto address = fresh();
        emit(address + " = getelementptr " + irType(self) + ", ptr %arg0, i32 0" + indexList(path.indices));
        bind(path.field->name, {path.field->type, address});
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
    bool terminated = sequence(f.body);
    if (!terminated) {
      cleanupScopes(0);
      emit("ret void");
    }
    output << ") {\nentry:\n" << allocations.str() << code.str() << "}\n\n" << thunks.str();
    return {globals.str(), output.str(), usesTextRuntime, usesHeapRuntime, usesTaskRuntime,
            usesCalendarRuntime, usesWideRuntime, usesBigRuntime, usesDecimalRuntime, usesCollectionRuntime,
            cleanups, destroys, tables};
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
  // Each module keeps private copies of the defaults it may construct.
  std::set<std::string> zeros;
  for (const auto &[name, record] : symbols.records)
    for (const auto &field : record.fields)
      if (managedValue(field.type) && (!field.initializer || field.runtimeDefault))
        zeros.insert(managedZero(field.type));
  for (const auto &[name, info] : symbols.statics)
    if (managedValue(info.type) && (!info.initializer || info.runtime) && !info.imported)
      zeros.insert(managedZero(info.type));
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
    if (hasDefaults(symbols, name))
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
      if (!managedText(info.type)) continue;
      Token text = info.members[i];
      text.text = info.values[i];
      ir << managedTextGlobal(enumTextSymbol(name, i), text, info.type);
    }
    ir << "@enum." << name << ".names = private unnamed_addr constant [" << count << " x ptr] [";
    for (size_t i = 0; i < info.members.size(); ++i)
      ir << (i ? ", " : "") << "ptr @enum." << name << ".name." << i;
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
      if (dynamicArray(field.type)) {
        const auto part = "%field" + std::to_string(i), held = "%held" + std::to_string(i);
        const auto &element = *field.type.element;
        const std::string cleanup = managedValue(element) ? "@duck$slot.release"
                                    : recordValue(element) && symbols.needsCleanup(element)
                                        ? "@duck$cleanup." + element.record
                                        : "null";
        ir += "  " + part + " = getelementptr %struct." + record + ", ptr %self, i32 0, i32 " +
              std::to_string(i) + "\n  " + held + " = load ptr, ptr " + part + "\n  call void @duck.seq$free(ptr " +
              held + ", ptr " + cleanup + ")\n  store ptr null, ptr " + part + "\n";
        continue;
      }
      if (collectionValue(field.type)) {
        const auto part = "%field" + std::to_string(i), held = "%held" + std::to_string(i);
        auto release = [](const Type &t) { return managedValue(t) ? "@duck$slot.release" : "null"; };
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
std::string emitTables(const Symbols &symbols, std::set<DispatchTable> tables) {
  std::string ir;
  std::set<DispatchTable> emitted;
  while (!tables.empty()) {
    const auto table = *tables.begin();
    tables.erase(tables.begin());
    if (!emitted.insert(table).second) continue;
    const auto &[record, path] = table;
    const auto seen = symbols.recordAt(record, path);
    ir += tableSymbol(table) + " = internal constant " + tableType(symbols, seen) + " { i64 " +
          pathOffset(record, path) + ", i64 " + std::to_string(path.size());
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
    declarations += "declare " + irType(signature.result) + " @" + symbol + "(";
    for (size_t i = 0; i < signature.parameters.size(); ++i) {
      if (i)
        declarations += ", ";
      const auto &p = signature.parameters[i];
      declarations += p.reference ? "ptr" : irType(p.type);
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

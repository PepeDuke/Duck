#pragma once
#include "Syntax.h"
#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <tuple>

namespace duck {
// Stage artifacts belong to the caller. No stage uses global mutable state.
std::vector<Token> lex(const std::string &source,
                       const std::string &sourceName = "");
// Collects type names across all units, qualified by the static bodies that
// enclose them (`Name.Space`). An enum name is recorded both as Name and as
// #@Name so the parser can tell enums from containers.
void collectContainerNames(const std::vector<Token> &tokens, std::set<std::string> &names);
// Resolves #define, #if, and #else over one file's tokens, before any names
// are collected, so an inactive branch publishes nothing. A define is visible
// from its line to the end of its file and to nothing but #if and #define.
std::vector<Token> preprocess(std::vector<Token> tokens);
// A macro is kept as syntax: its parameter types and body stay tokens until a
// call expands them in the caller's grammar context.
struct MacroParameter {
  std::vector<Token> type;
  Token name;
};
struct Macro {
  Token name;
  std::vector<MacroParameter> parameters;
  std::vector<Token> body; // Between the braces.
};
using MacroTable = std::map<std::string, Macro>; // By qualified name.
// Collects macros from file scope and container bodies across all units, so
// calls may precede definitions and cross files.
void collectMacros(const std::vector<Token> &tokens, MacroTable &macros);
Program parse(std::vector<Token> tokens);
Program parse(std::vector<Token> tokens, const std::set<std::string> &names);
Program parse(std::vector<Token> tokens, const std::set<std::string> &names, const MacroTable &macros);
// Keep units in input order. Combining is a barrier before declaration
// collection.
Program combineUnits(std::vector<Program> units);

std::string typeName(Type type);
// A constructor is an instance method keyed by its parameter types, as in
// `Gun.$$new$int` for Gun(int ammo) and `Gun.$$new` for Gun(); a partial
// constructor, `-Entity(World w)`, is `Entity.$$part$World`; the destructor
// is `Gun.$$destroy`. Source identifiers cannot contain '$', so these never
// collide with user members. Type names are escaped so the key has no '.'.
inline std::string constructorKey(const std::string &record, const std::vector<Parameter> &params,
                                  bool partial = false) {
  std::string member = partial ? "$part" : "$new";

  for (const auto &p : params) {
    member += '$';

    for (unsigned char c : typeName(p.type)) {
      if (std::isalnum(c)) member += char(c);
      else if (c == '_') member += "__";
      else if (c == '.') member += "_d";
      else if (c == '*') member += "_p";
      else member += "_x" + std::to_string(int(c));
    }

    if (p.reference) member += "_r";
  }

  return methodKey(record, member);
}
inline std::string destructorKey(const std::string &record) { return methodKey(record, "$destroy"); }
// An instance method written in source, as opposed to a constructor,
// destructor, or generated initializer. It can be overridden, and it takes
// its receiver's dispatch table as a hidden parameter after the receiver.
inline bool dispatched(const std::string &key, bool method) {
  return method && memberName(key).find('$') == std::string::npos;
}
// A reference parameter of container type, outside native functions, takes
// the dispatch table of the object it refers to as a hidden parameter after
// it, so calls through it reach the object's overrides.
inline bool carriesTable(const Parameter &parameter) {
  return parameter.reference && recordValue(parameter.type);
}
// A dispatch table describes one object of dynamic type `record` as seen
// through the parent state at `path` (empty for the object itself). Its
// layout, for the container B at path, is:
//   i64 the byte offset of path within record,
//   i64 the length of path, zero only for the whole object,
//   ptr the type descriptor of record, for duck calls,
//   ptr the table for each of B's parents, in order,
//   then, for each name of dispatchNames(B): ptr the final overrider,
//   i64 the byte offset of the overrider's receiver within record, and
//   ptr the overrider's own table.
using DispatchTable = std::pair<std::string, std::vector<size_t>>;
// One construction part of a container: the partial or regular
// constructors of the container or of a parent embedded at `path`. Any one
// overload completes the part; `automatic` is the zero-argument overload,
// which runs when no other is chosen, or empty.
struct ConstructionPart {
  std::vector<size_t> path;
  std::string record;
  bool partial = false;
  std::vector<std::string> overloads;
  std::string automatic;
  std::string id() const {
    std::string text;

    for (auto index : path) text += std::to_string(index) + ".";

    return text + (partial ? "partial" : "regular");
  }
  // How diagnostics name the part, such as -Entity(...) or Gun(...).
  std::string text() const { return (partial ? "-" : "") + record + "(...)"; }
};
// One constructor run during a construction or completion: the overload,
// the parent path it applies to, and the call arguments it takes.
struct PartCall {
  std::vector<size_t> path;
  std::string key;
  size_t first = 0, count = 0;
};
struct Signature {
  Type result;
  std::vector<Parameter> parameters;
  std::string nativeSymbol;
  Token declaration;
  std::set<size_t> nonescapingParameters;
  bool method = false;
  bool protectedAccess = false;
  bool inferred = false; // An accessor whose property type is not known yet.
  std::vector<Tag> tags;
  // What a function with an owner-role result can hand back: ownership
  // (return ::x) and a tracked alias (return x). resolveReturnRoles fills these.
  bool owns = false;
  bool lends = false;
};
// The text a duck call site and a method descriptor compare: "void(float,int&)".
inline std::string signatureText(const Type &result, const std::vector<Parameter> &parameters, size_t first) {
  std::string text = typeName(result) + "(";

  for (size_t i = first; i < parameters.size(); ++i)
    text += (i > first ? "," : "") + typeName(parameters[i].type) + (parameters[i].reference ? "&" : "");

  return text + ")";
}
// One member of a type descriptor. Fields come first in layout order, then
// methods in dispatch order; inherited members keep their declaring parent.
struct TypeMember {
  std::string name, key, declaring; // key is empty for a field.
  bool isProtected = false;
  std::vector<size_t> path; // A field's indices, or a method's receiver state.
  std::vector<Tag> tags;
  const Signature *signature = nullptr; // Null for a field.
  Type type;                            // A field's type.
};
// Property accessors are instance methods named get-Name and set-Name, so
// they override, dispatch, and inherit like other methods.
inline std::string getterKey(const std::string &record, const std::string &property) {
  return methodKey(record, "get-" + property);
}
inline std::string setterKey(const std::string &record, const std::string &property) {
  return methodKey(record, "set-" + property);
}
// The property an accessor key belongs to, or empty for other functions.
inline std::string propertyName(const std::string &key) {
  const auto member = memberName(key);
  return member.rfind("get-", 0) == 0 || member.rfind("set-", 0) == 0 ? member.substr(4) : "";
}
// A merged enum. Values are in member order: signed decimal integers, LLVM
// hexadecimal floating constants, or UTF-8 text, depending on the type.
struct EnumInfo {
  Token declaration;
  Type type;
  std::vector<Token> members;
  std::vector<std::string> values;
  bool attribute = false; // Marked [args]: members can be used as attributes.
  size_t find(const std::string &member) const {
    size_t i = 0;

    while (i < members.size() && members[i].text != member) ++i;

    return i;
  }
};
// Where an instance member lives: field indices from the searched container
// down through parent views, and the container that declares the member.
struct MemberPath {
  std::vector<size_t> indices;
  std::string declaring;
  const Parameter *field = nullptr; // Null for a method.
};
// A static field's single storage location, keyed by its qualified name.
struct StaticInfo {
  Token declaration;
  Type type;
  bool constant = false;
  bool protectedAccess = false;
  const Expr *initializer = nullptr; // Refers to the input AST.
  bool imported = false;             // Storage is defined by another module.
  std::string value;                 // LLVM constant, set by resolveStaticInitializers.
  size_t order = 0;                  // Declaration order within its module.
  bool runtime = false;              // Initialized by a generated $eval before Main.
  Usings usings;                     // Visible where the field was declared.
  std::string scope;                 // A `[@Name]` field's scope, with its declared type.
  Type scopedType = Type::Void;
};
inline bool hasField(const Record &record, const std::string &name) {
  for (const auto &field : record.fields)
    if (field.name.text == name) return true;

  return false;
}
struct Symbols : std::map<std::string, Signature> {
  std::map<std::string, Record> records;
  std::map<std::string, EnumInfo> enums;
  std::map<std::string, StaticInfo> statics;
  // The Exec table, keyed by sourceKey: this module's source files, the
  // hidden function of each one with statements, and its dependencies' files.
  std::set<std::string> sources, importedSources;
  std::map<std::string, std::string> scripts;
  // The hidden function an Exec call runs, or null for a file with no
  // statements. Rejects computed paths and files outside this module.
  const std::string *execTarget(const Expr &call) const;
  // Member lookup searches the container itself, then each parent left to
  // right, depth first, so the leftmost parent wins an overlap.
  bool findField(const std::string &record, const std::string &name, MemberPath &path) const;
  const_iterator findMethod(const std::string &record, const std::string &name,
                            MemberPath &path) const;
  bool derivesFrom(const std::string &record, const std::string &ancestor) const;
  // How many times base occurs among record's parents, transitively, and
  // the parent-view path to its first occurrence, left to right, depth first.
  size_t parentPath(const std::string &record, const std::string &base, std::vector<size_t> &path) const;
  // The container whose state sits at a parent-view path within record.
  std::string recordAt(std::string record, const std::vector<size_t> &path) const;
  // Overridable method names visible in a container, its own and inherited,
  // sorted: the order of its dispatch table's method slots.
  std::vector<std::string> dispatchNames(const std::string &record) const;
  // The method `name` runs for an object of type record seen through the
  // parent state at path: the most derived declaration on the path, else
  // what that parent state's own lookup finds. Returns its key and the path
  // of its receiver within record.
  DispatchTable overrider(const std::string &record, const std::vector<size_t> &path,
                          const std::string &name) const;
  // Fields an instance method may name directly, inherited ones included,
  // with each name bound to its highest-priority declaration.
  std::vector<MemberPath> visibleFields(const std::string &record) const;
  // Every member the container's type descriptor lists.
  std::vector<TypeMember> members(const std::string &record) const;
  // A container holding string, name, or big storage anywhere in its layout,
  // nested containers and parents included. Copies retain and cleanup releases.
  bool managedRecord(const std::string &record) const;
  // Whether an object of type holder can contain, or own through any chain of
  // owning fields, an object of type held; holder itself counts.
  bool canOwn(const std::string &holder, const std::string &held) const;
  // A container whose construction evaluates defaults at run time, in its
  // own fields, nested containers, or parents.
  bool runtimeDefaults(const std::string &record) const;
  // The container's own constructor keys, not inherited ones, regular or partial.
  std::vector<std::string> constructors(const std::string &record, bool partial = false) const;
  // Construction parts in run order: each parent's parts, left to right and
  // depth first, then the container's partial part, then its regular part.
  std::vector<ConstructionPart> parts(const std::string &record) const;
  // The zero-argument constructor's key, or empty when there is none.
  std::string defaultConstructor(const std::string &record) const {
    const auto key = constructorKey(record, {});
    return count(key) ? key : "";
  }
  // A container with a destructor, or with one anywhere in its layout
  // (fields and parents). Such values are move-only and destroyed once.
  bool destructible(const std::string &record) const;
  bool managed(const Type &type) const {
    return managedValue(type) || funcValue(type) || trackedAlias(type) ||
           (recordValue(type) && managedRecord(type.record));
  }
  // Storage that must release, destroy, or free something when it dies.
  bool needsCleanup(const Type &type) const {
    return managedValue(type) || funcValue(type) || trackedAlias(type) || collectionValue(type) ||
           dynamicArray(type) || (fixedArray(type) && needsCleanup(*type.element)) ||
           (recordValue(type) && (managedRecord(type.record) || destructible(type.record)));
  }
  // Values that cannot be copied, only moved: they own heap storage or
  // something destroyed exactly once.
  bool moveOnly(const Type &type) const {
    return collectionValue(type) || dynamicArray(type) || (fixedArray(type) && needsCleanup(*type.element)) ||
           (recordValue(type) && destructible(type.record));
  }
  // Values whose default is all zero bits: plain scalars, and containers
  // without constructors, defaults, or anything to clean up.
  bool zeroDefault(const Type &type) const;
  void validateType(const Type &type, const Token &token) const {
    if (funcValue(type)) {
      if (!type.signature) return; // Inferred from the initializer.

      validateType(type.signature->result, token);

      for (const auto &parameter : type.signature->parameters) validateType(parameter.type, token);

      return;
    }

    if (arrayValue(type)) {
      const auto &element = *type.element;
      const bool plain = trackedAlias(element) ||
                         (!element.depth && (numeric(element) || element == Type::Bool || element == Type::Char ||
                                             enumValue(element) || managedValue(element) || funcValue(element) ||
                                             decimalValue(element) || calendarValue(element)));

      if (!plain && !recordValue(element) && !arrayValue(element))
        fail(token, "an array cannot hold " + typeName(element) +
                        "; elements are numbers, bool, char, enums, string, name, big, tiny, dates and times, "
                        "func, containers, and arrays");

      validateType(element, token);
      return;
    }

    if (collectionValue(type)) {
      // Elements are plain values or reference-counted text and big
      // integers; keys must also compare by value.
      auto element = [&](const Type &t) {
        if (elementValue(t)) return markupValue(type);
        if (trackedAlias(t)) return !gridValue(type);

        return !t.depth && (numeric(t) || t == Type::Bool || t == Type::Char || enumValue(t) ||
                            managedValue(t) || decimalValue(t) || calendarValue(t));
      };
      auto key = [](const Type &t) {
        return !t.depth && (integral(t) || t == Type::Bool || t == Type::Char || enumValue(t) ||
                            managedValue(t) || calendarValue(t));
      };

      if (!element(*type.element))
        fail(token, std::string(gridValue(type) ? "a grid" : "a collection") + " cannot hold " +
                        typeName(*type.element) +
                        "; elements are numbers, bool, char, enums, string, name, big, tiny, dates and times, "
                        "and heap references");

      if (gridValue(type)) return validateType(*type.element, token);

      if (type.key && !key(*type.key))
        fail(token, "a pair cannot be keyed by " + typeName(*type.key) +
                        "; keys are integers, bool, char, enums, string, name, big, and dates and times");

      validateType(*type.element, token);

      if (type.key) validateType(*type.key, token);

      return;
    }

    if (type.base == Type::Record && !records.count(type.record))
      fail(token, "unknown container '" + type.record + "'");

    if (type.base == Type::Enum && !enums.count(type.record))
      fail(token, "unknown enum '" + type.record + "'");
  }
};
// One spelling per file, for matching Exec paths against input paths.
std::string sourceKey(const std::string &path);
// Merges fragments by name in input order, then assigns implicit values.
std::map<std::string, EnumInfo> resolveEnums(const std::vector<EnumFragment> &fragments);
bool borrowsOnly(const Stmt &statement, const std::string &name);
Symbols collectSymbols(const Program &program, bool requireEntry = true);
// Checks parent lists, rejects inheritance cycles, redeclared inherited
// fields, and overrides whose signatures differ from an inherited method,
// and embeds each parent as a leading field. Runs once all
// containers, imported ones included, are known.
void resolveInheritance(Symbols &symbols);
// Gives each untyped property the type of its getter expression, checks
// property names and accessor types, and updates the accessor functions in
// both programs and every symbol table that holds them.
void resolveProperties(Symbols &symbols, const std::vector<const Program *> &programs,
                       const std::vector<Symbols *> &tables);
// Checks that every override keeps its overridden methods' signatures.
// Runs once property types are known.
void validateOverrides(const Symbols &symbols);
// Resolves every [Name] attribute on a field or method to its enum member.
void resolveAttributes(Symbols &symbols);
void validateLayouts(const Symbols &symbols);
// Static storage is initialized before Main from compile-time constants
// only, so no initialization order exists to observe. Fills each value.
void resolveStaticInitializers(Symbols &symbols);
// Evaluates container field defaults with the same constant rules as static
// initializers, plus string and name literals.
void resolveFieldDefaults(Symbols &symbols);
// Generates `Container.$default$Field` methods for runtime defaults and
// `Static$eval` functions for runtime static initializers. Signatures for
// imported containers go to declarations; bodies only for this module.
std::vector<Function> generateInitializers(Symbols &symbols, Symbols &declarations);
// Key of the generated method evaluating a field's runtime default.
inline std::string defaultKey(const std::string &record, const std::string &field) {
  return methodKey(record, "default$" + field);
}
// Finds which functions return tracked references, across every path, and
// gives those an owner-role result; repeats until functions returning other
// functions' results settle. Runs once field types and defaults are known.
void resolveReturnRoles(Symbols &symbols, const std::vector<const Program *> &programs,
                        const std::vector<Symbols *> &tables);
void validateNativeSymbols(const Symbols &symbols);
// Checks that every container field and parent with constructors can be
// default-constructed, and that values with destructors are never copied
// into by-value parameters or held in static storage.
void validateLifetimes(const Symbols &symbols);

// Identifies a declared local across analysis and lowering.
inline std::string captureKey(const Token &t) {
  return t.source + ":" + std::to_string(t.line) + ":" + std::to_string(t.column) + ":" + t.text;
}
// Expression pointers refer to the input AST, which must outlive this artifact.
// Each function gets its own analysis, suitable for independent worker tasks.
struct FunctionAnalysis {
  std::map<const Expr *, Type> expressionTypes;
  std::map<const Expr *, Type> conversions;
  // Field position, through parent views for an inherited field.
  std::map<const Expr *, std::vector<size_t>> fieldIndices;
  // An inherited method call: the receiver's container and the parent-view
  // indices that reach the declaring parent's state.
  std::map<const Expr *, std::pair<std::string, std::vector<size_t>>> receiverPaths;
  std::map<const Expr *, std::string> integerConstants;
  std::map<const Expr *, std::string> floatingConstants;
  // tiny/utiny literals, already rounded: {signed coefficient, exponent}.
  std::map<const Expr *, std::pair<std::string, std::string>> decimalConstants;
  std::map<const Expr *, std::string> callTargets;
  // Names and paths that denote static field storage: the qualified name.
  std::map<const Expr *, std::string> staticFields;
  // Enum member constants such as Day.Monday: the member's index.
  std::map<const Expr *, size_t> enumMembers;
  // Container constructions (`Gun(5)`, `Gun g(w, 5)`, `Gun()`): the parts
  // run in order, automatic ones included. Empty without constructors.
  std::map<const Expr *, std::vector<PartCall>> constructions;
  // `g(w)` calls supplying the parts a local is still missing.
  std::map<const Expr *, std::vector<PartCall>> completions;
  // Reference arguments of a derived container passed for a parent
  // parameter: the parent-view path to the parameter's container.
  std::map<const Expr *, std::vector<size_t>> upcasts;
  // Property reads (`p.Age`, or `Age` inside a method) and assignment
  // targets: the receiver's container and the accessors used, with the
  // parent-view path from the receiver to each accessor's declaring state.
  struct PropertyAccess {
    std::string owner, getter, setter;
    std::vector<size_t> getterPath, setterPath;
  };
  std::map<const Expr *, PropertyAccess> properties;
  // Collection members read through `.`: Len, Front, Top, Last, LastKey.
  std::map<const Expr *, std::string> collectionMembers;
  // `g(5)`, `p(5)`, and `box.Inner(5)` calling a constructor on a complete
  // object: the new state is built first, then the old one is destroyed and
  // the new state moves into the same storage. A field target names the
  // receiver's record and the field's indices within it.
  struct Reinitialization {
    Type type;
    std::vector<PartCall> calls;
    std::string owner;
    std::vector<size_t> field;
  };
  std::map<const Expr *, Reinitialization> reinitializations;
  // `return local` statements that move a value with a destructor out of the
  // function instead of destroying it.
  std::set<const Stmt *> moves;
  // Calls resolved to a calendar built-in such as Year or AddDays.
  std::set<const Expr *> calendarCalls;
  std::map<const Stmt *, Type> localTypes;
  std::set<const Stmt *> scopedBorrows;
  std::set<const Expr *> receiverStorage;
  std::set<const Stmt *> terminatingLoops;
  // Filled by scheduleTasks. A spawned statement runs its call as a worker
  // task; pending tasks join before a statement that conflicts with them,
  // before a barrier statement, and at the end of their statement list.
  std::set<const Stmt *> taskSpawns;
  std::set<const Stmt *> taskBarriers;
  std::map<const Stmt *, std::set<const Stmt *>> taskConflicts; // The earlier spawns to join first.
  // Filled by planOrderedBlocks. A `${ }` block with no entry here (one inside
  // a lambda) takes the scheduler-wide exclusion.
  struct OrderedPlace {
    bool isStatic = false;
    Token root;             // The parameter or local, or the static's declaration.
    std::string staticName; // Qualified, for a static.
    std::vector<size_t> path;
  };
  struct OrderedLocks {
    bool exclusive = false;           // Effects unresolved before entry.
    std::vector<OrderedPlace> places; // One per scalar leaf of each resource.
  };
  std::map<const Stmt *, OrderedLocks> orderedLocks;
  // GetName, GetDeclaringName, and GetTags: the container, the member's index
  // in its descriptor (-1 for the container), and the attributes returned.
  struct Reflection {
    std::string record;
    long member = -1;
    std::string query;
    std::vector<size_t> tags;
    Type element;
  };
  std::map<const Expr *, Reflection> reflections;
  // `e..Foo(...)` and its checks: the shape the call site passes.
  struct DuckCall {
    Type record, result;
    std::vector<Parameter> parameters;
    std::string signature;
  };
  std::map<const Expr *, DuckCall> duckCalls;
  // `x.Label`: the child type's qualified name and where x stores children.
  struct MarkupLookup {
    std::string child;
    std::vector<size_t> holder;
  };
  std::map<const Expr *, MarkupLookup> markupLookups;
  std::set<const Expr *> markupIndexed;                      // `x.Label[i]` or `x.GetChildren()[i]`.
  std::map<const Expr *, std::vector<size_t>> markupChildren; // `x.GetChildren()`: where x stores them.
  std::set<const Expr *> elementNames;                       // `element.GetName()`.
  std::set<const Stmt *> markupSplices;                      // Content that is markup rather than a value.
  // Reads and writes of variables in a named scope, and their declared types.
  struct ScopedPlace {
    std::string scope;
    Type type;
  };
  std::map<const Expr *, ScopedPlace> scopedPlaces;
  bool scopeEffects = false; // Starts, ends, queries, or loops inside a scope.
  bool endsScope = false;    // Contains @end.
  // Functions a call to which can leave through an @end, set per module.
  std::shared_ptr<const std::set<std::string>> unwinders;
  bool unwindIndirect = false; // Calls through values may also leave that way.
  // Calls through a func value. A field callee names its owner and field
  // indices, so a member lambda receives the object it was called on.
  struct IndirectCall {
    Type type;
    bool field = false;
    std::string owner;
    std::vector<size_t> path;
  };
  std::map<const Expr *, IndirectCall> indirectCalls;
  // Global functions used as values: the qualified name.
  std::map<const Expr *, std::string> functionValues;
  struct Capture {
    enum Kind { Box, Alias, Borrow, This };
    Kind kind;
    Token token;
    Type type;
  };
  struct LambdaInfo {
    std::vector<Capture> captures;
    bool bound = false;    // A member lambda: its env is the receiver object.
    bool borrowed = false; // Holds this or a reference: it cannot outlive the frame.
    bool usesThis = false;
    std::string record; // The receiver's container, when usesThis or bound.
  };
  std::map<const Expr *, LambdaInfo> lambdas;
  // `return x` from a function with an owner-role result: x stays with its
  // owner and is returned as a borrowed handle.
  std::set<const Stmt *> handleReturns;
  // `if p` on a tracked reference whose branch uses the evaluated reference:
  // the object is pinned through the branch.
  std::set<const Stmt *> boundConditions;
  // Pointers used as conditions outside a plain `if p`: !p, p && q, while p,
  // p ? a | b. Each evaluates to true when the pointer is not null.
  std::set<const Expr *> pointerTests;
  // Uses of that reference inside the branch: the `if` each one belongs to.
  std::map<const Expr *, const Stmt *> checkedUses;
  // Locals a lambda captures by reference; they live in a shared heap box.
  std::set<std::string> captured;
};
FunctionAnalysis analyzeFunction(const Function &function,
                                 const Symbols &symbols);
// Evaluate explicit #Call expressions after all bodies have been typed, before
// scheduling or lowering. Results use the existing typed constant tables.
void evaluateCompileTimeCalls(const std::vector<const Function *> &functions,
                              std::vector<FunctionAnalysis> &analyses);

// A piece of storage a function touches: a static field, a reference
// parameter (the receiver is parameter 0), or a local. `path` holds field
// indices below the root, so two resources overlap when they share a root and
// one path is a prefix of the other. Array elements collapse to their array.
struct Resource {
  enum Kind { Static, Parameter, Local };
  Kind kind = Local;
  std::string root;     // The static's qualified name, or the local's unique id.
  size_t parameter = 0;
  std::vector<size_t> path;
  bool operator<(const Resource &o) const {
    return std::tie(kind, root, parameter, path) < std::tie(o.kind, o.root, o.parameter, o.path);
  }
  bool overlaps(const Resource &o) const {
    if (kind != o.kind || root != o.root || parameter != o.parameter) return false;

    const auto common = std::min(path.size(), o.path.size());
    return std::equal(path.begin(), path.begin() + common, o.path.begin());
  }
};
struct EffectSet {
  std::set<Resource> reads, writes;
  bool unknown = false;   // Touches storage the compiler cannot name: a synchronization barrier.
  bool external = false;  // Observable outside the program, as Print is: also a barrier.
  bool locks = false;     // Enters a `${ }` block that acquires something.
  bool exclusive = false; // Enters a `${ }` block needing the scheduler-wide exclusion.
  void merge(const EffectSet &o) {
    reads.insert(o.reads.begin(), o.reads.end());
    writes.insert(o.writes.begin(), o.writes.end());
    unknown |= o.unknown;
    external |= o.external;
    locks |= o.locks;
    exclusive |= o.exclusive;
  }
  // Whether running this and `o` in either order could be told apart.
  bool conflicts(const EffectSet &o) const {
    auto meet = [](const std::set<Resource> &a, const std::set<Resource> &b) {
      for (const auto &x : a)
        for (const auto &y : b)
          if (x.overlaps(y)) return true;

      return false;
    };
    return meet(writes, o.writes) || meet(writes, o.reads) || meet(reads, o.writes);
  }
};
// A function's read/write signature, in terms of its own parameters. Locals
// never appear: they die with the call.
struct EffectSummary : EffectSet {
  bool threadSafe = false; // Its body exchanges only plain data, so it may run on a worker.
};
using EffectTable = std::map<std::string, EffectSummary>;
// Least fixpoint over the call graph, so a recursive group shares its effects.
// Imported and native functions are unknown. A library (entry = false) can
// have its methods overridden by importers, so its virtual calls are too.
EffectTable summarizeEffects(const Program &program, const std::vector<FunctionAnalysis> &analyses,
                             const Symbols &symbols, bool entry = true);
// Decides what each `${ }` block locks. This is part of the program's meaning,
// so it runs for sequential builds as well.
void planOrderedBlocks(const Function &function, const EffectTable &effects, const Symbols &symbols,
                       FunctionAnalysis &analysis, bool entry = true);
void scheduleTasks(const Program &program, size_t index, const EffectTable &effects, const Symbols &symbols,
                   std::vector<FunctionAnalysis> &analyses, bool entry = true);

struct FunctionIR {
  std::string globals;
  std::string definition;
  bool usesTextRuntime = false;
  bool usesHeapRuntime = false;
  bool usesTaskRuntime = false;
  bool usesLockRuntime = false;
  bool usesCalendarRuntime = false;
  bool usesWideRuntime = false;
  bool usesBigRuntime = false;
  bool usesDecimalRuntime = false;
  bool usesCollectionRuntime = false;
  bool usesFuncRuntime = false;
  bool usesMetaRuntime = false;
  bool usesMarkupRuntime = false;
  bool usesScopeRuntime = false;
  // Containers whose heap cleanup functions this function's owners use.
  std::set<std::string> cleanups;
  // Containers whose @duck$destroy.<container> routines this function calls.
  std::set<std::string> destroys;
  // Dispatch tables this function passes to methods.
  std::set<DispatchTable> tables;
  // Module-level helper definitions by symbol, such as the cleanup of a
  // nested array element; emitted once per module.
  std::map<std::string, std::string> helpers;
  // Containers whose type descriptors this function reads.
  std::set<std::string> types;
  std::set<std::string> scopeNames; // Named scopes this function refers to.
};
FunctionIR lowerFunction(const Function &function, const Symbols &symbols,
                         const FunctionAnalysis &analysis);
// nativeSymbols lists symbols already declared by the program, so runtime
// imports are not declared twice.
// mainArguments selects the native entry for Main(string [] args).
std::string assembleModule(const std::vector<FunctionIR> &functions,
                           bool entry = true,
                           const std::set<std::string> &nativeSymbols = {},
                           bool mainArguments = false);
std::string textRuntimeIR();
// The native entry that builds Main's argument array.
std::string entryArgumentsIR();
std::string collectionRuntimeIR();
std::string collectionBigKeyIR();
std::string heapRuntimeIR();
std::string calendarRuntimeIR();
std::string wideRuntimeIR();
std::string bigRuntimeIR();
std::string decimalRuntimeIR();
// Closures, shared boxes, and the descriptor lookup behind duck calls.
std::string funcRuntimeIR();
std::string metaRuntimeIR();
std::string markupRuntimeIR();
std::string scopeRuntimeIR(const std::set<std::string> &nativeSymbols);
std::string taskRuntimeIR(const std::set<std::string> &nativeSymbols);
// The locks behind `${ }`. The task runtime reads their per-thread state too.
std::string lockRuntimeIR(const std::set<std::string> &nativeSymbols);
// The native entry of a test build: runs the tests given as (name, symbol),
// honoring --list and name filters from the command line.
std::string testRunnerIR(const std::vector<std::pair<std::string, std::string>> &tests,
                         const std::set<std::string> &nativeSymbols);
std::string emitRecords(const Symbols &symbols);
std::string emitEnums(const Symbols &symbols);
std::string emitDeclarations(const Symbols &symbols);
// Defines this module's static storage and declares imported storage.
std::string emitStatics(const Symbols &symbols);
// Defines @duck$cleanup.<container>, releasing a heap payload's string, name,
// and big fields before the heap runtime frees it.
// A destructible payload runs @duck$destroy.<container> first.
std::string emitCleanups(const Symbols &symbols, const std::set<std::string> &containers);
// Defines @duck$destroy.<container> for each container and every
// destructible container in their layouts: the user destructor, then the
// fields in reverse layout order, so parents are destroyed last.
std::string emitDestroys(const Symbols &symbols, std::set<std::string> containers);
// Module-private dispatch tables and type descriptors, with everything they reference.
std::string emitTables(const Symbols &symbols, std::set<DispatchTable> tables, std::set<std::string> types = {});

// Sequential orchestration. Analysis finishes before lowering begins.
// parallel = false keeps every call on the calling thread.
std::string compile(const Program &program, bool parallel = true);
// tests builds a test module: its entry is the generated runner and Main, if
// any, is compiled but not called. It requires entry = false.
std::string compileModule(const Program &program, const Program &imports,
                          bool entry, bool parallel = true, bool tests = false);
// Moves the program's T{ } tests in among its functions and rejects duplicates.
void enableTests(Program &program);
} // namespace duck

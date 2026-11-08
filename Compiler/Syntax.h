#pragma once
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace duck {
struct Token {
  std::string text;
  int line, column;
  bool string = false;
  std::string source;
  bool character = false; // A 'x' literal; also marked string so it is never punctuation.
  // Set on tokens copied from a macro body. `mark` is unique per expansion
  // and keeps the names that body declares apart from every other scope.
  unsigned mark = 0;
  std::shared_ptr<const struct Expansion> origin;
};
// One expansion of a macro: where it was called and where it was defined.
struct Expansion {
  Token call;
  std::string macro; // Qualified name.
  Token definition;
  unsigned mark = 0;
};
// "file:line:column" of a token, for diagnostics that point at a second place.
inline std::string where(const Token &t) {
  return (t.source.empty() ? "<input>" : t.source) + ":" + std::to_string(t.line) + ":" +
         std::to_string(t.column);
}
// A failure inside macro output names every expansion that produced the token.
[[noreturn]] inline void fail(const Token &t, const std::string &message) {
  std::string text = where(t) + ": error: " + message;
  for (const auto *e = t.origin.get(); e; e = e->call.origin.get())
    text += "\n  note: in expansion of macro '" + e->macro + "' defined at " + where(e->definition) +
            ", called at " + where(e->call);
  throw std::runtime_error(text);
}
struct FuncSignature;
bool sameSignature(const FuncSignature *a, const FuncSignature *b);
struct Type {
  enum Kind {
    Void, Int8, Short, Int, Long, UInt8, UInt16, UInt32, UInt64,
    Float, Double, Bool, Text, String, Name, Record,
    Char, Date, Time, DateTime,
    IntN, UIntN, // Custom byte-width iN/uN; `bits` holds 8*N.
    Big, UBig,   // Arbitrary-precision integers (reference-counted storage).
    Tiny, UTiny, // 69-digit decimal floating point values.
    Enum,        // A #@ enum member; `record` holds the enum name.
    Queue, Stack, Pair, // Collections; `element` (and a pair's `key`) hold their types.
    Array,              // `element` holds the element type; `extent` is 0 when dynamic.
    Func                // A callable; `signature` is null until inferred from its initializer.
  };
  enum Role { Raw, Owner, Alias };
  Role role = Raw;
  Kind base;
  unsigned depth = 0;
  unsigned bits = 0; // Only for IntN and UIntN.
  std::string record;
  // A collection's element type, or a pair's value type, and a pair's key type.
  std::shared_ptr<const Type> element, key;
  std::shared_ptr<const FuncSignature> signature;
  unsigned long long extent = 0; // A fixed array's element count.
  // `T [50*]` and `T [*]`: how many elements a dynamic array is expected to
  // hold. Only the scheduler's cost model reads it, so it is not part of the
  // type's identity and never limits how far the array can grow.
  unsigned long long sizeHint = 0;
  static constexpr unsigned long long HugeHint = ~0ull; // `T [*]`.
  Type(Kind kind = Void) : base(kind) {}
  static Type array(const Type &element, unsigned long long extent) {
    Type t(Array);
    t.element = std::make_shared<const Type>(element);
    t.extent = extent;
    return t;
  }
  // `T [-][-]`: a sparse grid of int coordinates. It is stored as a pair
  // keyed by an [N x int] coordinate tuple, with extent holding N.
  static Type grid(const Type &element, unsigned rank) {
    Type t = collection(Pair, element, array(Int, rank));
    t.extent = rank;
    return t;
  }
  static Type collection(Kind kind, const Type &element, const Type &key = Type()) {
    Type t(kind);
    t.element = std::make_shared<const Type>(element);
    if (kind == Pair) t.key = std::make_shared<const Type>(key);
    return t;
  }
  // iN/uN for 1, 2, 4, and 8 bytes are the exact host-width types.
  static Type integer(unsigned width, bool isUnsigned) {
    switch (width) {
    case 8: return isUnsigned ? UInt8 : Int8;
    case 16: return isUnsigned ? UInt16 : Short;
    case 32: return isUnsigned ? UInt32 : Int;
    case 64: return isUnsigned ? UInt64 : Long;
    }
    Type t(isUnsigned ? UIntN : IntN);
    t.bits = width;
    return t;
  }
  Type pointerTo() const { auto t = *this; ++t.depth; return t; }
  Type pointee() const { auto t = *this; --t.depth; t.role = Raw; return t; }
  bool operator==(Kind kind) const { return !depth && base == kind; }
  bool operator!=(Kind kind) const { return !(*this == kind); }
  bool operator==(const Type &t) const {
    auto same = [](const std::shared_ptr<const Type> &a, const std::shared_ptr<const Type> &b) {
      return a == b || (a && b && *a == *b);
    };
    return role == t.role && base == t.base && depth == t.depth && bits == t.bits &&
           record == t.record && extent == t.extent && same(element, t.element) && same(key, t.key) &&
           sameSignature(signature.get(), t.signature.get());
  }
  bool operator!=(const Type &t) const { return !(*this == t); }
  static const Type Pointer;
};
inline const Type Type::Pointer = Type(Type::Void).pointerTo();
// What a func value takes and gives. Indirect calls are checked against it.
struct FuncParameter {
  Type type;
  bool reference = false;
};
struct FuncSignature {
  Type result;
  std::vector<FuncParameter> parameters;
};
inline bool sameSignature(const FuncSignature *a, const FuncSignature *b) {
  if (a == b) return true;
  if (!a || !b || a->result != b->result || a->parameters.size() != b->parameters.size()) return false;
  for (size_t i = 0; i < a->parameters.size(); ++i)
    if (a->parameters[i].type != b->parameters[i].type ||
        a->parameters[i].reference != b->parameters[i].reference)
      return false;
  return true;
}
// A non-owning heap reference held in a field or collection slot: the address
// of the object's control block, or null. It expires when the owner releases
// the object. A slot holding an owner has role Owner instead.
inline bool trackedAlias(const Type &t) { return t.depth == 1 && t.role == Type::Alias; }
// A func value is one word: a reference-counted closure, or null.
inline bool funcValue(const Type &t) { return !t.depth && t.base == Type::Func; }
inline bool recordValue(const Type &t) { return !t.depth && t.base == Type::Record; }
// `T [N]` stores N elements inline; `T []` is a handle to growable heap
// storage, null while empty.
inline bool arrayValue(const Type &t) { return !t.depth && t.base == Type::Array; }
inline bool fixedArray(const Type &t) { return arrayValue(t) && t.extent != 0; }
inline bool dynamicArray(const Type &t) { return arrayValue(t) && t.extent == 0; }
// A queue, stack, or pair: a move-only handle to heap storage, null while empty.
inline bool collectionValue(const Type &t) {
  return !t.depth && (t.base == Type::Queue || t.base == Type::Stack || t.base == Type::Pair);
}
// A sparse signed grid, `T [-]` or `T [-][-]`: a pair keyed by coordinates.
inline bool gridValue(const Type &t) { return !t.depth && t.base == Type::Pair && t.extent != 0; }
// A pair written as `K, V pair`, not a grid.
inline bool pairValue(const Type &t) { return !t.depth && t.base == Type::Pair && t.extent == 0; }
// An enum value is stored as its member's declaration index. Printing uses the
// member name; operators, conversions, and concatenation use its underlying value.
inline bool enumValue(const Type &t) { return !t.depth && t.base == Type::Enum; }
// A markup value holds a tree's top-level elements: a queue of element nodes.
// Being a collection, it is a move-only handle that is null while empty.
inline Type elementType() {
  Type t = Type::Pointer;
  t.record = "$node";
  return t;
}
inline Type markupType() {
  Type t = Type::collection(Type::Queue, elementType());
  t.extent = 1;
  return t;
}
inline bool markupValue(const Type &t) { return !t.depth && t.base == Type::Queue && t.extent == 1; }
// A borrowed view of one element of a markup tree, from GetChildren().
inline bool elementValue(const Type &t) { return t.depth == 1 && t.base == Type::Void && t.record == "$node"; }
// A variable in a named lifetime scope is stored as a slot of two words, its
// arena address and generation. The slot has no cleanup of its own: the scope
// owns the value, so copying or destroying a container never touches it.
inline Type scopeSlotType() { return Type::array(Type::Long, 2); }
inline bool nativeScalar(const Type &t) {
  return t.depth || t == Type::Int || t == Type::UInt32 || t == Type::Long ||
         t == Type::UInt64 || t == Type::Float || t == Type::Double;
}
// Distinct calendar and character value types. They support comparison but
// no arithmetic, and convert to integers or text only explicitly.
inline bool calendarValue(const Type &t) {
  return t == Type::Char || t == Type::Date || t == Type::Time || t == Type::DateTime;
}
inline bool managedText(const Type &t) { return t == Type::String || t == Type::Name; }
inline bool bigInteger(const Type &t) { return t == Type::Big || t == Type::UBig; }
inline bool decimalValue(const Type &t) { return t == Type::Tiny || t == Type::UTiny; }
inline bool arbitraryNumber(const Type &t) { return bigInteger(t) || decimalValue(t); }
// Values whose storage is reference counted and released at scope exit.
inline bool managedValue(const Type &t) { return managedText(t) || bigInteger(t); }
inline bool textType(const Type &t) { return managedText(t) || t == Type::Text; }
inline bool floating(const Type &t) { return t == Type::Float || t == Type::Double; }
inline bool customInteger(const Type &t) {
  return !t.depth && (t.base == Type::IntN || t.base == Type::UIntN);
}
inline bool unsignedInteger(const Type &t) {
  return t == Type::UInt8 || t == Type::UInt16 ||
         t == Type::UInt32 || t == Type::UInt64 || (customInteger(t) && t.base == Type::UIntN);
}
inline int integerWidth(const Type &t) {
  return customInteger(t) ? int(t.bits)
       : t == Type::Int8 || t == Type::UInt8 ? 8
       : t == Type::Short || t == Type::UInt16 ? 16
       : t == Type::Int || t == Type::UInt32 ? 32
       : t == Type::Long || t == Type::UInt64 ? 64 : 0;
}
inline bool integral(const Type &t) { return integerWidth(t) != 0; }
inline bool numeric(const Type &t) { return integral(t) || floating(t); }
inline bool heapValue(const Type &t) { return numeric(t) || t == Type::Bool || recordValue(t); }
// Whether Print accepts a value of this type.
inline bool printable(const Type &t) {
  return t != Type::Void && !t.depth && !recordValue(t) && !arrayValue(t) && !collectionValue(t) && !funcValue(t);
}
inline int integerLiteralBase(const std::string &text) {
  if (text.size() >= 2 && text[0] == '0') {
    if (text[1] == 'x' || text[1] == 'X') return 16;
    if (text[1] == 'b' || text[1] == 'B') return 2;
  }
  return 10;
}
inline Type commonNumeric(Type a, Type b) {
  if (a == Type::Double || b == Type::Double)
    return Type::Double;
  if (a == Type::Float || b == Type::Float)
    return Type::Float;
  if (unsignedInteger(a) == unsignedInteger(b))
    return integerWidth(a) >= integerWidth(b) ? a : b;
  const auto signedType = unsignedInteger(a) ? b : a;
  const auto unsignedType = unsignedInteger(a) ? a : b;
  // Host widths first. Custom byte widths are considered only when an operand
  // already uses one, so uint64 mixed with a signed host type still needs a cast.
  std::vector<int> widths{8, 16, 32, 64};
  if (customInteger(a) || customInteger(b))
    for (int bytes = 9; bytes <= 64; ++bytes) widths.push_back(8 * bytes);
  for (auto width : widths)
    if (width >= integerWidth(signedType) && width > integerWidth(unsignedType))
      return Type::integer(unsigned(width), false);
  return Type::Void; // No lossless common integer type in this bootstrap.
}
// Common type of big/ubig or tiny/utiny with each other or with an integer of
// at most 64 bits. Both operands nonnegative keeps the unsigned type; big and
// tiny never mix, and floating values never mix with either.
inline Type arbitraryCommon(const Type &a, const Type &b) {
  const bool big = bigInteger(a) || bigInteger(b), decimal = decimalValue(a) || decimalValue(b);
  if (big == decimal) return Type::Void;
  auto compatible = [&](const Type &t) {
    return (big ? bigInteger(t) : decimalValue(t)) || (integral(t) && integerWidth(t) <= 64);
  };
  if (!compatible(a) || !compatible(b)) return Type::Void;
  auto nonnegative = [](const Type &t) {
    return t == Type::UBig || t == Type::UTiny || unsignedInteger(t);
  };
  const bool unsignedResult = nonnegative(a) && nonnegative(b);
  return big ? (unsignedResult ? Type::UBig : Type::Big)
             : (unsignedResult ? Type::UTiny : Type::Tiny);
}
struct Function;
struct Expr {
  Token token;
  std::string kind;
  Type castType = Type::Void;
  std::vector<std::unique_ptr<Expr>> args;
  std::unique_ptr<Expr> receiver;
  // A lambda's generated function. Shared because field defaults are cloned.
  std::shared_ptr<Function> lambda;
  // A "macroValue": read-only locals for the macro's arguments, in scope for
  // its result in args[0] and evaluated only when the expression is reached.
  std::vector<std::unique_ptr<struct Stmt>> bindings;
  bool addressable() const {
    return kind == "name" || kind == "." || kind == "index" || (kind == "unary" && token.text == "*");
  }
};
struct Stmt {
  Token token;
  std::string kind, name;
  std::string key;      // A foreach's key variable, as in `for k, v : pair`.
  bool reverse = false; // A foreach written `for -x : c`.
  Type type = Type::Void;
  bool constant = false;
  std::unique_ptr<Expr> expr, target;
  std::unique_ptr<Stmt> initializer, step;
  std::vector<std::unique_ptr<Stmt>> body, alternate;
  // A "switch" holds its selector as the hidden local `initializer`, reads it
  // back through `target`, runs macro argument bindings from `alternate`, and
  // has one "case" statement per section in `body`. A section's labels are
  // here; stacked labels and `default` share one section.
  std::vector<std::unique_ptr<Expr>> labels;
  bool isDefault = false;
};
// A `using Name.Space` directive. It applies to the declarations after it in
// the file scope or static body it appears in, and in their nested bodies.
// `level` is that scope's qualified container ("" at file scope); `target`
// is the imported container's qualified name.
struct Using {
  Token token;
  std::string level, target;
};
// The directives visible at a declaration, outermost first. Shared because
// every declaration in a scope sees the same list.
using Usings = std::shared_ptr<const std::vector<Using>>;
// One `[Name]` attribute. `type` and `member` name the enum member it
// resolves to, filled in by resolveAttributes.
struct Tag {
  Token token;
  std::string type;
  size_t member = 0;
};
struct Parameter {
  Token name;
  Type type;
  bool reference = false;
  bool protectedAccess = false; // A container field declared with '-'.
  // A synthesized leading field holding a parent's state, named after the
  // parent. Protected, so `this.Parent` is reachable only from derived code.
  bool parentView = false;
  // A container field's default, a compile-time constant. Shared so field
  // lists stay copyable; initialValue is its LLVM constant once resolved,
  // or empty for a string/name literal emitted from the expression.
  std::shared_ptr<Expr> initializer;
  std::string initialValue;
  // The default is not a compile-time constant; a generated method
  // evaluates it during construction.
  bool runtimeDefault = false;
  Usings usings; // Visible where a field default was written.
  std::vector<Tag> tags;
  // A func field whose default is a lambda: its body runs against the object
  // it is called on, so it is called through a receiver and never read.
  bool memberLambda = false;
  // `[@Play]`: the scope holding this field, and its declared type. `type`
  // is then the slot stored in the container.
  std::string scope;
  Type scopedType = Type::Void;
};
// Instance methods live in their own namespace, so a chained static block
// can declare a function with the same name: Meter.Read is static and
// Meter.$Read is the instance method, chosen by the receiver's kind.
inline std::string methodKey(const std::string &record, const std::string &member) {
  return record + ".$" + member;
}
// The member name of a qualified function key, without the method marker.
inline std::string memberName(const std::string &key) {
  auto member = key.substr(key.rfind('.') + 1);
  return !member.empty() && member[0] == '$' ? member.substr(1) : member;
}
struct Function {
  Token name;
  // Mutable so an untyped property's accessors can take the type inferred
  // from its getter after parsing (see resolveProperties).
  mutable Type result;
  mutable std::vector<Parameter> params;
  std::vector<std::unique_ptr<Stmt>> body;
  bool method = false;
  std::string nativeSymbol; // Nonempty for a bodyless extern "C" declaration.
  bool protectedAccess = false; // A container member declared with '-'.
  // Generated by the compiler to evaluate a default or static initializer.
  // Its fields are not bound as locals, so defaults cannot read them.
  bool generated = false;
  std::string initializes; // The static field a generated $eval assigns.
  Usings usings;
  // A property accessor written without a type: a getter's result, or a
  // setter's value parameter, comes from the property's getter expression.
  mutable bool inferred = false;
  // The hidden function holding a file's executable statements, run by Exec.
  bool script = false;
  std::vector<Tag> tags;
  // A test from a T{ } block: its stable name, as in Container.Test. Empty
  // for every other function.
  std::string testName;
};
struct Record {
  Token name;
  std::vector<Parameter> fields;
  std::vector<Token> parents; // In priority order, leftmost first.
  bool imported = false;      // Defined by an imported module.
  // Direct children of this container's `this{ }` block, by type. When
  // closed, nothing else can add children, so lookups are checked statically.
  std::map<std::string, int> markupChildren;
  bool markupClosed = false;
};
struct EnumMember {
  Token name;
  std::unique_ptr<Expr> value; // Null for an implicit value.
};
// One `Name [: type] #@{ ... }` block. Fragments with the same name merge in
// input order before implicit values are assigned.
struct EnumFragment {
  Token name;
  Type type = Type::Void; // Void when this fragment omits the underlying type.
  std::vector<EnumMember> members;
  bool attribute = false; // Marked [args], so members can be used as attributes.
};
// A field declared in a static block, as in `Math #{ int Count = 0 }`. The
// name is qualified by its enclosing containers, like a static function.
struct StaticField {
  Token name;
  Type type;
  bool constant = false;
  bool protectedAccess = false;
  std::unique_ptr<Expr> initializer; // Null for zero initialization.
  Usings usings;
  std::string scope; // As for a Parameter's scope: `type` is then its slot.
  Type scopedType = Type::Void;
};
struct Program : std::vector<Function> {
  std::vector<Record> records;
  std::vector<EnumFragment> enums;
  std::vector<StaticField> statics;
  std::vector<std::string> sources; // Source names in input order.
  // T{ } tests. They stay out of the program unless enableTests moves them in,
  // so an ordinary build compiles exactly what it would without them.
  std::vector<Function> tests;
  // Containers used as markup tags that hold children, which need child
  // storage, and those holding a literal value, which need a `value` field.
  std::vector<Token> markupHosts;
  struct MarkupValue {
    Token tag;
    Type type;
  };
  std::vector<MarkupValue> markupValues;
};
} // namespace duck

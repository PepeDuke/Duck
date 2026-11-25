void resolveInheritance(Symbols &symbols) {
  std::set<std::string> active, complete;

[] maybe we should have table that tracks unique container names so we do not have to use std::string for name references


const std::string &text = tag.token.text;
const auto dot = text.rfind('.'); 
 [] it is after parse should this not be token? we do lot of eachech inside the tokens by text we want to do that only if it is 100 needed

struct Tag {
  Token token;
  std::string type;
  size_t member = 0;
};
[] std::string type; cant we have this in enum?


 // The braces of a body written in source, so an interface file can drop
  // the bodies importers never read. Empty text when there are none.
  Token bodyOpen{}, bodyClose{};
[] do we need this? it seems useless.


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
[] std::string scope; does it need to be string? it seems kinda starange it should be number + type or index to table?


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

[] std::string scope; same thing here
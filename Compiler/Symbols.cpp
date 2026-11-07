#include "Pipeline.h"
#include <regex>

namespace duck {
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
  for (const auto &record : program.records)
    if (!symbols.records.emplace(record.name.text, record).second)
      fail(record.name, "duplicate container '" + record.name.text + "'");
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
            (p.reference && textType(p.type)))
          fail(p.name, "unsupported native parameter ABI; use 32/64-bit values, numeric references, pointers, pointer references, or text by value");
      }
    }
    if (f.name.text == "Print" || f.name.text == "Length" || f.name.text == "CodeUnitAt" ||
        !symbols.emplace(f.name.text, Signature{f.result, f.params, f.nativeSymbol, f.name, {}, f.method}).second)
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
namespace {
void validateLayout(const Symbols &symbols, const Record &record,
                    std::set<std::string> &active, std::set<std::string> &complete) {
  if (complete.count(record.name.text)) return;
  active.insert(record.name.text);
  for (const auto &field : record.fields) {
    symbols.validateType(field.type, field.name);
    if (recordValue(field.type)) {
      if (active.count(field.type.record)) fail(field.name, "recursive container field requires a pointer");
      validateLayout(symbols, symbols.records.at(field.type.record), active, complete);
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
} // namespace duck

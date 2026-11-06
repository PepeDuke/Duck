#include "Pipeline.h"
#include <regex>

namespace duck {
Symbols collectSymbols(const Program &program, bool requireEntry) {
  Symbols symbols;
  for (const auto &f : program) {
    if (!f.nativeSymbol.empty()) {
      if (f.name.text == "Main")
        fail(f.name, "Main cannot be a native declaration");
      if (!std::regex_match(f.nativeSymbol, std::regex("[A-Za-z_][A-Za-z0-9_]*")) ||
          f.nativeSymbol == "main" || f.nativeSymbol == "printf" ||
          f.nativeSymbol == "malloc" || f.nativeSymbol == "free" || f.nativeSymbol == "putchar")
        fail(f.name, "invalid or reserved native symbol '" + f.nativeSymbol + "'");
      auto supportedValue = [](Type type) {
        return type == Type::Void || type == Type::Pointer || floating(type) ||
               (integral(type) && integerWidth(type) >= 32);
      };
      if (!supportedValue(f.result))
        fail(f.name, "native return requires void, void*, float, double, or a 32/64-bit integer");
      for (const auto &p : f.params) {
        if (p.type == Type::Bool ||
            (!p.reference && !textType(p.type) && !supportedValue(p.type)) ||
            (p.reference && textType(p.type)))
          fail(p.name, "unsupported native parameter ABI; use 32/64-bit values, numeric references, void*, void*&, or text by value");
      }
    }
    if (f.name.text == "Print" || f.name.text == "Length" || f.name.text == "CodeUnitAt" ||
        !symbols.emplace(f.name.text, Signature{f.result, f.params, f.nativeSymbol, f.name}).second)
      fail(f.name, "duplicate or reserved function '" + f.name.text + "'");
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

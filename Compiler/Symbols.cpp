#include "Pipeline.h"

namespace duck {
Symbols collectSymbols(const Program &program, bool requireEntry) {
  Symbols symbols;
  for (const auto &f : program) {
    if (f.name.text == "Print" ||
        !symbols.emplace(f.name.text, Signature{f.result, f.params}).second)
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
  return symbols;
}
} // namespace duck

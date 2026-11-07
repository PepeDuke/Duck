#include "Pipeline.h"

namespace duck {
Program combineUnits(std::vector<Program> units) {
  Program program;
  size_t size = 0;
  for (const auto &unit : units)
    size += unit.size();
  program.reserve(size);
  for (auto &unit : units) {
    for (auto &record : unit.records) program.records.push_back(std::move(record));
    for (auto &function : unit)
      program.push_back(std::move(function));
  }
  return program;
}
std::string assembleModule(const std::vector<FunctionIR> &functions,
                           bool entry) {
  std::string module =
      "; Duck bootstrap LLVM IR\ndeclare i32 @printf(ptr, ...)\n"
      "declare double @llvm.floor.f64(double)\ndeclare void @llvm.trap()\n";
  bool needsText = false;
  for (const auto &f : functions) needsText = needsText || f.usesTextRuntime;
  bool needsHeap = false;
  for (const auto &f : functions) needsHeap = needsHeap || f.usesHeapRuntime;
  if (needsText || needsHeap) module += "declare ptr @malloc(i64)\ndeclare void @free(ptr)\n";
  if (needsHeap) module += heapRuntimeIR();
  if (needsText) module += textRuntimeIR();
  for (const auto &f : functions)
    module += f.globals;
  module += '\n';
  for (const auto &f : functions)
    module += f.definition;
  if (!entry)
    return module;
  return module + "define i32 @main() {\nentry:\n  call void @duck.Main()\n  "
                  "ret i32 0\n}\n";
}
std::string compile(const Program &program) {
  return compileModule(program, {}, true);
}
std::string compileModule(const Program &program, const Program &imports,
                          bool entry) {
  auto symbols = collectSymbols(program, entry);
  if (!entry && symbols.count("Main"))
    for (const auto &f : program)
      if (f.name.text == "Main")
        fail(f.name, "library module cannot define Main");
  const auto imported = collectSymbols(imports, false);
  for (const auto &[name, record] : imported.records)
    if (!symbols.records.emplace(name, record).second) fail(record.name, "duplicate container '" + name + "'");
  for (const auto &f : imports) {
    if (f.name.text == "Main")
      fail(f.name, "cannot import an executable module");
    if (!symbols.emplace(f.name.text, imported.at(f.name.text)).second)
      fail(f.name, "duplicate imported function '" + f.name.text + "'");
  }
  validateLayouts(symbols);
  for (const auto &[name, signature] : imported) {
    symbols.validateType(signature.result, signature.declaration);
    for (const auto &parameter : signature.parameters)
      symbols.validateType(parameter.type, parameter.name);
  }
  validateNativeSymbols(symbols);
  auto declarations = imported;
  for (const auto &[name, signature] : symbols)
    if (!signature.nativeSymbol.empty()) declarations.emplace(name, signature);
  std::vector<FunctionAnalysis> analyses;
  for (const auto &function : program)
    analyses.push_back(analyzeFunction(function, symbols));
  std::vector<FunctionIR> fragments;
  for (size_t i = 0; i < program.size(); ++i)
    fragments.push_back(lowerFunction(program[i], symbols, analyses[i]));
  return emitRecords(symbols) + emitDeclarations(declarations) + assembleModule(fragments, entry);
}
} // namespace duck

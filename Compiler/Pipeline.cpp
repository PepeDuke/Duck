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
    for (auto &fragment : unit.enums) program.enums.push_back(std::move(fragment));
    for (auto &field : unit.statics) program.statics.push_back(std::move(field));
    for (auto &source : unit.sources) program.sources.push_back(std::move(source));
    for (auto &function : unit)
      program.push_back(std::move(function));
  }
  return program;
}
std::string assembleModule(const std::vector<FunctionIR> &functions,
                           bool entry, const std::set<std::string> &nativeSymbols) {
  std::string module =
      "; Duck bootstrap LLVM IR\ndeclare i32 @printf(ptr, ...)\n"
      "declare double @llvm.floor.f64(double)\ndeclare void @llvm.trap()\n";
  bool needsText = false, needsBig = false, needsDecimal = false, needsCollections = false;
  for (const auto &f : functions) {
    needsBig = needsBig || f.usesBigRuntime;
    needsDecimal = needsDecimal || f.usesDecimalRuntime;
    needsCollections = needsCollections || f.usesCollectionRuntime;
  }
  // big storage shares the text reference count and the wide digit printer;
  // collections release text and big elements through it.
  for (const auto &f : functions) needsText = needsText || f.usesTextRuntime || needsBig || needsCollections;
  bool needsHeap = false;
  for (const auto &f : functions) needsHeap = needsHeap || f.usesHeapRuntime;
  if (needsText || needsHeap) module += "declare ptr @malloc(i64)\ndeclare void @free(ptr)\n";
  if (needsHeap) module += heapRuntimeIR();
  bool needsTasks = false;
  for (const auto &f : functions) needsTasks = needsTasks || f.usesTaskRuntime;
  if (needsTasks) module += taskRuntimeIR(nativeSymbols);
  if (needsText) module += textRuntimeIR();
  if (needsCollections) module += collectionRuntimeIR();
  if (needsCollections && needsBig) module += collectionBigKeyIR();
  bool needsCalendar = false;
  for (const auto &f : functions) needsCalendar = needsCalendar || f.usesCalendarRuntime;
  if (needsCalendar) module += calendarRuntimeIR();
  bool needsWide = needsBig;
  for (const auto &f : functions) needsWide = needsWide || f.usesWideRuntime;
  if (needsWide) module += wideRuntimeIR();
  if (needsBig) module += bigRuntimeIR();
  if (needsDecimal) module += decimalRuntimeIR();
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
std::string compile(const Program &program, bool parallel) {
  return compileModule(program, {}, true, parallel);
}
std::string compileModule(const Program &program, const Program &imports,
                          bool entry, bool parallel) {
  // Each module emits its own member tables, so only the defining module may
  // add members; otherwise indices from the two modules would disagree.
  for (const auto &fragment : program.enums)
    for (const auto &imported : imports.enums)
      if (fragment.name.text == imported.name.text)
        fail(fragment.name, "cannot extend imported enum '" + fragment.name.text +
                                "'; add members in its defining module");
  auto symbols = collectSymbols(program, entry);
  if (!entry && symbols.count("Main"))
    for (const auto &f : program)
      if (f.name.text == "Main")
        fail(f.name, "library module cannot define Main");
  auto imported = collectSymbols(imports, false);
  // Only the defining module may add instance members; an importer that
  // extended the container would disagree about its layout.
  for (auto [name, record] : imported.records)
    if (record.imported = true; !symbols.records.emplace(name, record).second)
      for (const auto &fragment : program.records)
        if (fragment.name.text == name)
          fail(fragment.name, "cannot extend imported container '" + name +
                                  "'; add instance members in its defining module");
  for (const auto &f : imports) {
    if (f.name.text == "Main")
      fail(f.name, "cannot import an executable module");
    if (!symbols.emplace(f.name.text, imported.at(f.name.text)).second)
      fail(f.name, "duplicate imported function '" + f.name.text + "'");
  }
  for (const auto &[name, info] : imported.enums)
    if (symbols.records.count(name) || !symbols.enums.emplace(name, info).second)
      fail(info.declaration, "duplicate type name '" + name + "'");
  for (auto [name, info] : imported.statics) {
    info.imported = true;
    if (symbols.count(name) || !symbols.statics.emplace(name, info).second)
      fail(info.declaration, "duplicate static field '" + name + "'");
  }
  resolveInheritance(symbols);
  resolveProperties(symbols, {&program, &imports}, {&symbols, &imported});
  validateOverrides(symbols);
  validateLayouts(symbols);
  validateLifetimes(symbols);
  for (const auto &[name, info] : symbols.statics) symbols.validateType(info.type, info.declaration);
  resolveFieldDefaults(symbols);
  resolveStaticInitializers(symbols);
  for (const auto &[name, signature] : imported) {
    symbols.validateType(signature.result, signature.declaration);
    for (const auto &parameter : signature.parameters)
      symbols.validateType(parameter.type, parameter.name);
  }
  validateNativeSymbols(symbols);
  auto declarations = imported;
  for (const auto &[name, signature] : symbols)
    if (!signature.nativeSymbol.empty()) declarations.emplace(name, signature);
  const auto generated = generateInitializers(symbols, declarations);
  std::vector<FunctionAnalysis> analyses;
  for (const auto &function : program)
    analyses.push_back(analyzeFunction(function, symbols));
  if (parallel) {
    // Imported bodies are not visible, so their effects stay unknown.
    const auto isolated = isolatedFunctions(program, analyses);
    for (size_t i = 0; i < program.size(); ++i)
      scheduleTasks(program[i], isolated, analyses[i]);
  }
  std::set<std::string> nativeSymbols;
  for (const auto &[name, signature] : declarations) {
    (void)name;
    if (!signature.nativeSymbol.empty()) nativeSymbols.insert(signature.nativeSymbol);
  }
  std::vector<FunctionIR> fragments;
  for (size_t i = 0; i < program.size(); ++i)
    fragments.push_back(lowerFunction(program[i], symbols, analyses[i]));
  // Generated initializers run sequentially; they are never task candidates.
  for (const auto &function : generated)
    fragments.push_back(lowerFunction(function, symbols, analyzeFunction(function, symbols)));
  std::set<std::string> cleanups, destroys;
  std::set<DispatchTable> tables;
  std::map<std::string, std::string> helpers;
  for (const auto &fragment : fragments) {
    helpers.insert(fragment.helpers.begin(), fragment.helpers.end());
    tables.insert(fragment.tables.begin(), fragment.tables.end());
    cleanups.insert(fragment.cleanups.begin(), fragment.cleanups.end());
    destroys.insert(fragment.destroys.begin(), fragment.destroys.end());
  }
  // A destructible heap payload's cleanup destroys it first, and a destroy
  // routine frees dynamic arrays whose container elements use their cleanup.
  for (bool changed = true; changed;) {
    changed = false;
    for (const auto &record : cleanups)
      if (symbols.destructible(record)) changed |= destroys.insert(record).second;
    std::set<std::string> pending = destroys;
    while (!pending.empty()) {
      const auto record = *pending.begin();
      pending.erase(pending.begin());
      for (const auto &field : symbols.records.at(record).fields) {
        if (recordValue(field.type) && symbols.destructible(field.type.record) &&
            destroys.insert(field.type.record).second) {
          pending.insert(field.type.record);
          changed = true;
        }
        if (dynamicArray(field.type) && recordValue(*field.type.element) &&
            symbols.needsCleanup(*field.type.element))
          changed |= cleanups.insert(field.type.element->record).second;
      }
    }
  }
  std::string helperIR;
  for (const auto &[symbol, definition] : helpers) {
    (void)symbol;
    helperIR += definition;
  }
  return helperIR + emitRecords(symbols) + emitEnums(symbols) + emitStatics(symbols) + emitCleanups(symbols, cleanups) +
         emitDestroys(symbols, destroys) + emitTables(symbols, tables) +
         emitDeclarations(declarations) + assembleModule(fragments, entry, nativeSymbols);
}
} // namespace duck

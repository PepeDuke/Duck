#include "Pipeline.h"
#include <algorithm>

namespace duck {
Program combineUnits(std::vector<Program> units) {
  Program program;
  size_t size = 0, scripts = 0;

  for (const auto &unit : units)
    size += unit.size();

  program.reserve(size);

  for (auto &unit : units) {
    for (auto &record : unit.records) program.records.push_back(std::move(record));
    for (auto &fragment : unit.enums) program.enums.push_back(std::move(fragment));
    for (auto &field : unit.statics) program.statics.push_back(std::move(field));
    for (auto &source : unit.sources) program.sources.push_back(std::move(source));
    for (auto &test : unit.tests) program.tests.push_back(std::move(test));
    for (auto &host : unit.markupHosts) program.markupHosts.push_back(std::move(host));
    for (auto &value : unit.markupValues) program.markupValues.push_back(std::move(value));

    for (auto &function : unit) {
      if (function.script) function.name.text = "$exec." + std::to_string(scripts++);

      program.push_back(std::move(function));
    }
  }

  return program;
}
void enableTests(Program &program) {
  std::map<std::string, Token> seen;

  for (auto &test : program.tests) {
    const auto [existing, added] = seen.emplace(test.testName, test.name);

    if (!added) fail(test.name, "test '" + test.testName + "' is already declared at " + where(existing->second));

    program.push_back(std::move(test));
  }

  program.tests.clear();
}
std::string assembleModule(const std::vector<FunctionIR> &functions,
                           bool entry, const std::set<std::string> &nativeSymbols,
                           bool mainArguments) {
  std::string module =
      "; Duck bootstrap LLVM IR\ndeclare i32 @printf(ptr, ...)\n"
      "declare double @llvm.floor.f64(double)\ndeclare void @llvm.trap()\n";
  bool needsText = false, needsBig = false, needsDecimal = false, needsCollections = mainArguments;
  bool needsFunc = false, needsMeta = false, needsMarkup = false, needsScope = false;

  for (const auto &f : functions) needsScope = needsScope || f.usesScopeRuntime;

  for (const auto &f : functions) {
    needsMarkup = needsMarkup || f.usesMarkupRuntime;
    needsFunc = needsFunc || f.usesFuncRuntime;
    needsMeta = needsMeta || f.usesMetaRuntime;
  }

  needsCollections = needsCollections || needsMarkup;

  for (const auto &f : functions) {
    needsBig = needsBig || f.usesBigRuntime;
    needsDecimal = needsDecimal || f.usesDecimalRuntime;
    needsCollections = needsCollections || f.usesCollectionRuntime;
  }

  // big storage shares the text reference count and the wide digit printer;
  // collections release text and big elements through it.
  for (const auto &f : functions) needsText = needsText || f.usesTextRuntime || needsBig || needsCollections;

  bool needsHeap = needsMarkup;

  for (const auto &f : functions) needsHeap = needsHeap || f.usesHeapRuntime;
  if (needsText || needsHeap || needsFunc || needsScope) module += "declare ptr @malloc(i64)\ndeclare void @free(ptr)\n";

  if (needsScope) {
    module += scopeRuntimeIR(nativeSymbols);
    std::set<std::string> names;

    for (const auto &f : functions) names.insert(f.scopeNames.begin(), f.scopeNames.end());

    for (const auto &name : names) module += "$duck$scope." + name + " = comdat any\n@duck$scope." + name +
                                          " = linkonce_odr global %duck.scope zeroinitializer, comdat\n";
  }

  if (needsFunc) module += funcRuntimeIR();
  if (needsMeta) module += metaRuntimeIR();
  if (needsHeap) module += heapRuntimeIR();

  bool needsTasks = false;
  bool needsLocks = false;

  for (const auto &f : functions) {
    needsTasks = needsTasks || f.usesTaskRuntime;
    needsLocks = needsLocks || f.usesLockRuntime;
  }

  if (needsTasks || needsLocks) module += lockRuntimeIR(nativeSymbols);
  if (needsTasks) module += taskRuntimeIR(nativeSymbols);
  if (needsText) module += textRuntimeIR();
  if (needsCollections) module += collectionRuntimeIR();
  if (needsMarkup) module += markupRuntimeIR();
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

  if (mainArguments)
    return module + entryArgumentsIR();

  return module + "define i32 @main() {\nentry:\n  call void @duck.Main()\n  "
                  "ret i32 0\n}\n";
}
std::string compile(const Program &program, bool parallel) {
  return compileModule(program, {}, true, parallel);
}
static void rejectImportedEnumExtensions(const Program &program, const Program &imports) {
  for (const auto &fragment : program.enums)
    for (const auto &imported : imports.enums)
      if (fragment.name.text == imported.name.text)
        fail(fragment.name, "cannot extend imported enum '" + fragment.name.text +
                                "'; add members in its defining module");
}
static void mergeImportedSymbols(Symbols &symbols, const Symbols &imported, const Program &program, const Program &imports) {
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
}
static void rejectImportedMarkupStorage(const Symbols &symbols, const Program &program) {
  for (const auto &host : program.markupHosts) {
    const auto found = symbols.records.find(host.text);

    if (found == symbols.records.end() || !found->second.imported) continue;

    if (!hasField(found->second, "$children"))
      fail(host, "'" + host.text + "' is defined in another module and cannot hold children; give it a this{ } block there");
  }

  for (const auto &value : program.markupValues) {
    const auto found = symbols.records.find(value.tag.text);

    if (found == symbols.records.end() || !found->second.imported) continue;

    if (!hasField(found->second, "value"))
      fail(value.tag, "'" + value.tag.text + "' is defined in another module; declare its value field there");
  }
}
static void validateImportedSignatures(const Symbols &symbols, const Symbols &imported) {
  for (const auto &[name, signature] : imported) {
    symbols.validateType(signature.result, signature.declaration);

    for (const auto &parameter : signature.parameters)
      symbols.validateType(parameter.type, parameter.name);
  }
}
static void validateScopedStorage(const Symbols &symbols) {
  auto validateScoped = [&](const Type &type, const Token &at) {
    symbols.validateType(type, at);

    if (type == Type::Void || funcValue(type) || (recordValue(type) && symbols.runtimeDefaults(type.record)) ||
        (fixedArray(type) && symbols.needsCleanup(type)))
      fail(at, typeName(type) + " cannot live in a named scope; its default must be a constant value");
  };

  for (const auto &[name, record] : symbols.records) {
    (void)name;

    for (const auto &field : record.fields)
      if (!field.scope.empty()) validateScoped(field.scopedType, field.name);
  }

  for (const auto &[name, info] : symbols.statics) {
    (void)name;

    if (!info.scope.empty()) validateScoped(info.scopedType, info.declaration);
  }
}
static void markUnwinders(std::vector<FunctionAnalysis> &analyses, const std::vector<const Function *> &evaluatedFunctions, const Program &imports) {
  // Which calls can come back because a callee ran @end: those reaching an
  // @end here, any call into another module, and calls through values, whose
  // targets are unknown. A module with neither @end nor imports has none.
  bool ends = !imports.empty();

  for (const auto &analysis : analyses) ends = ends || analysis.endsScope;

  if (ends) {
    std::set<std::string> importedFunctions;

    for (const auto &f : imports)
      if (f.nativeSymbol.empty()) importedFunctions.insert(f.name.text);

    auto unwinders = std::make_shared<std::set<std::string>>(importedFunctions);

    for (size_t i = 0; i < analyses.size(); ++i)
      if (analyses[i].endsScope || !analyses[i].indirectCalls.empty() || !analyses[i].duckCalls.empty())
        unwinders->insert(evaluatedFunctions[i]->name.text);

    for (bool changed = true; changed;) {
      changed = false;

      for (size_t i = 0; i < analyses.size(); ++i) {
        if (unwinders->count(evaluatedFunctions[i]->name.text)) continue;

        for (const auto &[call, target] : analyses[i].callTargets) {
          (void)call;

          if (unwinders->count(target)) {
            unwinders->insert(evaluatedFunctions[i]->name.text);
            changed = true;
            break;
          }
        }
      }
    }

    for (auto &analysis : analyses) {
      analysis.unwinders = unwinders;
      analysis.unwindIndirect = true;
    }
  }
}
static void rejectUnusedScripts(const Program &program, const std::vector<FunctionAnalysis> &analyses) {
  std::set<std::string> executed;

  for (const auto &analysis : analyses)
    for (const auto &[call, target] : analysis.callTargets) {
      (void)call;
      executed.insert(target);
    }

  for (const auto &function : program)
    if (function.script && !executed.count(function.name.text))
      fail(function.name, "file-scope statement never runs: no Exec in this module targets this "
                          "file; call Exec on the file, or move the statement into a function");
}
static void markFieldRuntimes(const Symbols &symbols, std::vector<FunctionIR> &fragments) {
  for (const auto &[name, record] : symbols.records)
    for (const auto &field : record.fields) {
      Type leaf = field.type;

      while (arrayValue(leaf)) leaf = *leaf.element;
      if (funcValue(leaf) && !fragments.empty()) fragments.front().usesFuncRuntime = true;
      if (markupValue(field.type) && !fragments.empty()) fragments.front().usesMarkupRuntime = true;

      if ((leaf.role != Type::Raw || field.type.role != Type::Raw) && !fragments.empty())
        fragments.front().usesHeapRuntime = true;

      if (collectionValue(leaf) && trackedAlias(*leaf.element) && !fragments.empty())
        fragments.front().usesHeapRuntime = true;
    }
}
static void closeCleanupSets(const Symbols &symbols, std::set<std::string> &cleanups, std::set<std::string> &destroys) {
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

        if (field.type.role == Type::Owner && symbols.needsCleanup(field.type.pointee()))
          changed |= cleanups.insert(field.type.record).second;

        if (dynamicArray(field.type) && recordValue(*field.type.element) &&
            symbols.needsCleanup(*field.type.element))
          changed |= cleanups.insert(field.type.element->record).second;
      }
    }
  }
}
static std::string buildTestRunner(const Program &program, const std::set<std::string> &nativeSymbols) {
  // Sorted so output order does not depend on the order of the input files.
  std::vector<std::pair<std::string, std::string>> cases;

  for (const auto &function : program)
    if (!function.testName.empty()) cases.emplace_back(function.testName, "@duck." + function.name.text);

  std::sort(cases.begin(), cases.end());
  return testRunnerIR(cases, nativeSymbols);
}
std::string compileModule(const Program &program, const Program &imports,
                          bool entry, bool parallel, bool tests) {
  // Each module emits its own member tables, so only the defining module may
  // add members; otherwise indices from the two modules would disagree.
  rejectImportedEnumExtensions(program, imports);
  auto symbols = collectSymbols(program, entry);

  for (const auto &source : program.sources) symbols.sources.insert(sourceKey(source));
  for (const auto &source : imports.sources) symbols.importedSources.insert(sourceKey(source));

  for (const auto &function : program)
    if (function.script) symbols.scripts.emplace(sourceKey(function.name.source), function.name.text);

  if (!entry && !tests && symbols.count("Main"))
    for (const auto &f : program)
      if (f.name.text == "Main")
        fail(f.name, "library module cannot define Main");

  auto imported = collectSymbols(imports, false);
  // Only the defining module may add instance members; an importer that
  // extended the container would disagree about its layout.
  mergeImportedSymbols(symbols, imported, program, imports);
  // Only the defining module may change a container's layout.
  rejectImportedMarkupStorage(symbols, program);
  resolveInheritance(symbols);
  resolveProperties(symbols, {&program, &imports}, {&symbols, &imported});
  validateOverrides(symbols);
  resolveAttributes(symbols);
  validateLayouts(symbols);
  validateLifetimes(symbols);

  for (const auto &[name, info] : symbols.statics) symbols.validateType(info.type, info.declaration);

  resolveFieldDefaults(symbols);
  resolveStaticInitializers(symbols);
  resolveReturnRoles(symbols, {&program, &imports}, {&symbols, &imported});
  validateOverrides(symbols); // Inferred roles are part of a method's signature.
  validateImportedSignatures(symbols, imported);
  validateNativeSymbols(symbols);
  // A scope builds a value from its type's default when it is first used.
  validateScopedStorage(symbols);
  auto declarations = imported;

  for (const auto &[name, signature] : symbols)
    if (!signature.nativeSymbol.empty()) declarations.emplace(name, signature);

  const auto generated = generateInitializers(symbols, declarations);
  std::vector<FunctionAnalysis> analyses;

  for (const auto &function : program)
    analyses.push_back(analyzeFunction(function, symbols));

  std::vector<const Function *> evaluatedFunctions;

  for (const auto &function : program) evaluatedFunctions.push_back(&function);

  for (const auto &function : generated) {
    evaluatedFunctions.push_back(&function);
    analyses.push_back(analyzeFunction(function, symbols));
  }

  evaluateCompileTimeCalls(evaluatedFunctions, analyses);
  markUnwinders(analyses, evaluatedFunctions, imports);
  // Exec paths are literals, so a file nothing runs is known here. Its
  // statements would otherwise be dead code that looks like it executes.
  rejectUnusedScripts(program, analyses);
  {
    // Imported bodies are not visible, so their effects stay unknown.
    const auto effects = summarizeEffects(program, analyses, symbols, entry);

    for (size_t i = 0; i < program.size(); ++i) planOrderedBlocks(program[i], effects, symbols, analyses[i], entry);

    if (parallel)
      for (size_t i = 0; i < program.size(); ++i)
        scheduleTasks(program, i, effects, symbols, analyses, entry);
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
  for (size_t i = 0; i < generated.size(); ++i)
    fragments.push_back(lowerFunction(generated[i], symbols, analyses[program.size() + i]));

  // A heap cleanup can release a func field in a function that never touches one.
  markFieldRuntimes(symbols, fragments);
  std::set<std::string> cleanups, destroys;
  std::set<DispatchTable> tables;
  std::set<std::string> types;
  std::map<std::string, std::string> helpers;

  for (const auto &fragment : fragments) {
    types.insert(fragment.types.begin(), fragment.types.end());
    helpers.insert(fragment.helpers.begin(), fragment.helpers.end());
    tables.insert(fragment.tables.begin(), fragment.tables.end());
    cleanups.insert(fragment.cleanups.begin(), fragment.cleanups.end());
    destroys.insert(fragment.destroys.begin(), fragment.destroys.end());
  }

  // A destructible heap payload's cleanup destroys it first, and a destroy
  // routine frees dynamic arrays whose container elements use their cleanup.
  closeCleanupSets(symbols, cleanups, destroys);
  std::string helperIR;

  for (const auto &[symbol, definition] : helpers) {
    (void)symbol;
    helperIR += definition;
  }

  const std::string runner = tests ? buildTestRunner(program, nativeSymbols) : std::string();
  return emitRecords(symbols) + helperIR + emitEnums(symbols) + emitStatics(symbols) + emitCleanups(symbols, cleanups) +
         emitDestroys(symbols, destroys) + emitTables(symbols, tables, types) +
         emitDeclarations(declarations) +
         assembleModule(fragments, entry, nativeSymbols,
                        entry && !symbols.at("Main").parameters.empty()) +
         runner;
}
} // namespace duck

#pragma once
#include "Syntax.h"
#include <map>
#include <set>

namespace duck {
// Stage artifacts belong to the caller. No stage uses global mutable state.
std::vector<Token> lex(const std::string &source,
                       const std::string &sourceName = "");
// Collects file-scope type names across all units. An enum name is recorded
// both as Name and as #@Name so the parser can tell enums from containers.
void collectContainerNames(const std::vector<Token> &tokens, std::set<std::string> &names);
Program parse(std::vector<Token> tokens);
Program parse(std::vector<Token> tokens, const std::set<std::string> &names);
// Keep units in input order. Combining is a barrier before declaration
// collection.
Program combineUnits(std::vector<Program> units);

struct Signature {
  Type result;
  std::vector<Parameter> parameters;
  std::string nativeSymbol;
  Token declaration;
  std::set<size_t> nonescapingParameters;
  bool method = false;
};
// A merged enum. Values are in member order: signed decimal integers, LLVM
// hexadecimal floating constants, or UTF-8 text, depending on the type.
struct EnumInfo {
  Token declaration;
  Type type;
  std::vector<Token> members;
  std::vector<std::string> values;
  size_t find(const std::string &member) const {
    size_t i = 0;
    while (i < members.size() && members[i].text != member) ++i;
    return i;
  }
};
struct Symbols : std::map<std::string, Signature> {
  std::map<std::string, Record> records;
  std::map<std::string, EnumInfo> enums;
  void validateType(const Type &type, const Token &token) const {
    if (type.base == Type::Record && !records.count(type.record))
      fail(token, "unknown container '" + type.record + "'");
    if (type.base == Type::Enum && !enums.count(type.record))
      fail(token, "unknown enum '" + type.record + "'");
  }
};
std::string typeName(Type type);
// Merges fragments by name in input order, then assigns implicit values.
std::map<std::string, EnumInfo> resolveEnums(const std::vector<EnumFragment> &fragments);
bool borrowsOnly(const Stmt &statement, const std::string &name);
Symbols collectSymbols(const Program &program, bool requireEntry = true);
void validateLayouts(const Symbols &symbols);
void validateNativeSymbols(const Symbols &symbols);

// Expression pointers refer to the input AST, which must outlive this artifact.
// Each function gets its own analysis, suitable for independent worker tasks.
struct FunctionAnalysis {
  std::map<const Expr *, Type> expressionTypes;
  std::map<const Expr *, Type> conversions;
  std::map<const Expr *, size_t> fieldIndices;
  std::map<const Expr *, std::string> integerConstants;
  std::map<const Expr *, std::string> floatingConstants;
  // tiny/utiny literals, already rounded: {signed coefficient, exponent}.
  std::map<const Expr *, std::pair<std::string, std::string>> decimalConstants;
  std::map<const Expr *, std::string> callTargets;
  // Enum member constants such as Day.Monday: the member's index.
  std::map<const Expr *, size_t> enumMembers;
  // Calls resolved to a calendar built-in such as Year or AddDays.
  std::set<const Expr *> calendarCalls;
  std::map<const Stmt *, Type> localTypes;
  std::set<const Stmt *> scopedBorrows;
  std::set<const Expr *> receiverStorage;
  std::set<const Stmt *> terminatingLoops;
  // Filled by scheduleTasks. A spawned declaration runs its call as a worker
  // task; pending tasks join before a statement that uses their name, before a
  // barrier statement, and at the end of their statement list.
  std::set<const Stmt *> taskSpawns;
  std::set<const Stmt *> taskBarriers;
  std::map<const Stmt *, std::set<std::string>> taskUses;
};
FunctionAnalysis analyzeFunction(const Function &function,
                                 const Symbols &symbols);

// Functions whose complete effects are known: plain value parameters and
// results, no external effects, and calls only to other isolated functions.
std::set<std::string> isolatedFunctions(const Program &program,
                                        const std::vector<FunctionAnalysis> &analyses);
void scheduleTasks(const Function &function, const std::set<std::string> &isolated,
                   FunctionAnalysis &analysis);

struct FunctionIR {
  std::string globals;
  std::string definition;
  bool usesTextRuntime = false;
  bool usesHeapRuntime = false;
  bool usesTaskRuntime = false;
  bool usesCalendarRuntime = false;
  bool usesWideRuntime = false;
  bool usesBigRuntime = false;
  bool usesDecimalRuntime = false;
};
FunctionIR lowerFunction(const Function &function, const Symbols &symbols,
                         const FunctionAnalysis &analysis);
// nativeSymbols lists symbols already declared by the program, so runtime
// imports are not declared twice.
std::string assembleModule(const std::vector<FunctionIR> &functions,
                           bool entry = true,
                           const std::set<std::string> &nativeSymbols = {});
std::string textRuntimeIR();
std::string heapRuntimeIR();
std::string calendarRuntimeIR();
std::string wideRuntimeIR();
std::string bigRuntimeIR();
std::string decimalRuntimeIR();
std::string taskRuntimeIR(const std::set<std::string> &nativeSymbols);
std::string emitRecords(const Symbols &symbols);
std::string emitEnums(const Symbols &symbols);
std::string emitDeclarations(const Symbols &symbols);

// Sequential orchestration. Analysis finishes before lowering begins.
// parallel = false keeps every call on the calling thread.
std::string compile(const Program &program, bool parallel = true);
std::string compileModule(const Program &program, const Program &imports,
                          bool entry, bool parallel = true);
} // namespace duck

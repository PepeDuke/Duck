#pragma once
#include "Syntax.h"
#include <map>
#include <set>

namespace duck {
// Stage artifacts belong to the caller. No stage uses global mutable state.
std::vector<Token> lex(const std::string &source,
                       const std::string &sourceName = "");
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
struct Symbols : std::map<std::string, Signature> {
  std::map<std::string, Record> records;
  void validateType(const Type &type, const Token &token) const {
    if (type.base == Type::Record && !records.count(type.record))
      fail(token, "unknown container '" + type.record + "'");
  }
};
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
  std::map<const Expr *, std::string> callTargets;
  std::map<const Stmt *, Type> localTypes;
  std::set<const Stmt *> scopedBorrows;
  std::set<const Expr *> receiverStorage;
  std::set<const Stmt *> terminatingLoops;
};
FunctionAnalysis analyzeFunction(const Function &function,
                                 const Symbols &symbols);

struct FunctionIR {
  std::string globals;
  std::string definition;
  bool usesTextRuntime = false;
  bool usesHeapRuntime = false;
};
FunctionIR lowerFunction(const Function &function, const Symbols &symbols,
                         const FunctionAnalysis &analysis);
std::string assembleModule(const std::vector<FunctionIR> &functions,
                           bool entry = true);
std::string textRuntimeIR();
std::string heapRuntimeIR();
std::string emitRecords(const Symbols &symbols);
std::string emitDeclarations(const Symbols &symbols);

// Sequential orchestration for now. Analysis finishes before lowering begins.
std::string compile(const Program &program);
std::string compileModule(const Program &program, const Program &imports,
                          bool entry);
} // namespace duck

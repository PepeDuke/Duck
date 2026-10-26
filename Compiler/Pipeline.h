#pragma once
#include "Syntax.h"
#include <map>
#include <set>

namespace duck {
// Stage artifacts belong to the caller. No stage uses global mutable state.
std::vector<Token> lex(const std::string &source,
                       const std::string &sourceName = "");
Program parse(std::vector<Token> tokens);
// Keep units in input order. Combining is a barrier before declaration
// collection.
Program combineUnits(std::vector<Program> units);

struct Signature {
  Type result;
  std::vector<Parameter> parameters;
};
using Symbols = std::map<std::string, Signature>;
Symbols collectSymbols(const Program &program, bool requireEntry = true);

// Expression pointers refer to the input AST, which must outlive this artifact.
// Each function gets its own analysis, suitable for independent worker tasks.
struct FunctionAnalysis {
  std::map<const Expr *, Type> expressionTypes;
  std::map<const Expr *, Type> conversions;
  std::map<const Expr *, std::string> integerConstants;
  std::map<const Expr *, std::string> floatingConstants;
  std::map<const Expr *, std::string> callTargets;
  std::set<const Stmt *> terminatingLoops;
};
FunctionAnalysis analyzeFunction(const Function &function,
                                 const Symbols &symbols);

struct FunctionIR {
  std::string globals;
  std::string definition;
};
FunctionIR lowerFunction(const Function &function, const Symbols &symbols,
                         const FunctionAnalysis &analysis);
std::string assembleModule(const std::vector<FunctionIR> &functions,
                           bool entry = true);
std::string emitDeclarations(const Symbols &symbols);

// Sequential orchestration for now. Analysis finishes before lowering begins.
std::string compile(const Program &program);
std::string compileModule(const Program &program, const Program &imports,
                          bool entry);
} // namespace duck

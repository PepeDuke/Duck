#include "Pipeline.h"

namespace duck {
namespace {
// A task may only exchange plain values with its caller. Text, pointers,
// containers, and references would share storage across threads.
bool plainValue(const Type &type) {
  return !type.depth && type.role == Type::Raw &&
         (numeric(type) || type == Type::Bool || enumValue(type));
}
template <class Visit> void visitExpression(const Expr &e, Visit &visit) {
  visit(e);
  if (e.receiver) visitExpression(*e.receiver, visit);
  for (const auto &child : e.args) visitExpression(*child, visit);
}
template <class VisitExpr, class VisitStmt>
void visitStatement(const Stmt &s, VisitExpr &expr, VisitStmt &stmt) {
  stmt(s);
  if (s.expr) visitExpression(*s.expr, expr);
  if (s.target) visitExpression(*s.target, expr);
  if (s.initializer) visitStatement(*s.initializer, expr, stmt);
  if (s.step) visitStatement(*s.step, expr, stmt);
  for (const auto &child : s.body) visitStatement(*child, expr, stmt);
  for (const auto &child : s.alternate) visitStatement(*child, expr, stmt);
}
bool candidate(const Function &f) {
  if (!f.nativeSymbol.empty() || f.method || !plainValue(f.result)) return false;
  for (const auto &p : f.params)
    if (p.reference || !plainValue(p.type)) return false;
  return true;
}

class Scheduler {
  const std::set<std::string> &isolated;
  FunctionAnalysis &analysis;

  // An external effect, or leaving the sequence, must observe every earlier
  // task as finished. This keeps output, traps, and nontermination in source order.
  bool barrier(const Stmt &s) const {
    bool found = false;
    auto expr = [&](const Expr &e) {
      if (e.kind != "call") return;
      if (e.token.text == "Print") { found = true; return; }
      auto target = analysis.callTargets.find(&e);
      if (target != analysis.callTargets.end() && !isolated.count(target->second)) found = true;
    };
    auto stmt = [&](const Stmt &child) {
      if (child.kind == "break" || child.kind == "continue" || child.kind == "return" ||
          child.kind == "returnOwner" || analysis.terminatingLoops.count(&child))
        found = true;
    };
    visitStatement(s, expr, stmt);
    return found;
  }
  static std::set<std::string> names(const Stmt &s) {
    std::set<std::string> used;
    auto expr = [&](const Expr &e) { if (e.kind == "name") used.insert(e.token.text); };
    auto stmt = [](const Stmt &) {};
    visitStatement(s, expr, stmt);
    return used;
  }
  bool spawnable(const Stmt &s) const {
    if (s.kind != "let" || !s.expr || s.expr->kind != "call" || s.expr->receiver) return false;
    auto target = analysis.callTargets.find(s.expr.get());
    return target != analysis.callTargets.end() && isolated.count(target->second) &&
           plainValue(analysis.localTypes.at(&s));
  }
  void list(const std::vector<std::unique_ptr<Stmt>> &statements) {
    for (const auto &s : statements) nested(*s);
    bool spawned = false;
    for (size_t i = 0; i + 1 < statements.size(); ++i) {
      const auto &s = *statements[i];
      if (!spawnable(s)) continue;
      // Starting a task that the next statement immediately joins only adds overhead.
      const auto &next = *statements[i + 1];
      if (barrier(next) || names(next).count(s.name)) continue;
      analysis.taskSpawns.insert(&s);
      spawned = true;
    }
    if (!spawned) return;
    for (const auto &s : statements) {
      if (barrier(*s)) analysis.taskBarriers.insert(s.get());
      analysis.taskUses[s.get()] = names(*s);
    }
  }
  void nested(const Stmt &s) {
    // Only statement lists spawn; for-initializers and steps run in place.
    if (!s.body.empty()) list(s.body);
    if (!s.alternate.empty()) list(s.alternate);
  }

public:
  Scheduler(const std::set<std::string> &isolated, FunctionAnalysis &analysis)
      : isolated(isolated), analysis(analysis) {}
  void run(const Function &function) { list(function.body); }
};
} // namespace

std::set<std::string> isolatedFunctions(const Program &program,
                                        const std::vector<FunctionAnalysis> &analyses) {
  std::map<std::string, size_t> index;
  std::set<std::string> isolated;
  for (size_t i = 0; i < program.size(); ++i) {
    const auto &f = program[i];
    if (!candidate(f)) continue;
    const auto &a = analyses[i];
    bool known = true;
    for (const auto &[expr, type] : a.expressionTypes)
      // Builtins and construction have no call target; Print is an external effect.
      known = known && plainValue(type) && (expr->kind != "call" || a.callTargets.count(expr));
    for (const auto &[stmt, type] : a.localTypes) {
      (void)stmt;
      known = known && plainValue(type);
    }
    if (!known) continue;
    isolated.insert(f.name.text);
    index[f.name.text] = i;
  }
  // Greatest fixpoint: recursive groups stay isolated unless some member
  // reaches a function with unknown or external effects.
  for (bool changed = true; changed;) {
    changed = false;
    for (auto it = isolated.begin(); it != isolated.end();) {
      bool known = true;
      for (const auto &[expr, target] : analyses[index.at(*it)].callTargets) {
        (void)expr;
        known = known && isolated.count(target);
      }
      if (known) ++it;
      else {
        it = isolated.erase(it);
        changed = true;
      }
    }
  }
  return isolated;
}

void scheduleTasks(const Function &function, const std::set<std::string> &isolated,
                   FunctionAnalysis &analysis) {
  if (function.nativeSymbol.empty()) Scheduler(isolated, analysis).run(function);
}
} // namespace duck

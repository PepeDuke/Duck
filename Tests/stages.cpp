#include "Pipeline.h"
#include <future>
#include <iostream>

namespace {
void check(bool value, const std::string &message) {
  if (!value)
    throw std::runtime_error(message);
}
template <class Work> void rejects(Work work, const std::string &message) {
  try {
    work();
  } catch (const std::runtime_error &error) {
    check(std::string(error.what()).find(message) != std::string::npos,
          "unexpected diagnostic: " + std::string(error.what()));
    return;
  }
  throw std::runtime_error("expected rejection: " + message);
}
} // namespace
int main() {
  try {
    const auto tokens = duck::lex("// comment\nMain() { Print(\"text\") }");
    check(tokens.size() > 1 && tokens[0].text == "\n" &&
              tokens[1].text == "Main" && tokens[1].line == 2 &&
              tokens[1].column == 1,
          "token source locations must survive tokenization");
    rejects([] { duck::lex("/* unterminated"); }, "unterminated comment");
    rejects([] { duck::parse(duck::lex("Main() {")); }, "expected '}'");

    auto invalid = duck::parse(duck::lex("Main() { int n = true }"));
    const auto invalidSymbols = duck::collectSymbols(invalid);
    rejects([&] { duck::analyzeFunction(invalid.front(), invalidSymbols); },
            "type mismatch");
    auto uninitialized = duck::parse(duck::lex("Main() { int n; Print(n) }"));
    const auto uninitializedSymbols = duck::collectSymbols(uninitialized);
    rejects(
        [&] {
          duck::analyzeFunction(uninitialized.front(), uninitializedSymbols);
        },
        "read before assignment");
    auto duplicate = duck::parse(duck::lex("Main() {}\nMain() {}"));
    rejects([&] { duck::collectSymbols(duplicate); },
            "duplicate or reserved function");
    std::vector<duck::Program> units;
    units.push_back(
        duck::parse(duck::lex("Main() { Print(Helper()) }", "main.dk")));
    units.push_back(
        duck::parse(duck::lex("int Helper() { return 4 }", "helper.dk")));
    auto combined = duck::combineUnits(std::move(units));
    check(combined[1].name.source == "helper.dk",
          "combining lost source identity");
    check(duck::compile(combined) ==
              duck::compile(duck::parse(duck::lex(
                  "Main() { Print(Helper()) }\nint Helper() { return 4 }"))),
          "unit boundaries changed IR");
    rejects([] { duck::lex("\n/* broken", "lexical.dk"); }, "lexical.dk:2:1:");
    rejects([] { duck::parse(duck::lex("\nMain() {", "syntax.dk")); },
            "syntax.dk:2:");

    const std::string source = R"(
Main() { Print(Even(4)); Alpha(); Beta() }
bool Even(int n) { if n == 0 { return true } else { return Odd(n - 1) } }
bool Odd(int n) { if n == 0 { return false } else { return Even(n - 1) } }
Alpha() { Print("alpha") }
Beta() { Print("beta") }
double Scale(double n) { return n + 0.5 }
Numbers() { double n = 1.25; Print(Scale(n)); Print(Scale(2)) }
int Assigned(bool b) { int n; if b { n = 1 } else { n = 2 }; return n }
int PostLoop() { int n; do { n = 4 } while false; return n }
int PostReturn() { do { return 3 } while false }
Utility #{ int Identity(int n) { return n } }
int Qualified() { return Utility.Identity(6) }
long Large() { return 5000000000 }
int FloorCast(double n) { return (int)n }
)";
    const auto program = duck::parse(duck::lex(source));
    const auto symbols = duck::collectSymbols(program);
    std::vector<std::future<duck::FunctionAnalysis>> analysisJobs;
    for (size_t i = 0; i < program.size(); ++i)
      analysisJobs.push_back(std::async(std::launch::async, [&, i] {
        return duck::analyzeFunction(program[i], symbols);
      }));
    std::vector<duck::FunctionAnalysis> analyses;
    for (auto &job : analysisJobs)
      analyses.push_back(job.get());
    std::vector<std::future<duck::FunctionIR>> loweringJobs;
    // Deliberately submit in reverse source order; collect in source order.
    for (size_t i = program.size(); i > 0; --i)
      loweringJobs.push_back(std::async(std::launch::async, [&, i] {
        return duck::lowerFunction(program[i - 1], symbols, analyses[i - 1]);
      }));
    std::vector<duck::FunctionIR> fragments(program.size());
    for (size_t i = 0; i < loweringJobs.size(); ++i)
      fragments[program.size() - 1 - i] = loweringJobs[i].get();
    const auto parallel = duck::assembleModule(fragments);
    check(parallel == duck::compile(program),
          "concurrent stages changed generated IR");
    check(fragments[3].globals.find("@str.Alpha.0") != std::string::npos &&
              fragments[4].globals.find("@str.Beta.0") != std::string::npos,
          "function-local constants need distinct symbols");
    check(duck::compile(program) == parallel,
          "repeated compilation leaked state");

    auto compileSource = [&source] {
      return duck::compile(duck::parse(duck::lex(source)));
    };
    auto first = std::async(std::launch::async, compileSource);
    auto second = std::async(std::launch::async, compileSource);
    check(first.get() == parallel && second.get() == parallel,
          "independent source pipelines changed generated IR");
    std::cout << "Compiler stages passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

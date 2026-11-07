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
    auto uninitialized = duck::parse(duck::lex("Main() { int n\nPrint(n) }"));
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
Main() { Print(Even(4))
    Alpha()
    Beta() }
bool Even(int n) { if n == 0 { return true } else { return Odd(n - 1) } }
bool Odd(int n) { if n == 0 { return false } else { return Even(n - 1) } }
Alpha() { Print("alpha") }
Beta() { Print("beta") }
double Scale(double n) { return n + 0.5 }
Numbers() { double n = 1.25
    Print(Scale(n))
    Print(Scale(2)) }
int Assigned(bool b) { int n
    if b { n = 1 } else { n = 2 }
    return n }
int PostLoop() { int n
    do { n = 4 } while false
    return n }
int PostReturn() { do { return 3 } while false }
Utility #{ int Identity(int n) { return n } }
int Qualified() { return Utility.Identity(6) }
long Large() { return 5000000000 }
int FloorCast(double n) { return (int)n }
int8 Small(int8 n) { n++
    return n }
short SmallFloor(double n) { return (int16)n }
int8 SmallMinimum() { return -128 }
short SmallSum(int8 n, short m) { return n + m }
uint64 UnsignedMaximum() { return 18446744073709551615 }
uint64 UnsignedDivide(uint64 n) { return n / (uint64)3 }
long Mixed(uint32 n, int m) { return n + m }
byte UnsignedFloor(double n) { return (uint8)n }
uint64 Hexadecimal() { return 0xFFFFFFFFFFFFFFFF }
long BinaryMinimum() { return -0b1000000000000000000000000000000000000000000000000000000000000000 }
uint64 Bitwise(uint64 n) { return ~(n ^ (uint64)0xFF) }
byte Shift(byte n, long count) { n <<= count
    return n >> count }
int BitwiseConditional(bool ready) { return ready ? (1 | 2) | 4 | 8 }
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

    auto native = duck::parse(duck::lex(
        "extern \"C\" int Absolute(int value) = \"abs\"\n"
        "Main() { Print(Absolute(-7)) }", "native.dk"));
    const auto nativeSymbols = duck::collectSymbols(native);
    auto nativeAnalysis = duck::analyzeFunction(native.front(), nativeSymbols);
    auto nativeFragment = duck::lowerFunction(native.front(), nativeSymbols, nativeAnalysis);
    check(nativeFragment.definition.empty() && nativeFragment.globals.empty(),
          "native declarations must not lower to Duck definitions");
    auto nativeIR = duck::compile(native);
    check(nativeIR.find("declare i32 @abs(i32)") != std::string::npos &&
              nativeIR.find("call i32 @abs(i32 -7)") != std::string::npos &&
              nativeIR.find("@duck.Absolute") == std::string::npos,
          "native symbol alias did not survive compilation stages");

    auto compileSource = [&source] {
      return duck::compile(duck::parse(duck::lex(source)));
    };
    auto first = std::async(std::launch::async, compileSource);
    auto second = std::async(std::launch::async, compileSource);
    check(first.get() == parallel && second.get() == parallel,
          "independent source pipelines changed generated IR");
    // Effect classification: only value-only call groups with no external
    // effects may run as tasks; one unknown callee taints its callers.
    auto effects = duck::parse(duck::lex(R"(
int Pure(int n) { return Helper(n) * 2 }
int Helper(int n) { if n < 1 { return 0 }
    return Pure(n - 1) + 1 }
int Prints(int n) { Print(n)
    return n }
int Tainted(int n) { return Prints(n) }
extern "C" int Absolute(int value) = "abs"
int Native(int n) { return Absolute(n) }
int Pointer(int* p) { return 1 }
string Text(int n) { return "x" }
int Local(int n) { string s = "x"
    return n }
Counter { int Value
    int Get() { return Value } }
Main() {
    a : Pure(1)
    b : Pure(2)
    c : Tainted(3)
    d : Pure(4)
    Print(a + b + c + d)
    e : Pure(5)
    Print(e)
    { f : Pure(6)
      g : Pure(7) }
}
)", "effects.dk"));
    const auto effectSymbols = duck::collectSymbols(effects);
    std::vector<duck::FunctionAnalysis> effectAnalyses;
    for (const auto &f : effects) effectAnalyses.push_back(duck::analyzeFunction(f, effectSymbols));
    const auto isolated = duck::isolatedFunctions(effects, effectAnalyses);
    check(isolated == std::set<std::string>{"Pure", "Helper"},
          "unexpected isolated functions");
    size_t mainIndex = 0;
    while (effects[mainIndex].name.text != "Main") ++mainIndex;
    auto &mainAnalysis = effectAnalyses[mainIndex];
    duck::scheduleTasks(effects[mainIndex], isolated, mainAnalysis);
    const auto &body = effects[mainIndex].body;
    // a: next statement is independent. b: next calls Tainted, a barrier.
    // c: unknown effects. d, e: joined immediately by Print. f: g follows.
    check(mainAnalysis.taskSpawns.count(body[0].get()) && !mainAnalysis.taskSpawns.count(body[1].get()) &&
              !mainAnalysis.taskSpawns.count(body[2].get()) && !mainAnalysis.taskSpawns.count(body[3].get()) &&
              !mainAnalysis.taskSpawns.count(body[5].get()),
          "unexpected task spawns");
    check(mainAnalysis.taskBarriers.count(body[2].get()) && mainAnalysis.taskBarriers.count(body[4].get()) &&
              !mainAnalysis.taskBarriers.count(body[1].get()),
          "unexpected task barriers");
    check(mainAnalysis.taskSpawns.count(body[7]->body[0].get()) &&
              !mainAnalysis.taskSpawns.count(body[7]->body[1].get()),
          "nested statement lists must schedule their own tasks");
    const auto effectsIR = duck::compile(effects);
    check(effectsIR == duck::compile(effects) &&
              effectsIR.find("@duck.task.Main.0") != std::string::npos &&
              duck::compile(effects, false).find("duck.task") == std::string::npos,
          "task lowering must be deterministic and disabled by sequential compilation");
    std::cout << "Compiler stages passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

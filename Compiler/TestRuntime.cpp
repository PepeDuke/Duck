#include "Pipeline.h"

namespace duck {
namespace {
std::string constant(const std::string &symbol, const std::string &value) {
  const char *hex = "0123456789ABCDEF";
  std::string escaped;
  for (unsigned char c : value) {
    escaped += '\\';
    escaped += hex[c >> 4];
    escaped += hex[c & 15];
  }
  return symbol + " = private unnamed_addr constant [" + std::to_string(value.size() + 1) + " x i8] c\"" + escaped +
         "\\00\"\n";
}
} // namespace
// The runner replaces Main as the executable's entry, so a test build shares
// no entry path with the program. Arguments starting with '-' are options; any
// other argument is a filter, and a test runs when its name contains a filter.
//
// --timing adds a line `TIME <nanoseconds> <name>` after each test's result,
// measured around the test body on a monotonic clock.
//
// Exit status: 0 when every selected test passed, 1 when any failed, and 2 for
// bad usage or filters that select nothing. A trapping test ends the process
// with the platform's own status; its RUN line is already flushed.
std::string testRunnerIR(const std::vector<std::pair<std::string, std::string>> &tests,
                         const std::set<std::string> &nativeSymbols) {
  std::string ir;
  const std::pair<const char *, const char *> imports[] = {{"fflush", "declare i32 @fflush(ptr)\n"},
                                                           {"strcmp", "declare i32 @strcmp(ptr, ptr)\n"},
                                                           {"strstr", "declare ptr @strstr(ptr, ptr)\n"}};
  for (const auto &[symbol, declaration] : imports)
    if (!nativeSymbols.count(symbol)) ir += declaration;
  // The compiler emits for its own host, so the host picks the clock.
#ifdef _WIN32
  const std::pair<const char *, const char *> clocks[] = {
      {"QueryPerformanceCounter", "declare i32 @QueryPerformanceCounter(ptr)\n"},
      {"QueryPerformanceFrequency", "declare i32 @QueryPerformanceFrequency(ptr)\n"}};
  for (const auto &[symbol, declaration] : clocks)
    if (!nativeSymbols.count(symbol)) ir += declaration;
  ir += R"IR(
define internal i64 @duck$test.now() {
entry:
  %ticks = alloca i64
  %rate = alloca i64
  call i32 @QueryPerformanceCounter(ptr %ticks)
  call i32 @QueryPerformanceFrequency(ptr %rate)
  %count = load i64, ptr %ticks
  %frequency = load i64, ptr %rate
  %seconds = sdiv i64 %count, %frequency
  %rest = srem i64 %count, %frequency
  %whole = mul i64 %seconds, 1000000000
  %scaled = mul i64 %rest, 1000000000
  %part = sdiv i64 %scaled, %frequency
  %nanos = add i64 %whole, %part
  ret i64 %nanos
}
)IR";
#else
  if (!nativeSymbols.count("clock_gettime")) ir += "declare i32 @clock_gettime(i32, ptr)\n";
  ir += R"IR(
define internal i64 @duck$test.now() {
entry:
  %time = alloca { i64, i64 }
  call i32 @clock_gettime(i32 1, ptr %time)
  %secondsSlot = getelementptr { i64, i64 }, ptr %time, i32 0, i32 0
  %nanosSlot = getelementptr { i64, i64 }, ptr %time, i32 0, i32 1
  %seconds = load i64, ptr %secondsSlot
  %rest = load i64, ptr %nanosSlot
  %whole = mul i64 %seconds, 1000000000
  %nanos = add i64 %whole, %rest
  ret i64 %nanos
}
)IR";
#endif
  ir += "@duck$test.failed = internal global i1 false\n";
  ir += constant("@duck$test.list", "--list");
  ir += constant("@duck$test.timing", "--timing");
  ir += constant("@duck$test.time", "TIME %lld %s\n");
  ir += constant("@duck$test.usage", "usage: <test program> [--list | --timing] [name-filter ...]\n");
  ir += constant("@duck$test.none", "no test matches the filter\n");
  ir += constant("@duck$test.line", "%s\n");
  ir += constant("@duck$test.run", "RUN  %s\n");
  ir += constant("@duck$test.pass", "PASS %s\n");
  ir += constant("@duck$test.fail", "FAIL %s\n");
  ir += constant("@duck$test.summary", "%d passed, %d failed, %d total\n");
  const auto count = std::to_string(tests.size());
  std::string names, bodies;
  for (size_t i = 0; i < tests.size(); ++i) {
    const auto symbol = "@duck$test.name." + std::to_string(i);
    ir += constant(symbol, tests[i].first);
    names += (i ? ", ptr " : "ptr ") + symbol;
    bodies += (i ? ", ptr " : "ptr ") + tests[i].second;
  }
  const auto table = "[" + count + " x ptr]";
  ir += "@duck$test.names = internal constant " + table + " " + (tests.empty() ? "zeroinitializer" : "[" + names + "]") + "\n";
  ir += "@duck$test.bodies = internal constant " + table + " " + (tests.empty() ? "zeroinitializer" : "[" + bodies + "]") + "\n";
  ir += R"IR(
define internal i32 @duck$test.mode(i32 %argc, ptr %argv) {
entry:
  br label %next
next:
  %i = phi i32 [ 1, %entry ], [ %following, %filter ], [ %following, %listing ], [ %following, %timed ]
  %mode = phi i32 [ 0, %entry ], [ %mode, %filter ], [ 1, %listing ], [ 3, %timed ]
  %more = icmp slt i32 %i, %argc
  br i1 %more, label %read, label %done
read:
  %slot = getelementptr ptr, ptr %argv, i32 %i
  %argument = load ptr, ptr %slot
  %following = add i32 %i, 1
  %first = load i8, ptr %argument
  %isFilter = icmp ne i8 %first, 45
  br i1 %isFilter, label %filter, label %option
filter:
  br label %next
option:
  %compared = call i32 @strcmp(ptr %argument, ptr @duck$test.list)
  %isList = icmp eq i32 %compared, 0
  br i1 %isList, label %listing, label %timingOption
timingOption:
  %comparedTiming = call i32 @strcmp(ptr %argument, ptr @duck$test.timing)
  %isTiming = icmp eq i32 %comparedTiming, 0
  br i1 %isTiming, label %timed, label %bad
listing:
  br label %next
timed:
  br label %next
bad:
  ret i32 2
done:
  ret i32 %mode
}

define internal i1 @duck$test.selected(ptr %name, i32 %argc, ptr %argv) {
entry:
  br label %next
next:
  %i = phi i32 [ 1, %entry ], [ %following, %read ]
  %filtered = phi i1 [ false, %entry ], [ %filteredNext, %read ]
  %more = icmp slt i32 %i, %argc
  br i1 %more, label %examine, label %done
examine:
  %slot = getelementptr ptr, ptr %argv, i32 %i
  %argument = load ptr, ptr %slot
  %following = add i32 %i, 1
  %first = load i8, ptr %argument
  %isFilter = icmp ne i8 %first, 45
  %found = call ptr @strstr(ptr %name, ptr %argument)
  %contains = icmp ne ptr %found, null
  %hit = and i1 %isFilter, %contains
  br i1 %hit, label %yes, label %read
read:
  %filteredNext = or i1 %filtered, %isFilter
  br label %next
yes:
  ret i1 true
done:
  %all = xor i1 %filtered, true
  ret i1 %all
}

define i32 @main(i32 %argc, ptr %argv) {
entry:
  %index = alloca i32
  %selected = alloca i32
  %passed = alloca i32
  %failed = alloca i32
  store i32 0, ptr %index
  store i32 0, ptr %selected
  store i32 0, ptr %passed
  store i32 0, ptr %failed
  %mode = call i32 @duck$test.mode(i32 %argc, ptr %argv)
  %badUsage = icmp eq i32 %mode, 2
  br i1 %badUsage, label %usage, label %start
usage:
  call i32 (ptr, ...) @printf(ptr @duck$test.usage)
  ret i32 2
start:
  %listing = icmp eq i32 %mode, 1
  %timing = icmp eq i32 %mode, 3
  br label %loop
loop:
  %current = load i32, ptr %index
  %more = icmp ult i32 %current, COUNT
  br i1 %more, label %body, label %finish
body:
  %nameSlot = getelementptr TABLE, ptr @duck$test.names, i32 0, i32 %current
  %name = load ptr, ptr %nameSlot
  %chosen = call i1 @duck$test.selected(ptr %name, i32 %argc, ptr %argv)
  br i1 %chosen, label %act, label %advance
act:
  %selectedCount = load i32, ptr %selected
  %selectedNext = add i32 %selectedCount, 1
  store i32 %selectedNext, ptr %selected
  br i1 %listing, label %print, label %run
print:
  call i32 (ptr, ...) @printf(ptr @duck$test.line, ptr %name)
  br label %advance
run:
  call i32 (ptr, ...) @printf(ptr @duck$test.run, ptr %name)
  call i32 @fflush(ptr null)
  store i1 false, ptr @duck$test.failed
  %bodySlot = getelementptr TABLE, ptr @duck$test.bodies, i32 0, i32 %current
  %test = load ptr, ptr %bodySlot
  %started = call i64 @duck$test.now()
  call void %test()
  %stopped = call i64 @duck$test.now()
  %elapsed = sub i64 %stopped, %started
  %didFail = load i1, ptr @duck$test.failed
  br i1 %didFail, label %onFail, label %onPass
onFail:
  call i32 (ptr, ...) @printf(ptr @duck$test.fail, ptr %name)
  %failedCount = load i32, ptr %failed
  %failedNext = add i32 %failedCount, 1
  store i32 %failedNext, ptr %failed
  br label %afterRun
onPass:
  call i32 (ptr, ...) @printf(ptr @duck$test.pass, ptr %name)
  %passedCount = load i32, ptr %passed
  %passedNext = add i32 %passedCount, 1
  store i32 %passedNext, ptr %passed
  br label %afterRun
afterRun:
  br i1 %timing, label %reportTime, label %advance
reportTime:
  call i32 (ptr, ...) @printf(ptr @duck$test.time, i64 %elapsed, ptr %name)
  br label %advance
advance:
  %following = add i32 %current, 1
  store i32 %following, ptr %index
  br label %loop
finish:
  %total = load i32, ptr %selected
  %none = icmp eq i32 %total, 0
  %hasOption = icmp ne i32 %mode, 0
  %optionCount = select i1 %hasOption, i32 2, i32 1
  %hasFilter = icmp sgt i32 %argc, %optionCount
  %unmatched = and i1 %none, %hasFilter
  br i1 %unmatched, label %noMatch, label %report
noMatch:
  call i32 (ptr, ...) @printf(ptr @duck$test.none)
  ret i32 2
report:
  br i1 %listing, label %listed, label %summary
listed:
  ret i32 0
summary:
  %passes = load i32, ptr %passed
  %failures = load i32, ptr %failed
  call i32 (ptr, ...) @printf(ptr @duck$test.summary, i32 %passes, i32 %failures, i32 %total)
  call i32 @fflush(ptr null)
  %anyFailed = icmp ne i32 %failures, 0
  %status = zext i1 %anyFailed to i32
  ret i32 %status
}
)IR";
  const auto replace = [&](const std::string &token, const std::string &value) {
    for (size_t at = ir.find(token); at != std::string::npos; at = ir.find(token, at + value.size()))
      ir.replace(at, token.size(), value);
  };
  replace("COUNT", count);
  replace("TABLE", table);
  // Read by the build tool, which skips modules without tests.
  return ir + "; duck-tests: " + count + "\n";
}
} // namespace duck

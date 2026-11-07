#include "Pipeline.h"

namespace duck {
// Worker tasks for calls with known effects (Windows x64 threads).
// DUCK_WORKERS limits how many threads may run Duck code at once, including
// the caller; it defaults to the active processor count. When the limit is
// reached, or a thread cannot be created, the task runs inline on the caller,
// so nested spawns never wait for a free worker. DUCK_WORKERS=1 runs every
// task inline, in source order. A task record lives in the spawning frame;
// the compiler joins it before that frame can exit.
std::string taskRuntimeIR(const std::set<std::string> &nativeSymbols) {
  std::string ir;
  // Call sites state complete function types, so an existing program
  // declaration of the same symbol can be reused.
  const std::pair<const char *, const char *> imports[] = {
      {"CreateThread", "declare ptr @CreateThread(ptr, i64, ptr, ptr, i32, ptr)\n"},
      {"WaitForSingleObject", "declare i32 @WaitForSingleObject(ptr, i32)\n"},
      {"CloseHandle", "declare i32 @CloseHandle(ptr)\n"},
      {"GetActiveProcessorCount", "declare i32 @GetActiveProcessorCount(i16)\n"},
      {"getenv", "declare ptr @getenv(ptr)\n"},
      {"atoi", "declare i32 @atoi(ptr)\n"}};
  for (const auto &[symbol, declaration] : imports)
    if (!nativeSymbols.count(symbol)) ir += declaration;
  return ir + R"IR(
@duck.task.limit = internal global i32 0
@duck.task.active = internal global i32 0
@duck.task.variable = private unnamed_addr constant [13 x i8] c"DUCK_WORKERS\00"

define internal i32 @duck.task$limit() {
entry:
  %cached = load atomic i32, ptr @duck.task.limit monotonic, align 4
  %known = icmp sgt i32 %cached, 0
  br i1 %known, label %done, label %read
read:
  %text = call ptr @getenv(ptr @duck.task.variable)
  %unset = icmp eq ptr %text, null
  br i1 %unset, label %hardware, label %parse
parse:
  %requested = call i32 @atoi(ptr %text)
  %valid = icmp sgt i32 %requested, 0
  br i1 %valid, label %store, label %hardware
hardware:
  %cores = call i32 @GetActiveProcessorCount(i16 -1)
  %some = icmp sgt i32 %cores, 0
  %count = select i1 %some, i32 %cores, i32 1
  br label %store
store:
  %limit = phi i32 [ %requested, %parse ], [ %count, %hardware ]
  store atomic i32 %limit, ptr @duck.task.limit monotonic, align 4
  ret i32 %limit
done:
  ret i32 %cached
}

; Returns the thread handle, or null when the task already ran inline.
define internal ptr @duck.task$spawn(ptr %function, ptr %task) {
entry:
  %limit = call i32 @duck.task$limit()
  ; Saturated workers are the common case in recursive work; check without writing.
  %seen = load atomic i32, ptr @duck.task.active monotonic, align 4
  %projected = add i32 %seen, 2
  %room = icmp sle i32 %projected, %limit
  br i1 %room, label %claim, label %direct
direct:
  %result = call i32 %function(ptr %task)
  ret ptr null
claim:
  %previous = atomicrmw add ptr @duck.task.active, i32 1 seq_cst
  %running = add i32 %previous, 2
  %allowed = icmp sle i32 %running, %limit
  br i1 %allowed, label %create, label %inline
create:
  %handle = call ptr @CreateThread(ptr null, i64 0, ptr %function, ptr %task, i32 0, ptr null)
  %created = icmp ne ptr %handle, null
  br i1 %created, label %started, label %inline
started:
  ret ptr %handle
inline:
  %undo = atomicrmw sub ptr @duck.task.active, i32 1 seq_cst
  %ignored = call i32 %function(ptr %task)
  ret ptr null
}

; Waiting for thread exit orders the worker's result store before the caller's load.
define internal void @duck.task$join(ptr %handle) {
entry:
  %inline = icmp eq ptr %handle, null
  br i1 %inline, label %done, label %wait
wait:
  %status = call i32 @WaitForSingleObject(ptr %handle, i32 -1)
  %failed = icmp ne i32 %status, 0
  br i1 %failed, label %trap, label %close
trap:
  call void @llvm.trap()
  unreachable
close:
  %closed = call i32 @CloseHandle(ptr %handle)
  %finished = atomicrmw sub ptr @duck.task.active, i32 1 seq_cst
  br label %done
done:
  ret void
}
)IR";
}
} // namespace duck

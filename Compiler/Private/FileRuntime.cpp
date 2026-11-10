#include "Pipeline.h"

namespace duck {
// The `file` type. A file value is the C runtime's FILE*, null while closed.
// Paths are Duck strings, which are UTF-16 and end in a zero unit, so they go
// straight to _wfopen. Files are always opened in binary mode: Read and Write
// move bytes unchanged, and ReadText decodes them as UTF-8.
//
// Mode 0 opens an existing file for reading, 1 creates or replaces a file for
// writing, and 2 writes at the end of a file, creating it when missing.
// Opening closes whatever the value held before. Reading or writing a closed
// file moves nothing and gives 0.
std::string fileRuntimeIR(const std::set<std::string> &nativeSymbols) {
  std::string ir;
  const std::pair<const char *, const char *> imports[] = {
      {"_wfopen", "declare ptr @_wfopen(ptr, ptr)\n"},
      {"fclose", "declare i32 @fclose(ptr)\n"},
      {"fread", "declare i64 @fread(ptr, i64, i64, ptr)\n"},
      {"fwrite", "declare i64 @fwrite(ptr, i64, i64, ptr)\n"},
      {"_fseeki64", "declare i32 @_fseeki64(ptr, i64, i32)\n"},
      {"_ftelli64", "declare i64 @_ftelli64(ptr)\n"},
  };

  for (const auto &[symbol, declaration] : imports)
    if (!nativeSymbols.count(symbol)) ir += declaration;

  return ir + R"IR(
@duck.file$modes = private unnamed_addr constant [3 x [3 x i16]] [[3 x i16] [i16 114, i16 98, i16 0], [3 x i16] [i16 119, i16 98, i16 0], [3 x i16] [i16 97, i16 98, i16 0]]

define internal void @duck.file$close(ptr %f) {
entry:
  %open = icmp ne ptr %f, null
  br i1 %open, label %close, label %done
close:
  %ignored = call i32 @fclose(ptr %f)
  br label %done
done:
  ret void
}
define internal void @duck.file$drop(ptr %slot) {
entry:
  %f = load ptr, ptr %slot
  call void @duck.file$close(ptr %f)
  store ptr null, ptr %slot
  ret void
}
define internal i1 @duck.file$open(ptr %slot, ptr %path, i32 %mode) {
entry:
  call void @duck.file$drop(ptr %slot)
  %units = getelementptr i8, ptr %path, i64 16
  %flags = getelementptr [3 x [3 x i16]], ptr @duck.file$modes, i32 0, i32 %mode
  %f = call ptr @_wfopen(ptr %units, ptr %flags)
  store ptr %f, ptr %slot
  %ok = icmp ne ptr %f, null
  ret i1 %ok
}
define internal i64 @duck.file$read(ptr %f, ptr %buffer, i64 %size, i64 %count) {
entry:
  %open = icmp ne ptr %f, null
  br i1 %open, label %read, label %closed
read:
  %n = call i64 @fread(ptr %buffer, i64 %size, i64 %count, ptr %f)
  ret i64 %n
closed:
  ret i64 0
}
define internal i64 @duck.file$write(ptr %f, ptr %buffer, i64 %size, i64 %count) {
entry:
  %open = icmp ne ptr %f, null
  br i1 %open, label %write, label %closed
write:
  %n = call i64 @fwrite(ptr %buffer, i64 %size, i64 %count, ptr %f)
  ret i64 %n
closed:
  ret i64 0
}
; The rest of the file, from the current position, as a name (UTF-8).
define internal ptr @duck.file$text(ptr %f) {
entry:
  %open = icmp ne ptr %f, null
  br i1 %open, label %measure, label %empty
measure:
  %at = call i64 @_ftelli64(ptr %f)
  %toEnd = call i32 @_fseeki64(ptr %f, i64 0, i32 2)
  %end = call i64 @_ftelli64(ptr %f)
  %back = call i32 @_fseeki64(ptr %f, i64 %at, i32 0)
  %rest = sub i64 %end, %at
  %known = icmp sgt i64 %rest, 0
  %valid = icmp sge i64 %at, 0
  %sized = and i1 %known, %valid
  br i1 %sized, label %read, label %empty
read:
  %t = call ptr @duck.text.alloc(i64 %rest, i64 1)
  %data = getelementptr i8, ptr %t, i64 16
  %n = call i64 @fread(ptr %data, i64 1, i64 %rest, ptr %f)
  call void @duck.text.finish(ptr %t, i64 %n, i64 1)
  ret ptr %t
empty:
  %none = call ptr @duck.text.alloc(i64 0, i64 1)
  ret ptr %none
}
)IR";
}
} // namespace duck

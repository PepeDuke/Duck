#pragma once
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace duck {
// With DUCKC_TIMINGS set, phase("name") prints to stderr how long the compiler
// spent since the previous phase mark, to find which stage of a slow compile
// takes the time. Without it, a mark costs one cached check.
// Set at startup, so the first phase includes everything before it.
inline auto phaseStart = std::chrono::steady_clock::now();

inline bool timingEnabled() {
  static const bool enabled = std::getenv("DUCKC_TIMINGS") != nullptr;
  return enabled;
}

inline void phase(const char *name) {
  if (!timingEnabled()) return;

  const auto now = std::chrono::steady_clock::now();
  std::fprintf(stderr, "duckc: %-24s %8.3fs\n", name, std::chrono::duration<double>(now - phaseStart).count());
  phaseStart = now;
}
} // namespace duck

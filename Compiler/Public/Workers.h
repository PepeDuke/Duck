#pragma once
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

namespace duck {
// Threads for the compiler phases that handle each function on its own.
constexpr size_t Workers = 4;

// Runs work(i) once for every i in [0, count) on up to Workers threads, and
// returns when all have finished. The calling thread hands out the indices
// through one shared counter, which serves as the queue. work must not throw:
// a caller keeps each item's failure and handles them in order afterwards.
template <class Work> void parallelFor(size_t count, Work work) {
  std::atomic<size_t> next{0};
  auto run = [&] {
    for (size_t i = next++; i < count; i = next++) work(i);
  };
  std::vector<std::thread> threads;

  for (size_t t = 0; t < std::min(Workers, count); ++t) threads.emplace_back(run);

  for (auto &thread : threads) thread.join();
}
} // namespace duck

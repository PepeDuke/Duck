#include "Syntax.h"
#include <mutex>

namespace duck::nodes {
namespace {
// A block is rounded up to a multiple of Grain bytes, and each size class has
// its own free list. Larger blocks go to the general heap.
constexpr std::size_t Grain = 16, Classes = 64, ChunkBytes = 256 * 1024;

struct FreeBlock {
  FreeBlock *next;
};

// Free blocks a thread hands back when it ends, for other threads to reuse.
struct Shared {
  std::mutex lock;
  FreeBlock *free[Classes] = {};
};

Shared &shared() {
  static Shared pool;
  return pool;
}

// One thread's free lists and the unused end of its current chunk. A block
// freed on a thread joins that thread's lists, whichever thread made it, so
// no list is ever touched by two threads and none needs a lock.
struct Cache {
  FreeBlock *free[Classes] = {};
  char *next = nullptr, *end = nullptr;

  ~Cache() {
    auto &pool = shared();
    std::lock_guard<std::mutex> guard(pool.lock);

    for (std::size_t c = 0; c < Classes; ++c)
      while (auto *block = free[c]) {
        free[c] = block->next;
        block->next = pool.free[c];
        pool.free[c] = block;
      }
  }
};

thread_local Cache cache;
} // namespace

void *allocate(std::size_t bytes) {
  const std::size_t sizeClass = (bytes + Grain - 1) / Grain;

  if (sizeClass == 0 || sizeClass >= Classes) return ::operator new(bytes);

  auto &own = cache;

  if (auto *block = own.free[sizeClass]) {
    own.free[sizeClass] = block->next;
    return block;
  }

  const std::size_t size = sizeClass * Grain;

  if (static_cast<std::size_t>(own.end - own.next) < size) {
    // The chunk is used up: first take what ended threads left of this size,
    // then start a new chunk. The old chunk's few spare bytes are not used.
    {
      auto &pool = shared();
      std::lock_guard<std::mutex> guard(pool.lock);

      if (auto *block = pool.free[sizeClass]) {
        own.free[sizeClass] = block->next;
        pool.free[sizeClass] = nullptr;
        return block;
      }
    }

    own.next = static_cast<char *>(::operator new(ChunkBytes));
    own.end = own.next + ChunkBytes;
  }

  void *block = own.next;
  own.next += size;
  return block;
}

void release(void *block, std::size_t bytes) noexcept {
  const std::size_t sizeClass = (bytes + Grain - 1) / Grain;

  if (sizeClass == 0 || sizeClass >= Classes) {
    ::operator delete(block);
    return;
  }

  auto *freed = static_cast<FreeBlock *>(block);
  freed->next = cache.free[sizeClass];
  cache.free[sizeClass] = freed;
}
} // namespace duck::nodes

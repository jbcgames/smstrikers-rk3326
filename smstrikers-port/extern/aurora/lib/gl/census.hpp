#pragma once

#include <atomic>
#include <cstdint>

// Live census of driver-side GL objects, for attributing GPU-memory growth on the
// 1 GB Mali devices (libmali's /dev/mali0 mappings are pinned and billed to the
// process; the G31 OOM kills show them dominating RSS). Byte totals are exact for
// textures/buffers (sizes we chose); programs, FBOs and samplers are counted only
// (their GPU cost is driver-internal).
namespace aurora::gl::census {

struct Counter {
  std::atomic<int64_t> count{0};
  std::atomic<int64_t> bytes{0};

  void add(int64_t sz) noexcept {
    count.fetch_add(1, std::memory_order_relaxed);
    bytes.fetch_add(sz, std::memory_order_relaxed);
  }
  void sub(int64_t sz) noexcept {
    count.fetch_sub(1, std::memory_order_relaxed);
    bytes.fetch_sub(sz, std::memory_order_relaxed);
  }
};

extern Counter textures;
extern Counter buffers;
extern Counter programs;
extern Counter fbos;
extern Counter samplers;

// One-shot summary line, logged from backend shutdown; leaks show up as a nonzero
// residual count. Not logged per frame (the periodic line was pure log spam in play).
void log_final() noexcept;

} // namespace aurora::gl::census

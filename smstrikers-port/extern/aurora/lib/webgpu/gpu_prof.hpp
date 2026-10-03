#pragma once

#include <string_view>

// GPU profiling zones. These wrapped Dawn timestamp-query writes into a Tracy GPU
// context; that machinery is Dawn-specific and is stubbed for the GL backend
// (Phase 7 may reintroduce it via EXT_disjoint_timer_query). The call sites keep
// their shape -- a scoped Zone and frame markers -- but do nothing.

namespace aurora::webgpu::gpu_prof {

void initialize();
void shutdown();

void frame_begin();
void frame_end();
void after_submit();

class Zone {
public:
  explicit Zone(std::string_view name) noexcept { (void)name; }
  ~Zone() = default;
  Zone(const Zone&) = delete;
  Zone& operator=(const Zone&) = delete;
};

} // namespace aurora::webgpu::gpu_prof

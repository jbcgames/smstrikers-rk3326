// GPU profiling zones -- stubbed for the GL backend.
//
// The original implementation wrote Dawn timestamp queries into a Tracy GPU
// context. That is Dawn-specific; the GL backend has no equivalent yet (Phase 7
// may reintroduce it via EXT_disjoint_timer_query). The Zone class is a no-op
// defined inline in gpu_prof.hpp; the frame/submit markers below do nothing so the
// call sites keep their shape.

#include "gpu_prof.hpp"

namespace aurora::webgpu::gpu_prof {

void initialize() {}
void shutdown() {}

void frame_begin() {}
void frame_end() {}
void after_submit() {}

} // namespace aurora::webgpu::gpu_prof

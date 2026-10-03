#pragma once

// GL buffer primitives for the hand-rolled backend.
//
// Two upload modes behind one handle. `persistent` buffers use GL_EXT_buffer_storage: an immutable
// persistent-coherent mapping the CPU fills with memcpy and the GPU reads with no driver-side copy
// -- this is the device fast path that bypasses libmali's copy worker (the memory-bandwidth cost of
// glBufferSubData, Normalcy Doctrine rule 4's "later, measured escalation"). When the extension is
// absent, or for buffers that must stay mutable, uploads fall back to plain glBufferSubData.
//
// IMPORTANT: immutable persistent storage has no glBufferData-style implicit orphaning, so a
// persistent buffer must NOT be memcpy'd while the GPU is still reading a range being overwritten.
// That hazard is the CALLER's to manage (common.cpp double-buffers the per-frame ring per frame slot
// and fences slot reuse). Buffers only ever appended at fresh offsets (the native geom cache) are
// safe without double-buffering. This module owns only the GL object + its mapping.

#include "gl_core.hpp"
#include "handles.hpp"

namespace aurora::gl {

// Create a GL buffer of `size` bytes bound to `target` (GL_ARRAY_BUFFER, GL_UNIFORM_BUFFER, ...).
// `dynamic` picks GL_DYNAMIC_DRAW vs GL_STATIC_DRAW for the mutable fallback. When `persistent` is
// set and GL_EXT_buffer_storage is available, the buffer is instead immutable + persistently mapped
// (Buffer::mapped is non-null); if the extension is missing it silently degrades to the `dynamic`
// mutable buffer. Contents are left undefined. Requires the owning context current.
Buffer create_buffer(GLenum target, uint64_t size, bool dynamic, bool persistent = false);

// Update a sub-range. For a persistent buffer this is a coherent memcpy into the mapping (no GL
// call, no flush); otherwise glBufferSubData. `offset + size` must be within the buffer.
void upload_buffer(const Buffer& buffer, uint64_t offset, const void* data, uint64_t size);

void destroy_buffer(Buffer& buffer) noexcept;

} // namespace aurora::gl

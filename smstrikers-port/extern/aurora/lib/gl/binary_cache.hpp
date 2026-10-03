#pragma once

// On-disk GL program-binary cache for the hand-rolled GLES backend.
//
// libmali/PowerVR have no persistent shader cache on Linux (the Android blob-cache
// callbacks are absent), so every boot re-links 200+ GLSL programs from source --
// ~100-500 ms each on Mali-G31, the minutes-long low-fps warm-up after every launch.
// This cache stores the driver's own glGetProgramBinary output keyed by a hash of the
// shader source text and feeds it back via glProgramBinary on the next boot, skipping
// compilation entirely. It sits inside gl::compile_program (the single funnel for every
// program: GX, RmlUi, clear, present, texture conversion), so all post-link setup runs
// identically on the binary-load path.
//
// Every entry point that issues GL (try_load/store) must be called with the owning
// context current -- the pipeline-compiler thread's share context, or the render worker
// in threadless mode. initialize()/shutdown() run on the main thread; they only touch
// GL strings (the driver fingerprint) and the sqlite db, never program-binary calls.
//
// The cache is self-disabling: absent glProgramBinary support, a driver-format change,
// or a Mesa renderer (its program-binary deserializer has bitten us before, task #77)
// all degrade to plain source compilation -- today's behavior. A crash sentinel bounds
// any driver crash inside glProgramBinary to at most two bad boots before the cache
// disables itself for that db.

#include "gl_core.hpp"

#include <cstdint>

namespace aurora::gl {

// Open/validate the cache. Call once, main thread, after the GL proc table is loaded and
// a context is current (query_caps time), BEFORE the pipeline cache begins its boot
// precompile. No-op-enables to a disabled state on any unsupported configuration.
void binary_cache_initialize();

// Flush pending writes, join the writer thread, close the db, clear the crash sentinel.
void binary_cache_shutdown();

// True when the cache is active (driver supports binaries, not Mesa, db open). compile_program
// uses this to decide whether to set GL_PROGRAM_BINARY_RETRIEVABLE_HINT before linking.
bool binary_cache_enabled();

// XXH3 of the concatenated vertex+fragment source -- the content-addressed cache key. Stable
// across our builds; an emitter change simply produces new keys.
uint64_t binary_cache_key(const char* vertexSource, const char* fragmentSource);

// Try to link `program` (a fresh, empty program object) from the cached binary for `key`.
// Returns true only when glProgramBinary reports GL_LINK_STATUS==GL_TRUE; the caller then
// skips source compilation. On any miss/reject this drains glGetError, drops the offending
// entry, and returns false, leaving the caller to compile from source. Disabled cache: false.
bool binary_cache_try_load(uint64_t key, GLuint program);

// Store a freshly linked program's binary under `key` (must have been linked with the
// retrievable hint set). Enqueues the blob to the writer thread. No-op when disabled or when
// `key` is already present.
void binary_cache_store(uint64_t key, GLuint program);

// Called when the boot pipeline precompile queue drains: the process survived the risky
// glProgramBinary loading window, so clear the crash sentinel + reset the poison counter, and
// log a one-line summary alongside the pipeline-cache stats supplied by the caller.
void binary_cache_precompile_drained();

// Hit/miss counters for the summary line (hits = binaries loaded, misses = source compiles
// that went through the cache path).
uint32_t binary_cache_hits();
uint32_t binary_cache_misses();

} // namespace aurora::gl

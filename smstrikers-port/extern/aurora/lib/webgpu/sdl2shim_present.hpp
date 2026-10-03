#pragma once

#include <cstdint>
#include <optional>

#include "../gl/handles.hpp"

namespace aurora::webgpu::sdl2shim_present {

// Dusklight (P4): double-buffered EFB present for the Mali/SDL2-shim device path.
//
// The render worker owns our GLES render context and does the expensive work (game render). But
// presentation is display-thread-bound on Mali's kmsdrm/gbm stack: a swap issued from the worker
// posts a buffer that is never scanned out (silent blank screen). So the worker composites the
// finished frame (scene + UI) into one of two shared "EFB" textures and the main thread blits that
// texture to the window's default framebuffer and swaps on the shim's borrowed context.
//
// The two threads run *different* EGL contexts (our render context, and the shim's borrowed one),
// which is what lets them run concurrently. The shared textures are backed by an EGLImage: the
// shim-side GL texture is created on the main thread, and the worker aliases the same image into a
// worker-context texture (glEGLImageTargetTexture2DOES) wrapped in an FBO it composites into. The
// two contexts are ordered with EGL fences.
//
// Inert unless the SDL2-shim video driver is in use; desktop keeps presenting from the worker.
struct AcquiredFrame {
  uint32_t slot = 0;
  uint32_t fbo = 0; // worker-context FBO aliasing the slot's shared texture; render the frame here
};

// True once initialize() has succeeded. Every other entry point is a no-op when false.
[[nodiscard]] bool active() noexcept;

// Main thread, during webgpu::initialize(), with the shim's EGL context current. `width`/`height`
// are the native present (window drawable) size -- the shared textures hold the final composited
// frame at native resolution. The global gl::gl proc table must already be loaded.
bool initialize(void* eglDisplay, uint32_t width, uint32_t height, gl::TextureFormat format);
// Reallocate the shared textures for a new window size. No-op when the size is unchanged.
// Main thread, shim context current, with no frame in flight.
bool resize(uint32_t width, uint32_t height, gl::TextureFormat format);
void shutdown();

// --- Render worker ---
// Claim the next EFB slot to render into, blocking until the main thread is done reading it.
// Returns nullopt when inactive or on failure. On the device path failure is fatal (no fallback);
// the caller logs and drops the frame. First call lazily aliases the slots into worker FBOs.
std::optional<AcquiredFrame> acquire();
// Fence the worker's render of this slot and hand it to the main thread. Must follow a successful
// acquire(), and must run on the worker after the frame's draws are recorded into the slot FBO.
void publish(uint32_t slot);
// Report that this frame produced nothing to present (keeps the main thread's accounting exact).
void publish_empty();

// --- Main thread ---
// Called once per frame immediately before the end-of-frame work is enqueued, so flush_present()
// knows a frame is owed and how long to wait for it.
void note_frame_enqueued();
// Present the frame the worker produced last: wait for it, blit it to the window, swap.
void flush_present();
// Drop any frame the worker is holding out to us without presenting it, and unblock a worker that
// is waiting for a slot. Must run before any render_worker::synchronize() on the main thread.
void recycle_pending();

} // namespace aurora::webgpu::sdl2shim_present

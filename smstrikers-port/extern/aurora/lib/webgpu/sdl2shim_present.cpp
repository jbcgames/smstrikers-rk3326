#include "sdl2shim_present.hpp"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

#include <SDL3/SDL_video.h>
#include <magic_enum.hpp>
#include <tracy/Tracy.hpp>

#include "../gfx/common.hpp"
#include "../gl/gl_core.hpp"
#include "../internal.hpp"
#include "../window.hpp"
#include "gpu.hpp"

namespace aurora::webgpu::sdl2shim_present {
namespace {
static Module Log("aurora::sdl2shim_present");

// Two slots: the worker composites into one while the main thread scans out the other. A third would
// only let the worker run further ahead of the display, which we don't want.
constexpr uint32_t SlotCount = 2;
// How long the main thread will wait for the worker to hand over the frame it owes us. Generous:
// exceeding it means the worker is wedged, and we'd rather log and keep the game responsive.
constexpr auto PresentWaitTimeout = std::chrono::milliseconds{500};
// Upper bound on the worker's wait for a slot's reverse fence (the main thread's blit of the
// previous frame retiring on the GPU). In steady state the fence retired a frame ago and the wait
// returns immediately; the bound only exists so a wedged fence produces one logged, torn frame
// instead of a silent hang.
constexpr uint64_t RevSyncWaitTimeoutNs = 2'000'000'000;

// The firmware SDL2's own swap entry point, republished by the SDL3 shim. This is the exact call
// the borrowed-context present path reaches the display with, so prefer it over routing back out
// through SDL3 -> shim -> SDL2.
using Sdl2SwapWindowFn = void (*)(void*);
constexpr const char* SDL2_SHIM_WINDOW_PROP = "SDL.window.sdl2_backend.window";
constexpr const char* SDL2_SHIM_GL_SWAP_WINDOW_PROP = "SDL.window.sdl2_backend.gl_swap_window";

enum class SlotState : uint8_t {
  Free,      // the worker may composite into it
  Rendering, // the worker owns it
  Ready,     // submitted; the main thread may present it
};

struct Slot {
  // Shim context (main thread): the RGBA8 texture backing the EGLImage, plus a read FBO the main
  // thread blits from during scan-out.
  uint32_t shimTexture = 0;
  uint32_t shimReadFbo = 0;
  void* eglImage = nullptr; // the bridge between the two contexts
  // Worker/render context: a texture aliasing the same EGLImage, wrapped in the FBO the present
  // composite renders into. Created lazily on the worker (ensure_worker_slots).
  uint32_t workerTexture = 0;
  uint32_t workerFbo = 0;
  void* fwdSync = nullptr; // worker finished rendering this slot (worker creates, main server-waits)
  void* revSync = nullptr; // main finished reading this slot (main creates, worker CPU-waits)
  SlotState state = SlotState::Free;
};

bool g_active = false;
bool g_workerAttached = false;
void* g_display = nullptr;
void* g_shimContext = nullptr;
uint32_t g_width = 0;
uint32_t g_height = 0;
void* g_sdl2Window = nullptr;
Sdl2SwapWindowFn g_sdl2SwapWindow = nullptr;
std::array<Slot, SlotCount> g_slots;

std::mutex g_mutex;
std::condition_variable g_slotFreed;  // main -> worker
std::condition_variable g_frameReady; // worker -> main
uint32_t g_nextSlot = 0;
int32_t g_framesOwed = 0;     // end_frames enqueued but not yet presented/dropped
bool g_hasReadyFrame = false;
uint32_t g_readySlot = 0;
bool g_aborting = false;

void destroy_sync(void*& sync) {
  if (sync != nullptr && gl::gl.eglDestroySyncKHR != nullptr) {
    gl::gl.eglDestroySyncKHR(g_display, sync);
    sync = nullptr;
  }
}

// Create an EGL fence in the current context's command stream and flush it (so the other context
// can wait on it). Called on main (shim ctx, reverse fence) and on the worker (render ctx, forward
// fence).
void* create_fence() {
  if (gl::gl.eglCreateSyncKHR == nullptr) {
    return nullptr;
  }
  void* sync = gl::gl.eglCreateSyncKHR(g_display, gl::EGL_SYNC_FENCE_KHR, nullptr);
  if (sync == nullptr) {
    return nullptr;
  }
  gl::gl.Flush();
  return sync;
}

// Main thread, shim context current: allocate the shim-side texture + read FBO and bridge it as an
// EGLImage. The worker aliases the same image in ensure_worker_slots().
bool create_shim_slot(Slot& slot) {
  gl::gl.GenTextures(1, &slot.shimTexture);
  gl::gl.BindTexture(gl::GL_TEXTURE_2D, slot.shimTexture);
  gl::gl.TexImage2D(gl::GL_TEXTURE_2D, 0, static_cast<gl::GLint>(gl::GL_RGBA8), static_cast<gl::GLsizei>(g_width),
                    static_cast<gl::GLsizei>(g_height), 0, gl::GL_RGBA, gl::GL_UNSIGNED_BYTE, nullptr);
  // EGL_KHR_gl_texture_2D_image requires a complete texture; clamp it to the single level we defined.
  gl::gl.TexParameteri(gl::GL_TEXTURE_2D, gl::GL_TEXTURE_MAX_LEVEL, 0);
  gl::gl.TexParameteri(gl::GL_TEXTURE_2D, gl::GL_TEXTURE_MIN_FILTER, gl::GL_NEAREST);
  gl::gl.TexParameteri(gl::GL_TEXTURE_2D, gl::GL_TEXTURE_MAG_FILTER, gl::GL_NEAREST);
  gl::gl.TexParameteri(gl::GL_TEXTURE_2D, gl::GL_TEXTURE_WRAP_S, gl::GL_CLAMP_TO_EDGE);
  gl::gl.TexParameteri(gl::GL_TEXTURE_2D, gl::GL_TEXTURE_WRAP_T, gl::GL_CLAMP_TO_EDGE);
  gl::gl.BindTexture(gl::GL_TEXTURE_2D, 0);
  if (const auto err = gl::gl.GetError(); err != gl::GL_NO_ERROR) {
    Log.error("[sdl2shim-efb] shim texture allocation failed (GL 0x{:x})", err);
    return false;
  }

  const gl::EGLint imageAttribs[] = {
      static_cast<gl::EGLint>(gl::EGL_GL_TEXTURE_LEVEL_KHR), 0,
      static_cast<gl::EGLint>(gl::EGL_IMAGE_PRESERVED_KHR),  1,
      static_cast<gl::EGLint>(gl::EGL_NONE),
  };
  slot.eglImage =
      gl::gl.eglCreateImageKHR(g_display, g_shimContext, gl::EGL_GL_TEXTURE_2D_KHR,
                               reinterpret_cast<gl::EGLClientBuffer>(static_cast<uintptr_t>(slot.shimTexture)),
                               imageAttribs);
  if (slot.eglImage == nullptr) {
    Log.error("[sdl2shim-efb] eglCreateImageKHR failed (egl 0x{:x})",
              gl::gl.eglGetError != nullptr ? gl::gl.eglGetError() : 0);
    return false;
  }

  gl::gl.GenFramebuffers(1, &slot.shimReadFbo);
  gl::gl.BindFramebuffer(gl::GL_FRAMEBUFFER, slot.shimReadFbo);
  gl::gl.FramebufferTexture2D(gl::GL_FRAMEBUFFER, gl::GL_COLOR_ATTACHMENT0, gl::GL_TEXTURE_2D, slot.shimTexture, 0);
  const auto status = gl::gl.CheckFramebufferStatus(gl::GL_FRAMEBUFFER);
  gl::gl.BindFramebuffer(gl::GL_FRAMEBUFFER, 0);
  if (status != gl::GL_FRAMEBUFFER_COMPLETE) {
    Log.error("[sdl2shim-efb] shim read FBO incomplete (0x{:x})", status);
    return false;
  }
  return true;
}

// Main thread, shim context current: free the shim-side objects. The worker-side texture/FBO belong
// to the render context and are freed with it at context teardown (we can't touch them from here) --
// but ensure_worker_slots deletes any stale ones from the worker before re-aliasing, so a resize
// does not leak them.
void destroy_shim_slot(Slot& slot) {
  destroy_sync(slot.fwdSync);
  destroy_sync(slot.revSync);
  if (slot.eglImage != nullptr && gl::gl.eglDestroyImageKHR != nullptr) {
    gl::gl.eglDestroyImageKHR(g_display, slot.eglImage);
    slot.eglImage = nullptr;
  }
  if (slot.shimReadFbo != 0) {
    gl::gl.DeleteFramebuffers(1, &slot.shimReadFbo);
    slot.shimReadFbo = 0;
  }
  if (slot.shimTexture != 0) {
    gl::gl.DeleteTextures(1, &slot.shimTexture);
    slot.shimTexture = 0;
  }
  slot.state = SlotState::Free;
}

// Worker thread, render context current: alias each slot's EGLImage into a worker-context texture
// wrapped in an FBO the present composite renders into. Idempotent; re-runs after a resize (which
// resets g_workerAttached), deleting the stale worker objects first.
bool ensure_worker_slots() {
  if (g_workerAttached) {
    return true;
  }
  if (gl::gl.glEGLImageTargetTexture2DOES == nullptr) {
    Log.error("[sdl2shim-efb] glEGLImageTargetTexture2DOES unavailable; cannot alias EFB slots");
    return false;
  }
  // GL errors are sticky, and this function used to check GetError() once at the END of the alias
  // sequence -- so ANY error latched earlier by unrelated worker-context code was blamed on the
  // alias, which permanently disabled presentation (game runs, black screen). That is exactly what
  // happened on an RK3562/Mali-G52 box (blob g29p1): a stray GL_INVALID_ENUM was already pending,
  // the alias itself was fine. Drain before measuring so we only judge our own calls.
  int drained = 0;
  for (auto err = gl::gl.GetError(); err != gl::GL_NO_ERROR && drained < 16; err = gl::gl.GetError()) {
    Log.warn("[sdl2shim-efb] drained stale GL error 0x{:x} before aliasing (not ours)", err);
    ++drained;
  }
  for (Slot& slot : g_slots) {
    if (slot.workerFbo != 0) {
      gl::gl.DeleteFramebuffers(1, &slot.workerFbo);
      slot.workerFbo = 0;
    }
    if (slot.workerTexture != 0) {
      gl::gl.DeleteTextures(1, &slot.workerTexture);
      slot.workerTexture = 0;
    }
    if (slot.eglImage == nullptr) {
      Log.error("[sdl2shim-efb] slot has no EGLImage to alias");
      return false;
    }
    gl::gl.GenTextures(1, &slot.workerTexture);
    gl::gl.BindTexture(gl::GL_TEXTURE_2D, slot.workerTexture);
    gl::gl.glEGLImageTargetTexture2DOES(gl::GL_TEXTURE_2D, slot.eglImage);
    // Judge the alias call on its own: this is the one that can legitimately fail.
    if (const auto aliasErr = gl::gl.GetError(); aliasErr != gl::GL_NO_ERROR) {
      Log.error("[sdl2shim-efb] glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, {}) raised 0x{:x}", slot.eglImage,
                aliasErr);
      return false;
    }
    gl::gl.TexParameteri(gl::GL_TEXTURE_2D, gl::GL_TEXTURE_MIN_FILTER, gl::GL_NEAREST);
    gl::gl.TexParameteri(gl::GL_TEXTURE_2D, gl::GL_TEXTURE_MAG_FILTER, gl::GL_NEAREST);
    gl::gl.BindTexture(gl::GL_TEXTURE_2D, 0);
    if (const auto err = gl::gl.GetError(); err != gl::GL_NO_ERROR) {
      Log.error("[sdl2shim-efb] worker slot texture setup failed (GL 0x{:x})", err);
      return false;
    }
    gl::gl.GenFramebuffers(1, &slot.workerFbo);
    gl::gl.BindFramebuffer(gl::GL_FRAMEBUFFER, slot.workerFbo);
    gl::gl.FramebufferTexture2D(gl::GL_FRAMEBUFFER, gl::GL_COLOR_ATTACHMENT0, gl::GL_TEXTURE_2D, slot.workerTexture, 0);
    const auto status = gl::gl.CheckFramebufferStatus(gl::GL_FRAMEBUFFER);
    gl::gl.BindFramebuffer(gl::GL_FRAMEBUFFER, 0);
    if (status != gl::GL_FRAMEBUFFER_COMPLETE) {
      Log.error("[sdl2shim-efb] worker FBO incomplete (0x{:x})", status);
      return false;
    }
  }
  g_workerAttached = true;
  Log.info("[sdl2shim-efb] worker aliased {} EFB slots into FBOs", SlotCount);
  return true;
}

// Blit the finished slot into the window's default framebuffer and swap. Main thread, shim context.
void blit_and_swap(Slot& slot) {
  ZoneScopedN("EFB blit + swap");
  SDL_Window* window = window::get_sdl_window();
  int drawableWidth = static_cast<int>(g_width);
  int drawableHeight = static_cast<int>(g_height);
  SDL_GetWindowSizeInPixels(window, &drawableWidth, &drawableHeight);

  // Wait, on the GPU, for the worker to finish compositing this slot. This does not block the CPU.
  if (slot.fwdSync != nullptr && gl::gl.eglWaitSyncKHR != nullptr) {
    gl::gl.eglWaitSyncKHR(g_display, slot.fwdSync, 0);
  }

  gl::gl.BindFramebuffer(gl::GL_READ_FRAMEBUFFER, slot.shimReadFbo);
  gl::gl.BindFramebuffer(gl::GL_DRAW_FRAMEBUFFER, 0);
  gl::gl.Disable(gl::GL_SCISSOR_TEST);
  // No Y-flip (S1c): the worker composited the frame GL-native (row 0 = bottom) into the slot and
  // the default framebuffer is also bottom-left origin, so a straight blit is upright.
  gl::gl.BlitFramebuffer(0, 0, static_cast<gl::GLint>(g_width), static_cast<gl::GLint>(g_height), 0, 0, drawableWidth,
                         drawableHeight, gl::GL_COLOR_BUFFER_BIT, gl::GL_LINEAR);
  gl::gl.BindFramebuffer(gl::GL_READ_FRAMEBUFFER, 0);

  if (g_sdl2SwapWindow != nullptr && g_sdl2Window != nullptr) {
    g_sdl2SwapWindow(g_sdl2Window);
  } else {
    SDL_GL_SwapWindow(window);
  }
}

} // namespace

bool active() noexcept { return g_active; }

bool initialize(void* eglDisplay, uint32_t width, uint32_t height, gl::TextureFormat format) {
  if (g_active) {
    return true;
  }
  if (eglDisplay == nullptr || width == 0 || height == 0) {
    return false;
  }
  // The shared textures are allocated as GL_RGBA8, so the present composite must render that format.
  if (format != gl::TextureFormat::RGBA8Unorm) {
    Log.error("[sdl2shim-efb] unsupported present format {}; EFB present disabled", magic_enum::enum_name(format));
    return false;
  }
  if (gl::gl.eglCreateImageKHR == nullptr || gl::gl.eglGetCurrentContext == nullptr) {
    Log.error("[sdl2shim-efb] required EGL entry points unavailable; EFB present disabled");
    return false;
  }
  g_display = eglDisplay;
  g_width = width;
  g_height = height;

  g_shimContext = gl::gl.eglGetCurrentContext();
  if (g_shimContext == nullptr) {
    Log.error("[sdl2shim-efb] no EGL context current on the main thread; EFB present disabled");
    return false;
  }

  const SDL_PropertiesID props = SDL_GetWindowProperties(window::get_sdl_window());
  g_sdl2Window = SDL_GetPointerProperty(props, SDL2_SHIM_WINDOW_PROP, nullptr);
  g_sdl2SwapWindow =
      reinterpret_cast<Sdl2SwapWindowFn>(SDL_GetPointerProperty(props, SDL2_SHIM_GL_SWAP_WINDOW_PROP, nullptr));

  for (uint32_t i = 0; i < SlotCount; ++i) {
    if (!create_shim_slot(g_slots[i])) {
      for (uint32_t j = 0; j <= i; ++j) {
        destroy_shim_slot(g_slots[j]);
      }
      return false;
    }
    g_slots[i].state = SlotState::Free;
  }

  g_nextSlot = 0;
  g_framesOwed = 0;
  g_hasReadyFrame = false;
  g_workerAttached = false;
  g_aborting = false;
  g_active = true;
  Log.info("[sdl2shim-efb] {}x{} {} EFB slots via EGLImage; present swap via {}; worker aliases on first frame",
           width, height, SlotCount,
           g_sdl2SwapWindow != nullptr && g_sdl2Window != nullptr ? "firmware SDL2 SDL_GL_SwapWindow"
                                                                  : "SDL3 SDL_GL_SwapWindow");
  return true;
}

bool resize(uint32_t width, uint32_t height, gl::TextureFormat format) {
  if (!g_active) {
    return false;
  }
  if (width == g_width && height == g_height) {
    return true;
  }
  if (width == 0 || height == 0 || format != gl::TextureFormat::RGBA8Unorm) {
    return false;
  }

  // Callers reach here through gfx::gpu_synchronize(), so the worker is idle and both slots are
  // ours to tear down (shim side). The worker re-aliases its side on the next acquire.
  for (Slot& slot : g_slots) {
    destroy_shim_slot(slot);
  }
  g_width = width;
  g_height = height;
  g_nextSlot = 0;
  g_framesOwed = 0;
  g_hasReadyFrame = false;
  g_workerAttached = false;

  for (uint32_t i = 0; i < SlotCount; ++i) {
    if (!create_shim_slot(g_slots[i])) {
      Log.error("[sdl2shim-efb] failed to reallocate EFB slots at {}x{}; presentation is now disabled", width,
                height);
      g_active = false;
      return false;
    }
    g_slots[i].state = SlotState::Free;
  }
  Log.info("[sdl2shim-efb] reallocated EFB slots at {}x{}", width, height);
  return true;
}

void shutdown() {
  if (!g_active) {
    return;
  }
  {
    std::lock_guard lock{g_mutex};
    g_aborting = true;
  }
  g_slotFreed.notify_all();
  g_frameReady.notify_all();

  g_active = false;
  for (Slot& slot : g_slots) {
    destroy_shim_slot(slot);
  }
  g_hasReadyFrame = false;
  g_framesOwed = 0;
  g_workerAttached = false;
  g_display = nullptr;
  g_shimContext = nullptr;
  g_sdl2Window = nullptr;
  g_sdl2SwapWindow = nullptr;
}

std::optional<AcquiredFrame> acquire() {
  if (!g_active) {
    return std::nullopt;
  }
  ZoneScopedN("EFB acquire");
  // The worker owns the render context here; alias the slots into worker FBOs on the first frame.
  if (!ensure_worker_slots()) {
    return std::nullopt;
  }
  const uint32_t slotIndex = g_nextSlot;
  {
    std::unique_lock lock{g_mutex};
    // Wait for the main thread to finish scanning this slot out. In steady state it already has.
    g_slotFreed.wait(lock, [&] { return g_aborting || g_slots[slotIndex].state == SlotState::Free; });
    if (g_aborting) {
      return std::nullopt;
    }
    g_slots[slotIndex].state = SlotState::Rendering;
  }

  Slot& slot = g_slots[slotIndex];
  // Wait, on the CPU and with no EGL context held, until the main thread's blit of this slot has
  // retired on the GPU. This must NOT be a server wait (eglWaitSync into the worker's stream): that
  // dams the worker's GL commands behind a fence that only signals after main-thread present
  // activity, so the worker's next GL call blocks inside libmali while holding the render-context
  // mutex, and any game-thread GL call (vertex-cache upload, texture creation) then closes a
  // deadlock cycle against it -- the gdb-verified Mali-G31 hang. eglClientWaitSyncKHR needs no
  // current context, so the worker parks holding nothing the other threads can contend on. In
  // steady state the fence retired a frame ago and this returns immediately; on timeout we render
  // anyway (one torn frame, loudly).
  if (slot.revSync != nullptr && gl::gl.eglClientWaitSyncKHR != nullptr) {
    const int32_t waited = gl::gl.eglClientWaitSyncKHR(g_display, slot.revSync, 0, RevSyncWaitTimeoutNs);
    if (waited != gl::EGL_CONDITION_SATISFIED_KHR) {
      Log.warn("[sdl2shim-efb] slot {} reverse-fence wait returned 0x{:x}; rendering into it anyway", slotIndex,
               waited);
    }
    destroy_sync(slot.revSync);
  }
  g_nextSlot = (slotIndex + 1) % SlotCount;
  return AcquiredFrame{.slot = slotIndex, .fbo = slot.workerFbo};
}

void publish(uint32_t slot) {
  if (!g_active || slot >= SlotCount) {
    return;
  }
  // Fence the worker's just-composited frame so the shim context can order its blit behind it.
  g_slots[slot].fwdSync = create_fence();

  // The main thread consumes the previous frame before enqueueing the work that produces this one,
  // so the mailbox is always empty here. If that invariant ever breaks, drop the stale frame rather
  // than overwrite it -- an overwritten entry would leave its slot Ready forever and wedge acquire().
  uint32_t stale = SlotCount;
  {
    std::lock_guard lock{g_mutex};
    if (g_hasReadyFrame && g_readySlot != slot) {
      stale = g_readySlot;
    }
    g_slots[slot].state = SlotState::Ready;
    g_readySlot = slot;
    g_hasReadyFrame = true;
  }

  if (stale < SlotCount) {
    Log.warn("[sdl2shim-efb] dropping unpresented frame in slot {}", stale);
    Slot& staleSlot = g_slots[stale];
    destroy_sync(staleSlot.fwdSync);
    {
      std::lock_guard lock{g_mutex};
      staleSlot.state = SlotState::Free;
      if (g_framesOwed > 0) {
        --g_framesOwed;
      }
    }
    g_slotFreed.notify_one();
  }

  g_frameReady.notify_one();
}

void publish_empty() {
  if (!g_active) {
    return;
  }
  {
    std::lock_guard lock{g_mutex};
    if (g_framesOwed > 0) {
      --g_framesOwed;
    }
  }
  g_frameReady.notify_one();
}

void note_frame_enqueued() {
  if (!g_active) {
    return;
  }
  std::lock_guard lock{g_mutex};
  ++g_framesOwed;
}

void flush_present() {
  if (!g_active) {
    return;
  }
  ZoneScopedN("EFB flush present");

  uint32_t slotIndex = 0;
  {
    std::unique_lock lock{g_mutex};
    if (g_framesOwed <= 0 && !g_hasReadyFrame) {
      return; // nothing in flight (first frame, or the worker reported an empty frame)
    }
    // g_framesOwed drops to zero when the worker reports a frame it could not present, which is
    // also a reason to stop waiting.
    const auto ready = [&] { return g_aborting || g_hasReadyFrame || g_framesOwed <= 0; };
    if (!g_frameReady.wait_for(lock, PresentWaitTimeout, ready)) {
      Log.warn("[sdl2shim-efb] timed out waiting for the render worker to submit a frame");
      return;
    }
    if (g_aborting || !g_hasReadyFrame) {
      return;
    }
    slotIndex = g_readySlot;
    g_hasReadyFrame = false;
    if (g_framesOwed > 0) {
      --g_framesOwed;
    }
  }

  Slot& slot = g_slots[slotIndex];
  const bool presentable = window::is_presentable();
  if (presentable) {
    blit_and_swap(slot);
  }
  destroy_sync(slot.fwdSync);

  {
    std::lock_guard lock{g_mutex};
    // Only fence the slot back if we actually read from it; otherwise the worker has nothing to
    // wait on and can reuse it immediately.
    slot.revSync = presentable ? create_fence() : nullptr;
    slot.state = SlotState::Free;
  }
  g_slotFreed.notify_one();

  if (presentable) {
    gfx::after_present();
  }
}

void recycle_pending() {
  if (!g_active) {
    return;
  }
  uint32_t slotIndex = 0;
  {
    std::lock_guard lock{g_mutex};
    if (!g_hasReadyFrame) {
      return;
    }
    slotIndex = g_readySlot;
    g_hasReadyFrame = false;
    if (g_framesOwed > 0) {
      --g_framesOwed;
    }
  }

  // Dropped without a read, so the worker needs no reverse fence to reuse the slot.
  Slot& slot = g_slots[slotIndex];
  destroy_sync(slot.fwdSync);
  {
    std::lock_guard lock{g_mutex};
    slot.state = SlotState::Free;
  }
  g_slotFreed.notify_one();
}

} // namespace aurora::webgpu::sdl2shim_present

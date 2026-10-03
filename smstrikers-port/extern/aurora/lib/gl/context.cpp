#include "context.hpp"

#include "../internal.hpp"

#include <SDL3/SDL_video.h>

namespace aurora::gl {
namespace {
Module Log("aurora::gl");

ContextMode g_mode = ContextMode::Desktop;

// Desktop (SDL_GL) state
SDL_Window* g_window = nullptr;
SDL_GLContext g_renderCtx = nullptr;
SDL_GLContext g_shareCtx = nullptr;

// Device (raw EGL) state. The shim's borrowed EGLDisplay/context stay owned by the
// shim and current on the main thread; these render/share contexts share objects
// with the shim's context and are bound (once, forever) on the worker / compiler
// threads via raw EGL. Each context needs its own current surface, so we give each a
// 1x1 pbuffer (offscreen -- we render to FBOs / EFB slots, never a window surface);
// EGL_NO_SURFACE is the surfaceless fallback if a pbuffer can't be made.
EGLDisplay g_eglDisplay = nullptr;
EGLConfig g_eglConfig = nullptr;
EGLContext g_eglRenderCtx = nullptr;
EGLContext g_eglShareCtx = nullptr;
EGLSurface g_eglRenderSurface = EGL_NO_SURFACE;
EGLSurface g_eglShareSurface = EGL_NO_SURFACE;

// SDL_GL_GetProcAddress returns SDL_FunctionPointer (void(*)()); adapt to our
// void*(const char*) loader signature.
void* desktop_get_proc(const char* name) { return reinterpret_cast<void*>(SDL_GL_GetProcAddress(name)); }

bool create_desktop(const ContextConfig& cfg) {
  g_window = static_cast<SDL_Window*>(cfg.sdlWindow);
  if (g_window == nullptr) {
    Log.error("[gl] desktop context: no SDL window");
    return false;
  }
  // ES 3.0, offscreen rendering (we never draw to the window's own buffers --
  // present is an explicit blit), so no depth/stencil on the default framebuffer.
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
  SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

  g_renderCtx = SDL_GL_CreateContext(g_window);
  if (g_renderCtx == nullptr) {
    Log.error("[gl] SDL_GL_CreateContext (render) failed: {}", SDL_GetError());
    return false;
  }
  SDL_GL_MakeCurrent(g_window, g_renderCtx);
  SDL_GL_SetSwapInterval(0);

  // A second context sharing objects with the render context, for the compiler
  // thread. SDL makes a freshly created context current, so restore render after.
  SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
  g_shareCtx = SDL_GL_CreateContext(g_window);
  if (g_shareCtx == nullptr) {
    // Non-fatal: the pipeline cache falls back to threadless (compile on worker).
    Log.warn("[gl] SDL_GL_CreateContext (share) failed: {}; compiler thread disabled", SDL_GetError());
  }
  SDL_GL_MakeCurrent(g_window, g_renderCtx);

  if (!load(desktop_get_proc)) {
    return false;
  }
  const char* vendor = reinterpret_cast<const char*>(gl.GetString(GL_VENDOR));
  const char* renderer = reinterpret_cast<const char*>(gl.GetString(GL_RENDERER));
  const char* version = reinterpret_cast<const char*>(gl.GetString(GL_VERSION));
  Log.info("[gl] desktop context up: {} / {} / {}",
           vendor ? vendor : "unknown",
           renderer ? renderer : "unknown",
           version ? version : "unknown");
  return true;
}

// Choose a pbuffer-capable, ES3-renderable RGBA8 config for the dummy surfaces. Tries the
// ES3 renderable bit first, then ES2 (some drivers advertise ES3 only via the ES2 bit).
bool choose_pbuffer_config(EGLDisplay display, EGLConfig& out) {
  const auto tryChoose = [&](EGLint renderableBit) {
    const EGLint attribs[] = {
        EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, renderableBit, EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE,      8,               EGL_BLUE_SIZE,       8,             EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE,      0,               EGL_STENCIL_SIZE,    0,             EGL_NONE,
    };
    EGLConfig config = nullptr;
    EGLint numConfigs = 0;
    return gl.eglChooseConfig(display, attribs, &config, 1, &numConfigs) == EGL_TRUE && numConfigs >= 1
               ? (out = config, true)
               : false;
  };
  return tryChoose(EGL_OPENGL_ES3_BIT) || tryChoose(EGL_OPENGL_ES2_BIT);
}

// Create a 1x1 pbuffer for a context's dummy current-surface. Returns EGL_NO_SURFACE on failure;
// the caller falls back to surfaceless make-current (EGL_KHR_surfaceless_context).
EGLSurface create_dummy_pbuffer(EGLDisplay display, EGLConfig config) {
  const EGLint attribs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
  return gl.eglCreatePbufferSurface(display, config, attribs);
}

bool create_device(const ContextConfig& cfg) {
  if (cfg.eglDisplay == nullptr || cfg.shimGetProc == nullptr) {
    Log.error("[gl] sdl2-shim context: missing EGLDisplay or getProc");
    return false;
  }
  g_eglDisplay = cfg.eglDisplay;

  // The shim's borrowed context is current on this (main) thread right now, so the proc
  // table resolves against it. The entry points are display-global, so loading here and
  // using them from the worker/compiler contexts later is fine.
  if (!load(cfg.shimGetProc)) {
    Log.error("[gl] sdl2-shim context: failed to load required GL/EGL entry points");
    return false;
  }
  if (gl.eglCreateContext == nullptr || gl.eglChooseConfig == nullptr || gl.eglMakeCurrent == nullptr ||
      gl.eglCreatePbufferSurface == nullptr) {
    Log.error("[gl] sdl2-shim context: EGL context-management entry points unavailable");
    return false;
  }

  if (!choose_pbuffer_config(g_eglDisplay, g_eglConfig)) {
    Log.error("[gl] sdl2-shim context: no pbuffer/ES3 EGLConfig (eglChooseConfig failed)");
    return false;
  }

  const EGLint ctxAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
  // Share with the shim's context so the shim, worker and compiler are one share group.
  g_eglRenderCtx = gl.eglCreateContext(g_eglDisplay, g_eglConfig, cfg.shareEglContext, ctxAttribs);
  if (g_eglRenderCtx == nullptr) {
    Log.error("[gl] sdl2-shim context: eglCreateContext (render) failed (egl 0x{:x})",
              gl.eglGetError != nullptr ? gl.eglGetError() : 0);
    return false;
  }
  g_eglRenderSurface = create_dummy_pbuffer(g_eglDisplay, g_eglConfig);
  if (g_eglRenderSurface == EGL_NO_SURFACE) {
    Log.warn("[gl] sdl2-shim context: render pbuffer unavailable; relying on surfaceless make-current");
  }

  // The compiler thread's share context. Non-fatal: without it the pipeline cache compiles
  // inline on the worker (threadless), same as the desktop share-failure path.
  g_eglShareCtx = gl.eglCreateContext(g_eglDisplay, g_eglConfig, g_eglRenderCtx, ctxAttribs);
  if (g_eglShareCtx == nullptr) {
    Log.warn("[gl] sdl2-shim context: share context creation failed (egl 0x{:x}); compiler thread disabled",
             gl.eglGetError != nullptr ? gl.eglGetError() : 0);
  } else {
    g_eglShareSurface = create_dummy_pbuffer(g_eglDisplay, g_eglConfig);
  }

  // Leave the shim's context current on the main thread (it owns present/blit-and-swap); the
  // worker binds g_eglRenderCtx via make_render_current() when it comes online.
  Log.info("[gl] sdl2-shim (device) contexts up: render={} share={} (display={})",
           static_cast<void*>(g_eglRenderCtx), static_cast<void*>(g_eglShareCtx), g_eglDisplay);
  return true;
}
} // namespace

bool create_contexts(const ContextConfig& cfg) {
  g_mode = cfg.mode;
  switch (cfg.mode) {
  case ContextMode::Desktop:
    return create_desktop(cfg);
  case ContextMode::Sdl2Shim:
    return create_device(cfg);
  }
  return false;
}

bool make_render_current() {
  if (g_mode == ContextMode::Desktop) {
    return SDL_GL_MakeCurrent(g_window, g_renderCtx);
  }
  return gl.eglMakeCurrent(g_eglDisplay, g_eglRenderSurface, g_eglRenderSurface, g_eglRenderCtx) == EGL_TRUE;
}

bool make_share_current() {
  if (g_mode == ContextMode::Desktop) {
    return g_shareCtx != nullptr && SDL_GL_MakeCurrent(g_window, g_shareCtx);
  }
  return g_eglShareCtx != nullptr &&
         gl.eglMakeCurrent(g_eglDisplay, g_eglShareSurface, g_eglShareSurface, g_eglShareCtx) == EGL_TRUE;
}

bool make_none_current() {
  if (g_mode == ContextMode::Desktop) {
    return SDL_GL_MakeCurrent(g_window, nullptr);
  }
  // EGL_NO_CONTEXT is an EGLint in the enum table; eglMakeCurrent wants an EGLContext (void*).
  return gl.eglMakeCurrent(g_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, nullptr) == EGL_TRUE;
}

bool has_share_context() {
  if (g_mode == ContextMode::Desktop) {
    return g_shareCtx != nullptr;
  }
  return g_eglShareCtx != nullptr;
}

void destroy_contexts() {
  if (g_mode == ContextMode::Desktop) {
    SDL_GL_MakeCurrent(g_window, nullptr);
    if (g_shareCtx != nullptr) {
      SDL_GL_DestroyContext(g_shareCtx);
      g_shareCtx = nullptr;
    }
    if (g_renderCtx != nullptr) {
      SDL_GL_DestroyContext(g_renderCtx);
      g_renderCtx = nullptr;
    }
    return;
  }
  // Device: tear down only what we own (not the shim's display/context). Each context is
  // current on its own thread; those threads have already stopped by shutdown time.
  if (g_eglDisplay != nullptr && gl.eglDestroyContext != nullptr) {
    if (g_eglShareSurface != EGL_NO_SURFACE) {
      gl.eglDestroySurface(g_eglDisplay, g_eglShareSurface);
      g_eglShareSurface = EGL_NO_SURFACE;
    }
    if (g_eglShareCtx != nullptr) {
      gl.eglDestroyContext(g_eglDisplay, g_eglShareCtx);
      g_eglShareCtx = nullptr;
    }
    if (g_eglRenderSurface != EGL_NO_SURFACE) {
      gl.eglDestroySurface(g_eglDisplay, g_eglRenderSurface);
      g_eglRenderSurface = EGL_NO_SURFACE;
    }
    if (g_eglRenderCtx != nullptr) {
      gl.eglDestroyContext(g_eglDisplay, g_eglRenderCtx);
      g_eglRenderCtx = nullptr;
    }
  }
  g_eglDisplay = nullptr;
}

ContextMode current_mode() { return g_mode; }

} // namespace aurora::gl

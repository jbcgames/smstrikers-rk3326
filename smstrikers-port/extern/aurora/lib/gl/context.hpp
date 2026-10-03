#pragma once

// GL context creation and per-thread ownership.
//
// Normalcy Doctrine rule 3: one context per thread, created once, never
// re-bound mid-run. We create two sharing contexts:
//   - render context: owned by the render worker for its whole life
//   - share  context: owned by the pipeline-compiler thread for its whole life
// On the device (sdl2 shim) path the main thread additionally keeps the shim's
// own borrowed context; all binding there is via raw EGL, never SDL_GL_MakeCurrent
// (SDL's TLS bookkeeping desyncs vs the raw eglMakeCurrent Dawn/we issue -- the
// #79 lesson).

#include "gl_core.hpp"

namespace aurora::gl {

enum class ContextMode {
  Desktop,  // SDL_GL context on an SDL_WINDOW_OPENGL window (dev box)
  Sdl2Shim, // raw EGL context on the shim's published EGLDisplay (Mali/PowerVR)
};

struct ContextConfig {
  ContextMode mode = ContextMode::Desktop;
  void* sdlWindow = nullptr;         // SDL_Window*  (desktop)
  void* eglDisplay = nullptr;        // shim EGLDisplay (device)
  void* shareEglContext = nullptr;   // shim's context to share objects with (device)
  ProcAddressFn shimGetProc = nullptr; // shim gl_get_proc (device); SDL_GL_GetProcAddress on desktop
};

// Creates the render + share contexts and calls gl::load() with the render
// context current on the calling (main) thread. Returns false on any failure.
bool create_contexts(const ContextConfig& cfg);

// Bind the named context on the calling thread. The render worker calls
// make_render_current() once at startup; the compiler thread make_share_current().
bool make_render_current();
bool make_share_current();
bool make_none_current();

// True if a share context exists (compiler-thread GLSL compile is possible). When
// false, the pipeline cache must compile inline on the render worker (threadless).
bool has_share_context();

void destroy_contexts();

ContextMode current_mode();

} // namespace aurora::gl

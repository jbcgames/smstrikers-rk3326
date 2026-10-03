#pragma once

// Top-level entry for the hand-rolled GLES backend. This file grows into the
// replacement for lib/webgpu/gpu.cpp across the phases (initialize/shutdown, caps,
// framebuffer targets, present). Named `backend` rather than `device` to avoid
// confusion with aurora's unrelated lib/device.cpp (input devices).

namespace aurora::gl {

// Master gate for the GL backend during bring-up.
//
// Phase 0 keeps this false: the lib/gl/ code compiles and links into aurora_core
// but Dawn remains the live backend and nothing calls into lib/gl/. Phase 1 flips
// the initialize() path over to GL. Kept as a constexpr, not an env var, per the
// no-new-env-flags rule.
inline constexpr bool kGlBackendSmoke = false;

// Standalone loader/context bring-up probe: creates a desktop GL context on the
// given SDL_Window*, fills the proc table, logs the driver strings, and tears
// down. Never called in a normal boot; flip kGlBackendSmoke locally to validate
// the loader + context path in isolation from the render path. Returns success.
bool smoke(void* sdlWindow);

} // namespace aurora::gl

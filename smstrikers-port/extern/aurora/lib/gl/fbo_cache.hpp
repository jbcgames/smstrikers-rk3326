#pragma once

// Framebuffer-object cache, keyed by attachment set.
//
// A tiler wants a stable FBO per (color, depth) attachment pair so it can track
// tile state across a pass instead of re-validating a fresh FBO each time -- and
// it keeps our GL traffic looking like a normal engine's (Normalcy Doctrine).
// The set of live attachment pairs is tiny (EFB color+depth, a few offscreen /
// snapshot targets), so a linear-scanned vector is the right structure.
//
// Scaffolded beside Dawn: nothing calls this until the Phase 2 pass skeleton.

#include "gl_core.hpp"
#include "handles.hpp"

namespace aurora::gl {

// Return a complete FBO with `color` attached at COLOR_ATTACHMENT0 and, when
// `depth` is non-null, attached at the DEPTH (or DEPTH_STENCIL, for a packed
// format) attachment point. Cached by attachment ids; repeated calls with the
// same pair return the same GL name. Leaves the returned FBO bound to
// GL_FRAMEBUFFER. Requires the owning context current.
GLuint get_framebuffer(const Texture& color, const Texture* depth = nullptr);

// Drop every cached FBO (glDeleteFramebuffers). Call on swapchain resize and
// shutdown, before the attachment textures themselves are destroyed.
void clear_framebuffer_cache() noexcept;

// Evict any cached FBO that references `textureId` (call just before destroying a
// texture so a recycled GL name can't alias a stale attachment).
void invalidate_framebuffers_for(GLuint textureId) noexcept;

} // namespace aurora::gl

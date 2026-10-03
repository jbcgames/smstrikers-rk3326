#include "fbo_cache.hpp"

#include "census.hpp"
#include "../internal.hpp"

#include <vector>

namespace aurora::gl {
namespace {
Module Log("aurora::gl");

struct FboEntry {
  GLuint color = 0;
  GLuint depth = 0;
  GLuint fbo = 0;
};
std::vector<FboEntry> s_entries;

GLenum depth_attachment_point(const Texture& depth) {
  return depth.format == TextureFormat::Depth24PlusStencil8 ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT;
}
} // namespace

GLuint get_framebuffer(const Texture& color, const Texture* depth) {
  const GLuint depthId = depth != nullptr ? depth->id : 0;
  for (const auto& e : s_entries) {
    if (e.color == color.id && e.depth == depthId) {
      gl.BindFramebuffer(GL_FRAMEBUFFER, e.fbo);
      return e.fbo;
    }
  }

  GLuint fbo = 0;
  gl.GenFramebuffers(1, &fbo);
  gl.BindFramebuffer(GL_FRAMEBUFFER, fbo);
  gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color.id, 0);
  if (depth != nullptr && depth->id != 0) {
    gl.FramebufferTexture2D(GL_FRAMEBUFFER, depth_attachment_point(*depth), GL_TEXTURE_2D, depth->id, 0);
  }
  const GLenum status = gl.CheckFramebufferStatus(GL_FRAMEBUFFER);
  if (status != GL_FRAMEBUFFER_COMPLETE) {
    Log.error("get_framebuffer: incomplete FBO (color={}, depth={}) status={:#x}", color.id, depthId, status);
  }
  census::fbos.add(0);
  s_entries.push_back({color.id, depthId, fbo});
  return fbo;
}

void clear_framebuffer_cache() noexcept {
  for (auto& e : s_entries) {
    if (e.fbo != 0) {
      census::fbos.sub(0);
      gl.DeleteFramebuffers(1, &e.fbo);
    }
  }
  s_entries.clear();
}

void invalidate_framebuffers_for(GLuint textureId) noexcept {
  if (textureId == 0) {
    return;
  }
  for (auto it = s_entries.begin(); it != s_entries.end();) {
    if (it->color == textureId || it->depth == textureId) {
      if (it->fbo != 0) {
        census::fbos.sub(0);
        gl.DeleteFramebuffers(1, &it->fbo);
      }
      it = s_entries.erase(it);
    } else {
      ++it;
    }
  }
}

} // namespace aurora::gl

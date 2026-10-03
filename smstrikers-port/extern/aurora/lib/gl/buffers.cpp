#include "buffers.hpp"

#include "census.hpp"
#include "state.hpp"
#include "../internal.hpp"
#include "../webgpu/gpu.hpp"

#include <cstdint>
#include <cstring>

namespace aurora::gl {
namespace {
Module Log("aurora::gl");
} // namespace

Buffer create_buffer(GLenum target, uint64_t size, bool dynamic, bool persistent) {
  GLuint id = 0;
  gl.GenBuffers(1, &id);
  gl.BindBuffer(target, id);

  void* mapped = nullptr;
  if (persistent && webgpu::g_bufferStorageSupported && gl.BufferStorage != nullptr && gl.MapBufferRange != nullptr) {
    // Immutable persistent-coherent write-combine storage. GL_DYNAMIC_STORAGE_BIT is deliberately
    // kept so that, if the mapping below fails, glBufferSubData is still legal on this buffer (the
    // upload_buffer fallback path). GL_MAP_COHERENT_BIT means CPU writes reach the GPU with no
    // glFlushMappedBufferRange.
    constexpr GLbitfield kStorageFlags =
        GL_DYNAMIC_STORAGE_BIT | GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
    constexpr GLbitfield kMapFlags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
    gl.BufferStorage(target, static_cast<GLsizeiptr>(size), nullptr, kStorageFlags);
    mapped = gl.MapBufferRange(target, 0, static_cast<GLsizeiptr>(size), kMapFlags);
    if (mapped == nullptr) {
      // Storage is immutable now, but DYNAMIC_STORAGE keeps glBufferSubData valid -- leave mapped
      // null and let upload_buffer take the glBufferSubData branch.
      Log.warn("[gl] persistent map failed for {}-byte buffer; using glBufferSubData on immutable storage", size);
    }
  } else {
    gl.BufferData(target, static_cast<GLsizeiptr>(size), nullptr, dynamic ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);
  }
  census::buffers.add(static_cast<int64_t>(size));
  return Buffer{.id = id, .target = target, .size = size, .mapped = mapped};
}

void upload_buffer(const Buffer& buffer, uint64_t offset, const void* data, uint64_t size) {
  if (buffer.id == 0 || size == 0) {
    return;
  }
  if (buffer.mapped != nullptr) {
    // Coherent persistent mapping: the memcpy IS the upload -- no bind, no glBufferSubData, no
    // driver-side copy, no flush. The caller guarantees the GPU is not reading [offset, offset+size).
    std::memcpy(static_cast<uint8_t*>(buffer.mapped) + offset, data, size);
    return;
  }
  gl.BindBuffer(buffer.target, buffer.id);
  gl.BufferSubData(buffer.target, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(size), data);
}

void destroy_buffer(Buffer& buffer) noexcept {
  if (buffer.id != 0) {
    census::buffers.sub(static_cast<int64_t>(buffer.size));
    if (buffer.mapped != nullptr && gl.UnmapBuffer != nullptr) {
      gl.BindBuffer(buffer.target, buffer.id);
      gl.UnmapBuffer(buffer.target);
    }
    gl.DeleteBuffers(1, &buffer.id);
    note_buffer_destroyed(buffer.id);
    buffer.id = 0;
    buffer.mapped = nullptr;
  }
}

} // namespace aurora::gl

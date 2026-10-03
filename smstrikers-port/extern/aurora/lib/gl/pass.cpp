#include "pass.hpp"

#include "gl_core.hpp"
#include "state.hpp"

#include <absl/container/flat_hash_map.h>

// The PassEncoder issues GL directly against the render worker's current context
// (aurora's own command list already provides the WebGPU CommandEncoder ordering).
// All methods here run on the render worker.
//
// Phase 3 adds GX geometry submission: the texture/uniform bind groups, the native
// vertex layout (a VAO per present-attr set, S5 integer matrix indices) and the
// indexed/non-indexed draws. Viewport/scissor (S1b Y-flip), blend constant and the
// attribute-less clear/present Draw were wired in Phase 2.

namespace aurora::gl {
namespace {
GLenum topology_gl(PrimitiveTopology topology) {
  switch (topology) {
  case PrimitiveTopology::PointList:
    return GL_POINTS;
  case PrimitiveTopology::LineList:
    return GL_LINES;
  case PrimitiveTopology::LineStrip:
    return GL_LINE_STRIP;
  case PrimitiveTopology::TriangleList:
    return GL_TRIANGLES;
  case PrimitiveTopology::TriangleStrip:
    return GL_TRIANGLE_STRIP;
  }
  return GL_TRIANGLES;
}

// Canonical GX native-fetch attribute order, mirrored from command_processor.cpp
// build_native_layout()/canonical_attr_size(). These are GXAttr enum values; bit i of
// gl::Pipeline::vertexLayout means "attr i is present". Present attrs get packed
// `layout(location)` indices (0,1,2,... in this order) matching the GLSL emitter's
// addInput() and the CPU vertex expansion. Kept as plain ints so pass.cpp needs no GX
// headers; the enum numbering is load-bearing and fixed across the codebase.
namespace canon {
constexpr int kPnMtxIdx = 0;   // GX_VA_PNMTXIDX
constexpr int kTexMtxIdx0 = 1; // GX_VA_TEX0MTXIDX
constexpr int kTexMtxIdx7 = 8; // GX_VA_TEX7MTXIDX
constexpr int kPos = 9;        // GX_VA_POS
constexpr int kNrm = 10;       // GX_VA_NRM
constexpr int kClr0 = 11;      // GX_VA_CLR0
constexpr int kClr1 = 12;      // GX_VA_CLR1
constexpr int kTex0 = 13;      // GX_VA_TEX0
constexpr int kCount = 21;     // through GX_VA_TEX7
} // namespace canon

struct AttrSpec {
  GLint comps;
  GLenum type;
  bool normalized;
  bool integer;
  uint32_t size; // bytes in the expanded vertex record (canonical_attr_size)
};

AttrSpec attr_spec(int attr) {
  if (attr == canon::kPnMtxIdx || (attr >= canon::kTexMtxIdx0 && attr <= canon::kTexMtxIdx7)) {
    return {1, GL_UNSIGNED_INT, false, true, 4}; // Uint32 matrix index (S5)
  }
  if (attr == canon::kPos || attr == canon::kNrm) {
    return {3, GL_FLOAT, false, false, 12};
  }
  if (attr == canon::kClr0 || attr == canon::kClr1) {
    return {4, GL_UNSIGNED_BYTE, true, false, 4}; // Unorm8x4, byte0=R
  }
  return {2, GL_FLOAT, false, false, 8}; // TEXn Float32x2
}

// VAO per present-attr layout (worker-only; VAOs are container objects, not shared
// across contexts). Enable-mask + attrib formats live in the VAO; the per-draw base
// offset is re-specified each draw (the vertex data sits at a dynamic ring offset).
absl::flat_hash_map<uint32_t, GLuint> g_vaoCache;

GLuint get_or_create_vao(uint32_t mask, bool& created) {
  const auto it = g_vaoCache.find(mask);
  if (it != g_vaoCache.end()) {
    created = false;
    return it->second;
  }
  GLuint vao = 0;
  gl.GenVertexArrays(1, &vao);
  g_vaoCache.emplace(mask, vao);
  created = true;
  return vao;
}

static GLuint s_lastVao = 0;
static GLuint s_lastVbuf = 0;
static uint64_t s_lastBaseOffset = UINT64_MAX;
static GLuint s_lastIbuf = 0;

// Bind the layout VAO and (re)point its attributes at `vbuf` + `baseOffset`, then bind
// `ibuf` as the element array (VAO state). Mirrors the CPU vertex record exactly.
void bind_gx_layout(uint32_t mask, const Buffer& vbuf, uint64_t baseOffset, const Buffer& ibuf) {
  bool created = false;
  const GLuint vao = get_or_create_vao(mask, created);
  bind_vertex_array(vao);

  if (!created && s_lastVao == vao && s_lastVbuf == vbuf.id && s_lastBaseOffset == baseOffset && s_lastIbuf == ibuf.id) {
    return;
  }
  s_lastVao = vao;
  s_lastVbuf = vbuf.id;
  s_lastBaseOffset = baseOffset;
  s_lastIbuf = ibuf.id;

  uint32_t stride = 0;
  for (int i = 0; i < canon::kCount; ++i) {
    if ((mask & (1u << i)) != 0) {
      stride += attr_spec(i).size;
    }
  }

  gl.BindBuffer(GL_ARRAY_BUFFER, vbuf.id);
  uint32_t offset = 0;
  GLuint location = 0;
  for (int i = 0; i < canon::kCount; ++i) {
    if ((mask & (1u << i)) == 0) {
      continue;
    }
    const auto spec = attr_spec(i);
    if (created) {
      gl.EnableVertexAttribArray(location);
    }
    const auto* ptr = reinterpret_cast<const void*>(static_cast<uintptr_t>(baseOffset + offset));
    if (spec.integer) {
      gl.VertexAttribIPointer(location, spec.comps, spec.type, static_cast<GLsizei>(stride), ptr);
    } else {
      gl.VertexAttribPointer(location, spec.comps, spec.type, spec.normalized ? GL_TRUE : GL_FALSE,
                             static_cast<GLsizei>(stride), ptr);
    }
    offset += spec.size;
    ++location;
  }
  gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibuf.id);
}

// RmlUi geometry VAO: interleaved Rml::Vertex records (stride 20) with position
// (Float32x2 @0, loc0), tex_coord (Float32x2 @12, loc1) and colour (Unorm8x4 @8, loc2) --
// byte-identical to the WebGPU VertexBufferLayout it replaces (offsets are offsetof on
// Rml::Vertex, which orders position, colour, tex_coord). Attributes are re-pointed each
// draw because the geometry sits at a dynamic vertex-ring offset.
void bind_rml_layout(const Buffer& vbuf, uint64_t baseOffset, const Buffer& ibuf) {
  constexpr uint32_t kRmlStride = 20; // sizeof(Rml::Vertex)
  constexpr uintptr_t kPosOffset = 0;
  constexpr uintptr_t kColourOffset = 8;
  constexpr uintptr_t kTexCoordOffset = 12;
  bool created = false;
  const GLuint vao = get_or_create_vao(kRmlGeometryVertexLayout, created);
  bind_vertex_array(vao);
  gl.BindBuffer(GL_ARRAY_BUFFER, vbuf.id);
  if (created) {
    gl.EnableVertexAttribArray(0);
    gl.EnableVertexAttribArray(1);
    gl.EnableVertexAttribArray(2);
  }
  gl.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, kRmlStride,
                         reinterpret_cast<const void*>(baseOffset + kPosOffset));
  gl.VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, kRmlStride,
                         reinterpret_cast<const void*>(baseOffset + kTexCoordOffset));
  gl.VertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, kRmlStride,
                         reinterpret_cast<const void*>(baseOffset + kColourOffset));
  gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibuf.id);
}
} // namespace

void reset_pass_vao_cache() noexcept {
  g_vaoCache.clear();
  s_lastVao = 0;
  s_lastVbuf = 0;
  s_lastBaseOffset = UINT64_MAX;
  s_lastIbuf = 0;
}

void PassEncoder::SetPipeline(const Pipeline& pipeline) {
  m_pipeline = pipeline;
  m_hasPipeline = true;
  use_program(pipeline.program);
  apply_baked_state(pipeline.state);
}

void PassEncoder::SetBindGroup(uint32_t index, const BindingSet& set, size_t dynamicOffsetCount,
                               const uint32_t* dynamicOffsets) {
  (void)index;
  if (set.bufferCount == 0) {
    // Texture group (GX group 2 / RmlUi): unit i == GX slot i (S3). Sampler uniforms
    // were bound to these units at link time.
    for (uint32_t i = 0; i < kMaxTextureBindings; ++i) {
      bind_texture_unit(i, set.textures[i].texture, set.textures[i].sampler);
    }
    return;
  }
  // Uniform group (GX group 1): one buffer bound with a per-draw dynamic offset.
  size_t dynIdx = 0;
  for (uint8_t j = 0; j < set.bufferCount; ++j) {
    const auto& b = set.buffers[j];
    uint32_t offset = b.offset;
    if (b.dynamic && dynIdx < dynamicOffsetCount) {
      offset += dynamicOffsets[dynIdx++];
    }
    bind_uniform_range(b.binding, b.buffer, offset, b.size);
  }
}

void PassEncoder::SetVertexBuffer(uint32_t slot, const Buffer& buffer, uint64_t offset, uint64_t size) {
  (void)slot;
  (void)size;
  m_vertexBuffer = buffer;
  m_vertexOffset = offset;
}

void PassEncoder::SetIndexBuffer(const Buffer& buffer, IndexFormat format, uint64_t offset, uint64_t size) {
  (void)size;
  m_indexBuffer = buffer;
  m_indexFormat = format;
  m_indexOffset = offset;
}

void PassEncoder::SetViewport(float x, float y, float width, float height, float minDepth, float maxDepth) {
  // WebGPU/GX viewports are top-left origin; GL is bottom-left. Flip once, here (S1b).
  const auto gx = static_cast<GLint>(x);
  const auto gw = static_cast<GLsizei>(width);
  const auto gh = static_cast<GLsizei>(height);
  const auto gy = static_cast<GLint>(static_cast<float>(m_target.height) - (y + height));
  set_viewport_gl(gx, gy, gw, gh, minDepth, maxDepth);
}

void PassEncoder::SetScissorRect(uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
  const auto gy = static_cast<GLint>(m_target.height - (y + height));
  set_scissor_gl(static_cast<GLint>(x), gy, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
}

void PassEncoder::SetBlendConstant(const Color* color) {
  if (color == nullptr) {
    return;
  }
  set_blend_color(static_cast<float>(color->r), static_cast<float>(color->g), static_cast<float>(color->b),
                  static_cast<float>(color->a));
}

void PassEncoder::SetStencilReference(uint32_t reference) {
  m_stencilRef = reference;
  set_stencil_reference(reference);
}

void PassEncoder::Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) {
  (void)firstInstance;
  if (!m_hasPipeline || m_pipeline.program == 0 || vertexCount == 0) {
    return;
  }
  if (m_pipeline.vertexLayout == kRmlGeometryVertexLayout) {
    bind_rml_layout(m_vertexBuffer, m_vertexOffset, m_indexBuffer);
  } else if (m_pipeline.vertexLayout != 0) {
    // Native non-indexed GX draw: base offset is folded into the attrib pointers, so
    // firstVertex stays 0 relative to that base.
    bind_gx_layout(m_pipeline.vertexLayout, m_vertexBuffer, m_vertexOffset, m_indexBuffer);
  } else {
    // Attribute-less draw (clear / present / RmlUi fullscreen triangle via gl_VertexID).
    bind_vertex_array(0);
  }
  const GLenum mode = topology_gl(m_pipeline.state.topology);
  if (instanceCount > 1) {
    gl.DrawArraysInstanced(mode, static_cast<GLint>(firstVertex), static_cast<GLsizei>(vertexCount),
                           static_cast<GLsizei>(instanceCount));
  } else {
    gl.DrawArrays(mode, static_cast<GLint>(firstVertex), static_cast<GLsizei>(vertexCount));
  }
}

void PassEncoder::DrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t baseVertex,
                              uint32_t firstInstance) {
  (void)baseVertex; // native fetch bakes the vertex base into the attrib pointers
  (void)firstInstance;
  if (!m_hasPipeline || m_pipeline.program == 0 || indexCount == 0) {
    return;
  }
  if (m_pipeline.vertexLayout == kRmlGeometryVertexLayout) {
    bind_rml_layout(m_vertexBuffer, m_vertexOffset, m_indexBuffer);
  } else {
    bind_gx_layout(m_pipeline.vertexLayout, m_vertexBuffer, m_vertexOffset, m_indexBuffer);
  }
  const GLenum mode = topology_gl(m_pipeline.state.topology);
  const bool u32 = m_indexFormat == IndexFormat::Uint32;
  const GLenum idxType = u32 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT;
  // glDrawElements' last arg is a byte offset into the element buffer bound in the VAO.
  const uintptr_t idxBytes = m_indexOffset + static_cast<uintptr_t>(firstIndex) * (u32 ? 4u : 2u);
  const auto* idxPtr = reinterpret_cast<const void*>(idxBytes);
  if (instanceCount > 1) {
    gl.DrawElementsInstanced(mode, static_cast<GLsizei>(indexCount), idxType, idxPtr,
                             static_cast<GLsizei>(instanceCount));
  } else {
    gl.DrawElements(mode, static_cast<GLsizei>(indexCount), idxType, idxPtr);
  }
}

void PassEncoder::End() {
  // Store-op handling (StoreOp::Discard -> glInvalidateFramebuffer, S7) is applied in
  // gfx::common render() where the pass's per-attachment store ops are known.
}

// KHR_debug markers (glPushDebugGroup source enum + gating) land with the debug-
// output plumbing in a later phase; these are gated behind AURORA_GFX_DEBUG_GROUPS
// at the call sites and stay no-ops for now.
void PassEncoder::PushDebugGroup(const char* label) { (void)label; }
void PassEncoder::PopDebugGroup() {}
void PassEncoder::InsertDebugMarker(const char* label) { (void)label; }

} // namespace aurora::gl

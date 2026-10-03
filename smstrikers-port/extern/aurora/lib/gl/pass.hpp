#pragma once

// gl::PassEncoder -- the replay target that replaces wgpu::RenderPassEncoder.
//
// Its method surface deliberately mirrors wgpu::RenderPassEncoder (same names,
// same argument order, same defaults) so aurora's replay call sites -- in
// gfx/common.cpp render_pass(), gx/pipeline.cpp render(), rmlui/pipeline.cpp
// render() -- change only the encoder's *type*, not their shape, during the
// Phase 1 cutover.
//
// Unlike WebGPU there is no separate command buffer: methods issue GL directly
// against the render worker's current context (aurora's own command list already
// provides the ordering WebGPU used a CommandEncoder for). Real GL emission lands
// in Phases 2-3; Phase 1 keeps the bodies as state-tracking stubs so the tree
// builds and boots to a black frame.

#include "../gfx/gfx_types.hpp"
#include "handles.hpp"

#include <cstddef>
#include <cstdint>

namespace aurora::gl {

inline constexpr uint64_t kWholeSize = ~uint64_t{0};

// Drop the cached native-layout VAOs. Call on the render worker before the GL
// context goes away (shutdown / context loss); the GL names become invalid then.
void reset_pass_vao_cache() noexcept;

// The framebuffer a pass renders into. width/height drive the top-left ->
// bottom-left origin conversion for viewport/scissor (seam S1b).
struct PassTarget {
  GLuint fbo = 0; // 0 = default framebuffer (desktop present)
  uint32_t width = 0;
  uint32_t height = 0;
};

class PassEncoder {
public:
  explicit PassEncoder(const PassTarget& target) : m_target(target) {}

  void SetPipeline(const Pipeline& pipeline);
  void SetBindGroup(uint32_t index, const BindingSet& set, size_t dynamicOffsetCount = 0,
                    const uint32_t* dynamicOffsets = nullptr);
  void SetVertexBuffer(uint32_t slot, const Buffer& buffer, uint64_t offset = 0, uint64_t size = kWholeSize);
  void SetIndexBuffer(const Buffer& buffer, IndexFormat format, uint64_t offset = 0, uint64_t size = kWholeSize);
  void SetViewport(float x, float y, float width, float height, float minDepth, float maxDepth);
  void SetScissorRect(uint32_t x, uint32_t y, uint32_t width, uint32_t height);
  void SetBlendConstant(const Color* color);
  void SetStencilReference(uint32_t reference);
  void Draw(uint32_t vertexCount, uint32_t instanceCount = 1, uint32_t firstVertex = 0, uint32_t firstInstance = 0);
  void DrawIndexed(uint32_t indexCount, uint32_t instanceCount = 1, uint32_t firstIndex = 0, int32_t baseVertex = 0,
                   uint32_t firstInstance = 0);
  void End();

  void PushDebugGroup(const char* label);
  void PopDebugGroup();
  void InsertDebugMarker(const char* label);

  const PassTarget& target() const { return m_target; }

private:
  PassTarget m_target;

  // Draw state assembled across Set* calls and consumed by Draw/DrawIndexed. The
  // pipeline is stored by value: bind_pipeline() hands us a local gl::Pipeline that
  // would dangle otherwise, and Draw needs its baked topology/restart flag.
  Pipeline m_pipeline{};
  bool m_hasPipeline = false;
  Buffer m_vertexBuffer{};
  uint64_t m_vertexOffset = 0;
  Buffer m_indexBuffer{};
  IndexFormat m_indexFormat = IndexFormat::Uint16;
  uint64_t m_indexOffset = 0;
  uint32_t m_stencilRef = 0;
};

} // namespace aurora::gl

#include "pipeline.hpp"

#include "../webgpu/gpu.hpp"
#include "gx_fmt.hpp"
#include "shader_info.hpp"
#include "tracy/Tracy.hpp"

namespace aurora::gx {
static Module Log("aurora::gx");

// The hardware vertex-attribute layout (native fetch) is built in Phase 3, where
// the draw path creates a VAO per layout hash and issues glVertexAttrib(I)Pointer
// in canonical GX attr order:
//   PNMTXIDX Uint32 -> TEX0-7MTXIDX Uint32 -> POS F32x3 -> NRM F32x3 ->
//   CLR0/1 Unorm8x4 -> TEX0-7 F32x2
// That order must stay byte-identical to shader.cpp's native inputs and the CPU
// expansion in command_processor. The old wgpu::VertexBufferLayout builder is gone;
// gl::Pipeline::vertexLayout carries the layout hash instead.

gl::Pipeline create_pipeline(const PipelineConfig& config) {
  ZoneScoped;
  const uint32_t program = build_shader(config.shaderConfig);
  const auto label = fmt::format("GX Pipeline {:x} shader {:x}",
                                 xxh3_hash(config, static_cast<HashType>(gfx::ShaderType::GX)),
                                 xxh3_hash(config.shaderConfig));
  return build_pipeline(config, program, label.c_str());
}

void render(const DrawData& data, gl::PassEncoder& pass) {
  if (!gfx::bind_pipeline(data.pipeline, pass)) {
    return;
  }

  // Group 1: the GX uniform block, bound with a per-draw dynamic offset
  // (glBindBufferRange in Phase 3).
  gl::BindingSet uniformSet{};
  uniformSet.buffers[0] = gl::BindingSet::BufferBinding{
      .buffer = gfx::g_uniformBuffer.id,
      .binding = 0,
      .offset = 0,
      .size = static_cast<uint32_t>(data.uniformRange.size),
      .dynamic = true,
  };
  uniformSet.bufferCount = 1;
  const std::array offsets{data.uniformRange.offset};
  pass.SetBindGroup(1, uniformSet, offsets.size(), offsets.data());
  if (data.bindGroups.textureBindGroup) {
    pass.SetBindGroup(2, gfx::find_bind_group(data.bindGroups.textureBindGroup));
  }
  if (data.nativeVertexFetch) {
    // Native draws read from a real vertex buffer instead of the group-0 SSBOs. Cached
    // geometry lives in the persistent cross-frame cache buffer; everything else in the
    // shared per-frame vertex ring.
    const gl::Buffer& vtxBuffer = data.cachedGeometry ? gfx::g_nativeVertexCacheBuffer : gfx::g_vertexBuffer;
    pass.SetVertexBuffer(0, vtxBuffer, data.vertRange.offset, data.vertRange.size);
  }
  const gl::Buffer& idxBuffer = data.cachedGeometry ? gfx::g_nativeIndexCacheBuffer : gfx::g_indexBuffer;
  pass.SetIndexBuffer(idxBuffer, gl::IndexFormat::Uint16, data.idxRange.offset, data.idxRange.size);
  if (data.dstAlpha != UINT32_MAX) {
    const gl::Color color{0.f, 0.f, 0.f, data.dstAlpha / 255.f};
    pass.SetBlendConstant(&color);
  }
  if (data.indexCount == 0) {
    pass.Draw(data.vtxCount, data.instanceCount);
  } else {
    pass.DrawIndexed(data.indexCount, data.instanceCount);
  }
}
} // namespace aurora::gx

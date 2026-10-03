#pragma once

#include "../gfx/common.hpp"
#include "gx.hpp"

namespace aurora::gx {
struct DrawData {
  gfx::PipelineRef pipeline;
  gfx::Range vertRange;
  gfx::Range idxRange;
  gfx::Range uniformRange;
  uint32_t vtxCount;
  uint32_t indexCount;
  uint32_t instanceCount;
  GXBindGroups bindGroups;
  uint32_t dstAlpha;
  // When true, bind vertRange as a hardware vertex buffer (slot 0) instead of
  // relying on the group-0 storage buffers. Set by the native-fetch draw path.
  bool nativeVertexFetch = false;
  // When true, vertRange/idxRange address the persistent native geometry cache buffers
  // (g_nativeVertexCacheBuffer / g_nativeIndexCacheBuffer) rather than the per-frame
  // rings. Implies nativeVertexFetch. Set by the native geometry cache on a hit/promote.
  bool cachedGeometry = false;
};

constexpr uint32_t GXPipelineConfigVersion = 15;
struct PipelineConfig {
  uint32_t version = GXPipelineConfigVersion;
  uint32_t msaaSamples = 1;
  ShaderConfig shaderConfig;
  GXCompare depthFunc;
  GXCullMode cullMode;
  GXBlendMode blendMode;
  GXBlendFactor blendFacSrc, blendFacDst;
  GXLogicOp blendOp;
  uint32_t dstAlpha;
  // Non-zero when this draw keeps GX triangle-strip topology on the GPU (TriangleStrip
  // primitive + 0xffff primitive-restart separators) instead of being unrolled to a
  // triangle list. Set by the native strip-batching path; drives to_primitive_state.
  uint32_t triangleStripTopology;
  uint32_t polygonOffsetBits;
  uint32_t polygonOffsetScaleBits;
  uint32_t polygonOffsetClampBits;
  bool depthCompare, depthUpdate, alphaUpdate, colorUpdate;
};
static_assert(std::has_unique_object_representations_v<PipelineConfig>);

gl::Pipeline create_pipeline([[maybe_unused]] const PipelineConfig& config);
void render(const DrawData& data, gl::PassEncoder& pass);

void queue_surface(const u8* dlStart, uint32_t dlSize, bool bigEndian) noexcept;
} // namespace aurora::gx

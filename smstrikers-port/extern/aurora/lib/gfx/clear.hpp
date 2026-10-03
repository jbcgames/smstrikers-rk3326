#pragma once

#include "common.hpp"

namespace aurora::gfx::clear {
struct DrawData {
  PipelineRef pipeline;
  gl::Color color;
  float depth = 0.f;
};

constexpr uint32_t ClearPipelineConfigVersion = 2;
struct PipelineConfig {
  uint32_t version = ClearPipelineConfigVersion;
  uint32_t msaaSamples = 1;
  bool clearColor = true;
  bool clearAlpha = true;
  bool clearDepth = true;
  uint8_t _pad = 0;
};
static_assert(std::has_unique_object_representations_v<PipelineConfig>);

// Compile the shared clear program. Must run on the render worker (context current);
// gfx::initialize calls it before any pipeline is built. Idempotent.
void init_program();
gl::Pipeline create_pipeline(const PipelineConfig& config);
void render(const DrawData& data, gl::PassEncoder& pass, const gl::Extent3D& targetSize);
} // namespace aurora::gfx::clear

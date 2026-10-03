#pragma once

#include "common.hpp"

#include <dolphin/gx/GXEnum.h>

namespace aurora::gfx::tex_copy_conv {

enum class SampleFilter : uint8_t {
  Nearest,
  Linear,
};

struct ConvRequest {
  GXTexFmt fmt;
  gl::Texture srcView; // Resolved EFB / offscreen color or depth texture
  Range uniformRange;  // UV transform uniform (offset + scale)
  TextureHandle dst;   // Destination texture
  SampleFilter sampleFilter = SampleFilter::Nearest;
};

bool needs_conversion(GXTexFmt fmt);

void initialize();
void shutdown();
// No CommandEncoder on GL: these issue GL directly on the render worker.
void run(const ConvRequest& req);
void blit(const ConvRequest& req);

bool snapshot_depth_supported() noexcept;
void snapshot_depth(const gl::Texture& srcDepth, uint32_t msaaSamples, const gl::Texture& dst);

} // namespace aurora::gfx::tex_copy_conv

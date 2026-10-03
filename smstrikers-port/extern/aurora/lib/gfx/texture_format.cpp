#include "texture.hpp"

#include "../internal.hpp"

#include <algorithm>
#include <cstdint>

#include <magic_enum.hpp>

namespace aurora::gfx {
namespace {
Module Log("aurora::gfx");

constexpr u32 div_ceil(u32 value, u32 divisor) noexcept { return (value + divisor - 1) / divisor; }
} // namespace

TextureFormatInfo format_info(gl::TextureFormat format) noexcept {
  // Phase 1: the hand-rolled gl::TextureFormat enum only carries the formats that
  // actually reach the GPU in this fork, so the sRGB and intermediate-block ASTC
  // variants the old wgpu table listed no longer exist as enumerators. Only the two
  // ASTC sizes present in gl::TextureFormat (4x4, 8x8) are kept; the rest are dropped.
  switch (format) {
    DEFAULT_FATAL("unimplemented texture format {}", magic_enum::enum_name(format));
  case gl::TextureFormat::R8Unorm:
    return {1, 1, 1, false};
  case gl::TextureFormat::RG8Unorm:
  case gl::TextureFormat::R16Sint:
    return {1, 1, 2, false};
  case gl::TextureFormat::RGBA8Unorm:
  case gl::TextureFormat::BGRA8Unorm:
  case gl::TextureFormat::R32Float:
    return {1, 1, 4, false};
  case gl::TextureFormat::BC1RGBAUnorm:
    return {4, 4, 8, true};
  case gl::TextureFormat::BC3RGBAUnorm:
  case gl::TextureFormat::BC5RGUnorm:
  case gl::TextureFormat::BC7RGBAUnorm:
    return {4, 4, 16, true};
  case gl::TextureFormat::ASTC4x4Unorm:
    return {4, 4, 16, true};
  case gl::TextureFormat::ASTC8x8Unorm:
    return {8, 8, 16, true};
  }
}

uint64_t calc_texture_size(gl::TextureFormat format, u32 width, u32 height, u32 mips) noexcept {
  const auto info = format_info(format);
  uint64_t total = 0;
  for (uint32_t mip = 0; mip < mips; ++mip) {
    const uint32_t mipWidth = std::max(width >> mip, 1u);
    const uint32_t mipHeight = std::max(height >> mip, 1u);
    const uint64_t widthBlocks = div_ceil(mipWidth, info.blockWidth);
    const uint64_t heightBlocks = div_ceil(mipHeight, info.blockHeight);
    const uint64_t mipBytes = widthBlocks * heightBlocks * info.blockSize;
    total += mipBytes;
  }
  return total;
}

bool is_block_aligned(gl::TextureFormat format, uint32_t width, uint32_t height) noexcept {
  if (width == 0 || height == 0) {
    return false;
  }

  const auto info = format_info(format);
  return !info.compressed || (width % info.blockWidth == 0 && height % info.blockHeight == 0);
}
} // namespace aurora::gfx

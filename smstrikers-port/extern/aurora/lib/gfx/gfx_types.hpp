#pragma once

// Backend-neutral graphics vocabulary for the hand-rolled GLES backend.
//
// The enumerator names here are deliberately IDENTICAL to the ones in Dawn's
// webgpu_cpp.h (`wgpu::TextureFormat::RGBA8Unorm`, `wgpu::LoadOp::Clear`, ...).
// That is what makes the Dawn->GL cutover mechanical: a call site that reads
// `wgpu::TextureFormat::RGBA8Unorm` becomes `gl::TextureFormat::RGBA8Unorm`
// with a namespace swap and nothing else.
//
// During the coexistence window (Phase 0, Dawn still live) these live in the
// fresh `aurora::gl` namespace so they cannot collide with `wgpu::` types that
// are still in use. Existing code fully-qualifies `wgpu::`, so there is no
// ambiguity. Once Dawn is gone, `aurora::gfx` re-exports these (see the alias
// block at the bottom) so the historical `gfx::` spelling keeps working too.

#include <cstdint>

namespace aurora::gl {

// Only the formats that actually reach the GPU in this fork, plus the handful
// the EFB/texture-conversion paths name. Kept in sync with lib/gl/textures.cpp's
// format table (the single source of truth for GL internalformat/format/type).
enum class TextureFormat : uint8_t {
  Undefined = 0,
  RGBA8Unorm,
  RGBA8UnormSrgb,
  BGRA8Unorm,
  R8Unorm,
  RG8Unorm,
  R16Sint,      // palette index textures (isampler2D + texelFetch)
  R32Float,     // depth snapshot target (compat-gated off today)
  R32Uint,
  Depth32Float, // EFB depth attachment
  Depth24Plus,
  Depth24PlusStencil8, // RmlUi clip-mask stencil (GL_DEPTH24_STENCIL8)
  // Compressed (replacement packs only; extension-gated at upload)
  BC1RGBAUnorm,
  BC3RGBAUnorm,
  BC4RUnorm,
  BC5RGUnorm,
  BC7RGBAUnorm,
  ETC2RGB8Unorm,
  ETC2RGBA8Unorm,
  ASTC4x4Unorm,
  ASTC8x8Unorm,
};

enum class LoadOp : uint8_t {
  Undefined = 0,
  Clear,
  Load,
};

enum class StoreOp : uint8_t {
  Undefined = 0,
  Store,
  Discard,
};

enum class CompareFunction : uint8_t {
  Undefined = 0,
  Never,
  Less,
  Equal,
  LessEqual,
  Greater,
  NotEqual,
  GreaterEqual,
  Always,
};

enum class BlendFactor : uint8_t {
  Zero = 0,
  One,
  Src,
  OneMinusSrc,
  SrcAlpha,
  OneMinusSrcAlpha,
  Dst,
  OneMinusDst,
  DstAlpha,
  OneMinusDstAlpha,
  SrcAlphaSaturated,
  Constant,
  OneMinusConstant,
};

enum class BlendOperation : uint8_t {
  Add = 0,
  Subtract,
  ReverseSubtract,
  Min,
  Max,
};

enum class CullMode : uint8_t {
  None = 0,
  Front,
  Back,
};

// GX/WebGPU winding. NOTE (seam S1a): because we render GL-native (no projection
// Y-flip) the rasterized image is mirrored vs WebGPU, so the state translator maps
// FrontFace::CW -> glFrontFace(GL_CCW) and CCW -> GL_CW. This enum stays in WebGPU
// terms; the flip happens once, in lib/gl/state.cpp.
enum class FrontFace : uint8_t {
  CCW = 0,
  CW,
};

enum class PrimitiveTopology : uint8_t {
  PointList = 0,
  LineList,
  LineStrip,
  TriangleList,
  TriangleStrip,
};

enum class IndexFormat : uint8_t {
  Undefined = 0,
  Uint16,
  Uint32,
};

enum class AddressMode : uint8_t {
  ClampToEdge = 0,
  Repeat,
  MirrorRepeat,
};

enum class FilterMode : uint8_t {
  Nearest = 0,
  Linear,
};

// Distinct from FilterMode so "no mips" can be expressed as Undefined, matching
// wgpu::MipmapFilterMode + the GX sampler's mipless [0,0] LOD clamp.
enum class MipmapFilterMode : uint8_t {
  Undefined = 0,
  Nearest,
  Linear,
};

enum class ColorWriteMask : uint8_t {
  None = 0,
  Red = 1,
  Green = 2,
  Blue = 4,
  Alpha = 8,
  All = Red | Green | Blue | Alpha,
};
inline ColorWriteMask operator|(ColorWriteMask a, ColorWriteMask b) {
  return static_cast<ColorWriteMask>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}
inline ColorWriteMask operator&(ColorWriteMask a, ColorWriteMask b) {
  return static_cast<ColorWriteMask>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}
inline bool any(ColorWriteMask m) { return static_cast<uint8_t>(m) != 0; }

struct Extent3D {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t depthOrArrayLayers = 1;
  bool operator==(const Extent3D& o) const {
    return width == o.width && height == o.height && depthOrArrayLayers == o.depthOrArrayLayers;
  }
};

struct Origin3D {
  uint32_t x = 0;
  uint32_t y = 0;
  uint32_t z = 0;
};

struct Color {
  double r = 0.0;
  double g = 0.0;
  double b = 0.0;
  double a = 0.0;
};

} // namespace aurora::gl

// Dawn is gone: re-export the vocabulary into aurora::gfx so the historical
// `gfx::TextureFormat` / `gfx::LoadOp` spelling (what the old code called
// `wgpu::`) keeps working alongside the canonical `gl::` spelling. New GL code
// prefers `gl::`; either resolves to the same type.
namespace aurora::gfx {
using gl::AddressMode;
using gl::BlendFactor;
using gl::BlendOperation;
using gl::Color;
using gl::ColorWriteMask;
using gl::CompareFunction;
using gl::CullMode;
using gl::Extent3D;
using gl::FilterMode;
using gl::FrontFace;
using gl::IndexFormat;
using gl::LoadOp;
using gl::MipmapFilterMode;
using gl::Origin3D;
using gl::PrimitiveTopology;
using gl::StoreOp;
using gl::TextureFormat;
using gl::any;
using gl::operator|;
using gl::operator&;
} // namespace aurora::gfx

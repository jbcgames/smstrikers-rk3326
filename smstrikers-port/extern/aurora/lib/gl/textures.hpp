#pragma once

// GL texture + sampler lifecycle for the hand-rolled backend.
//
// This is the single source of truth for the gfx TextureFormat -> GL
// {internalformat, external format, type} mapping (the format table below), and
// owns creation/upload/destruction of gl::Texture and gl::Sampler handles.
// Immutable storage (glTexStorage2D) is used everywhere -- it is the corpus-normal
// path and lets the driver lay textures out optimally (Normalcy Doctrine).
//
// Scaffolded beside Dawn: nothing calls this until the Phase 1 cutover replaces
// gpu.cpp's g_device.CreateTexture / common.cpp's offscreen+snapshot paths.

#include "gl_core.hpp"
#include "handles.hpp"

#include "../gfx/gfx_types.hpp"

namespace aurora::gl {

// Resolved GL format triple for a gfx TextureFormat. For compressed formats,
// `internalFormat` is the compressed enum, `external`/`type` are unused, and
// `compressed` is set with the block geometry filled in for sub-image sizing.
struct GlFormatInfo {
  GLenum internalFormat = 0; // sized internalformat for glTexStorage2D
  GLenum external = 0;       // format for glTexSubImage2D (uncompressed only)
  GLenum type = 0;           // type for glTexSubImage2D (uncompressed only)
  uint32_t bytesPerPixel = 0; // uncompressed only
  bool compressed = false;
  bool depth = false;
  uint32_t blockWidth = 1;
  uint32_t blockHeight = 1;
  uint32_t blockBytes = 0; // compressed only: bytes per block
  bool valid = false;
};

// Returns the GL format triple for `format`. `valid == false` for formats that
// must never reach GL creation on this fork (they log at the call site).
GlFormatInfo gl_format(TextureFormat format);

// Sampler state, mirroring the fields of wgpu::SamplerDescriptor that the GX and
// RmlUi paths actually set. Hashed by the sampler_ref cache; the value is a GLuint.
struct SamplerDescriptor {
  AddressMode addressU = AddressMode::ClampToEdge;
  AddressMode addressV = AddressMode::ClampToEdge;
  AddressMode addressW = AddressMode::ClampToEdge;
  FilterMode magFilter = FilterMode::Nearest;
  FilterMode minFilter = FilterMode::Nearest;
  MipmapFilterMode mipmapFilter = MipmapFilterMode::Undefined;
  float lodMinClamp = 0.f;
  float lodMaxClamp = 0.f;
  uint16_t maxAnisotropy = 1;
};

// Create an immutable 2D texture (glGenTextures + glTexStorage2D). `mips` is the
// full level count (>=1). `renderable` is metadata only (GL attaches textures
// directly); it rides along on the handle for the FBO cache. Requires the owning
// context current on the calling thread.
Texture create_texture(TextureFormat format, Extent3D size, uint32_t mips, bool renderable);

// Upload one mip level. `bytesPerRow` is the source row stride; 0 means tightly
// packed. Sets UNPACK_ALIGNMENT=1 (+ROW_LENGTH when needed) then glTexSubImage2D
// (or glCompressedTexSubImage2D for compressed formats).
void upload_texture(const Texture& texture, uint32_t level, Origin3D offset, Extent3D size, const void* data,
                    uint32_t bytesPerRow = 0);

void destroy_texture(Texture& texture) noexcept;

// Create a sampler object (glGenSamplers + glSamplerParameter*). Anisotropy is
// applied only when EXT_texture_filter_anisotropic is present (caller passes the
// clamped value; 1 = off).
Sampler create_sampler(const SamplerDescriptor& desc, bool anisotropySupported);

void destroy_sampler(Sampler& sampler) noexcept;

} // namespace aurora::gl

#include "textures.hpp"

#include "census.hpp"
#include "fbo_cache.hpp"
#include "state.hpp"
#include "../internal.hpp"

#include <vector>

namespace aurora::gl {
namespace {
Module Log("aurora::gl");

GLint min_filter_enum(FilterMode min, MipmapFilterMode mip) {
  switch (mip) {
  case MipmapFilterMode::Undefined:
    return min == FilterMode::Linear ? GL_LINEAR : GL_NEAREST;
  case MipmapFilterMode::Nearest:
    return min == FilterMode::Linear ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST;
  case MipmapFilterMode::Linear:
    return min == FilterMode::Linear ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR;
  }
  return GL_NEAREST;
}

GLint mag_filter_enum(FilterMode mag) { return mag == FilterMode::Linear ? GL_LINEAR : GL_NEAREST; }

GLint address_enum(AddressMode mode) {
  switch (mode) {
  case AddressMode::Repeat:
    return GL_REPEAT;
  case AddressMode::MirrorRepeat:
    return GL_MIRRORED_REPEAT;
  case AddressMode::ClampToEdge:
  default:
    return GL_CLAMP_TO_EDGE;
  }
}

// GPU storage the full declared mip chain occupies (glTexStorage2D allocates every
// level up front, whether or not it is ever uploaded).
int64_t texture_gpu_bytes(TextureFormat format, Extent3D size, uint32_t levels) {
  const auto info = gl_format(format);
  if (!info.valid) {
    return 0;
  }
  int64_t total = 0;
  uint32_t w = size.width;
  uint32_t h = size.height;
  for (uint32_t l = 0; l < levels; ++l) {
    if (info.compressed) {
      const int64_t blocksW = (w + info.blockWidth - 1) / info.blockWidth;
      const int64_t blocksH = (h + info.blockHeight - 1) / info.blockHeight;
      total += blocksW * blocksH * info.blockBytes;
    } else {
      total += static_cast<int64_t>(w) * h * info.bytesPerPixel;
    }
    w = w > 1 ? w / 2 : 1;
    h = h > 1 ? h / 2 : 1;
  }
  return total;
}
} // namespace

GlFormatInfo gl_format(TextureFormat format) {
  GlFormatInfo i{};
  i.valid = true;
  switch (format) {
  case TextureFormat::RGBA8Unorm:
    i.internalFormat = GL_RGBA8, i.external = GL_RGBA, i.type = GL_UNSIGNED_BYTE, i.bytesPerPixel = 4;
    break;
  case TextureFormat::RGBA8UnormSrgb:
    i.internalFormat = GL_SRGB8_ALPHA8, i.external = GL_RGBA, i.type = GL_UNSIGNED_BYTE, i.bytesPerPixel = 4;
    break;
  case TextureFormat::BGRA8Unorm:
    // Surface/swapchain format only; in GL the default framebuffer presents, so a
    // BGRA8 *texture* is never created on the live path. Collapse to RGBA8 storage
    // if one ever is (channel order would be a sampling-side concern).
    i.internalFormat = GL_RGBA8, i.external = GL_RGBA, i.type = GL_UNSIGNED_BYTE, i.bytesPerPixel = 4;
    break;
  case TextureFormat::R8Unorm:
    i.internalFormat = GL_R8, i.external = GL_RED, i.type = GL_UNSIGNED_BYTE, i.bytesPerPixel = 1;
    break;
  case TextureFormat::RG8Unorm:
    i.internalFormat = GL_RG8, i.external = GL_RG, i.type = GL_UNSIGNED_BYTE, i.bytesPerPixel = 2;
    break;
  case TextureFormat::R16Sint:
    i.internalFormat = GL_R16I, i.external = GL_RED_INTEGER, i.type = GL_SHORT, i.bytesPerPixel = 2;
    break;
  case TextureFormat::R32Float:
    i.internalFormat = GL_R32F, i.external = GL_RED, i.type = GL_FLOAT, i.bytesPerPixel = 4;
    break;
  case TextureFormat::R32Uint:
    i.internalFormat = GL_R32UI, i.external = GL_RED_INTEGER, i.type = GL_UNSIGNED_INT, i.bytesPerPixel = 4;
    break;
  case TextureFormat::Depth32Float:
    i.internalFormat = GL_DEPTH_COMPONENT32F, i.external = GL_DEPTH_COMPONENT, i.type = GL_FLOAT, i.bytesPerPixel = 4,
    i.depth = true;
    break;
  case TextureFormat::Depth24Plus:
    i.internalFormat = GL_DEPTH_COMPONENT24, i.external = GL_DEPTH_COMPONENT, i.type = GL_UNSIGNED_INT,
    i.bytesPerPixel = 4, i.depth = true;
    break;
  case TextureFormat::Depth24PlusStencil8:
    i.internalFormat = GL_DEPTH24_STENCIL8, i.external = GL_DEPTH_STENCIL, i.type = GL_UNSIGNED_INT_24_8,
    i.bytesPerPixel = 4, i.depth = true;
    break;
  // Compressed (replacement packs; extension-gated at upload). 4x4 block geometry
  // for all BC/ETC2/ASTC4x4; 8x8 for ASTC8x8.
  case TextureFormat::BC1RGBAUnorm:
    i.internalFormat = GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, i.compressed = true, i.blockWidth = 4, i.blockHeight = 4,
    i.blockBytes = 8;
    break;
  case TextureFormat::BC3RGBAUnorm:
    i.internalFormat = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, i.compressed = true, i.blockWidth = 4, i.blockHeight = 4,
    i.blockBytes = 16;
    break;
  case TextureFormat::BC4RUnorm:
    i.internalFormat = GL_COMPRESSED_RED_RGTC1_EXT, i.compressed = true, i.blockWidth = 4, i.blockHeight = 4,
    i.blockBytes = 8;
    break;
  case TextureFormat::BC5RGUnorm:
    i.internalFormat = GL_COMPRESSED_RG_RGTC2_EXT, i.compressed = true, i.blockWidth = 4, i.blockHeight = 4,
    i.blockBytes = 16;
    break;
  case TextureFormat::BC7RGBAUnorm:
    i.internalFormat = GL_COMPRESSED_RGBA_BPTC_UNORM_EXT, i.compressed = true, i.blockWidth = 4, i.blockHeight = 4,
    i.blockBytes = 16;
    break;
  case TextureFormat::ETC2RGB8Unorm:
    i.internalFormat = GL_COMPRESSED_RGB8_ETC2, i.compressed = true, i.blockWidth = 4, i.blockHeight = 4,
    i.blockBytes = 8;
    break;
  case TextureFormat::ETC2RGBA8Unorm:
    i.internalFormat = GL_COMPRESSED_RGBA8_ETC2_EAC, i.compressed = true, i.blockWidth = 4, i.blockHeight = 4,
    i.blockBytes = 16;
    break;
  case TextureFormat::ASTC4x4Unorm:
    i.internalFormat = GL_COMPRESSED_RGBA_ASTC_4x4_KHR, i.compressed = true, i.blockWidth = 4, i.blockHeight = 4,
    i.blockBytes = 16;
    break;
  case TextureFormat::ASTC8x8Unorm:
    i.internalFormat = GL_COMPRESSED_RGBA_ASTC_8x8_KHR, i.compressed = true, i.blockWidth = 8, i.blockHeight = 8,
    i.blockBytes = 16;
    break;
  case TextureFormat::Undefined:
  default:
    i.valid = false;
    break;
  }
  return i;
}

Texture create_texture(TextureFormat format, Extent3D size, uint32_t mips, bool renderable) {
  const auto info = gl_format(format);
  if (!info.valid) {
    Log.fatal("create_texture: unsupported format {}", static_cast<int>(format));
    return {};
  }
  const uint32_t levels = mips < 1 ? 1 : mips;
  GLuint id = 0;
  gl.GenTextures(1, &id);
  gl.BindTexture(GL_TEXTURE_2D, id);
  gl.TexStorage2D(GL_TEXTURE_2D, static_cast<GLsizei>(levels), info.internalFormat, static_cast<GLsizei>(size.width),
                  static_cast<GLsizei>(size.height));
  // Sensible defaults; the actual sampler object drives filtering at draw time. A
  // full mip chain is declared but not necessarily uploaded, so clamp MAX_LEVEL.
  gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
  gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, static_cast<GLint>(levels - 1));
  // glTexStorage2D leaves texel memory undefined. Renderable color targets (EFB-copy /
  // offscreen textures) can be sampled before anything renders into them (a GX effect
  // reads last frame's copy on the first frame, or a copy that hasn't run yet), and
  // sampling undefined GPU memory is exactly the driver-dependent behavior the Normalcy
  // Doctrine avoids — on desktop it surfaced as a red/magenta wash. Zero-fill level 0 so
  // an unpopulated copy reads opaque black.
  if (renderable && !info.depth && !info.compressed && info.bytesPerPixel > 0) {
    std::vector<uint8_t> zeros(static_cast<size_t>(size.width) * size.height * info.bytesPerPixel, 0);
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(size.width), static_cast<GLsizei>(size.height),
                     info.external, info.type, zeros.data());
  }
  census::textures.add(texture_gpu_bytes(format, size, levels));
  return Texture{
      .id = id,
      .format = format,
      .size = size,
      .mips = levels,
      .renderable = renderable,
  };
}

void upload_texture(const Texture& texture, uint32_t level, Origin3D offset, Extent3D size, const void* data,
                    uint32_t bytesPerRow) {
  if (texture.id == 0) {
    return;
  }
  const auto info = gl_format(texture.format);
  if (!info.valid) {
    return;
  }
  gl.BindTexture(GL_TEXTURE_2D, texture.id);

  if (info.compressed) {
    const uint32_t blocksW = (size.width + info.blockWidth - 1) / info.blockWidth;
    const uint32_t blocksH = (size.height + info.blockHeight - 1) / info.blockHeight;
    const GLsizei imageSize = static_cast<GLsizei>(blocksW * blocksH * info.blockBytes);
    gl.CompressedTexSubImage2D(GL_TEXTURE_2D, static_cast<GLint>(level), static_cast<GLint>(offset.x),
                               static_cast<GLint>(offset.y), static_cast<GLsizei>(size.width),
                               static_cast<GLsizei>(size.height), info.internalFormat, imageSize, data);
    return;
  }

  gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
  const bool customStride = bytesPerRow != 0 && bytesPerRow != size.width * info.bytesPerPixel;
  if (customStride) {
    gl.PixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(bytesPerRow / info.bytesPerPixel));
  }
  gl.TexSubImage2D(GL_TEXTURE_2D, static_cast<GLint>(level), static_cast<GLint>(offset.x), static_cast<GLint>(offset.y),
                   static_cast<GLsizei>(size.width), static_cast<GLsizei>(size.height), info.external, info.type, data);
  if (customStride) {
    gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
  }
}

void destroy_texture(Texture& texture) noexcept {
  if (texture.id != 0) {
    // Drop any cached FBO whose attachment is this texture BEFORE deleting it. GL
    // detaches a deleted texture only from the currently-bound framebuffer; a cached
    // non-bound FBO keeps a dangling attachment, and once glGenTextures recycles the
    // name, get_framebuffer would hit that stale entry and render the new texture's
    // content into the dead attachment (the persistent-black minimap: every EFB copy
    // vanished into a dangling FBO while the quad sampled the zero-filled texture).
    invalidate_framebuffers_for(texture.id);
    census::textures.sub(texture_gpu_bytes(texture.format, texture.size, texture.mips));
    gl.DeleteTextures(1, &texture.id);
    note_texture_destroyed(texture.id);
    texture.id = 0;
  }
}

Sampler create_sampler(const SamplerDescriptor& desc, bool anisotropySupported) {
  GLuint id = 0;
  gl.GenSamplers(1, &id);
  gl.SamplerParameteri(id, GL_TEXTURE_WRAP_S, address_enum(desc.addressU));
  gl.SamplerParameteri(id, GL_TEXTURE_WRAP_T, address_enum(desc.addressV));
  gl.SamplerParameteri(id, GL_TEXTURE_WRAP_R, address_enum(desc.addressW));
  gl.SamplerParameteri(id, GL_TEXTURE_MIN_FILTER, min_filter_enum(desc.minFilter, desc.mipmapFilter));
  gl.SamplerParameteri(id, GL_TEXTURE_MAG_FILTER, mag_filter_enum(desc.magFilter));
  gl.SamplerParameterf(id, GL_TEXTURE_MIN_LOD, desc.lodMinClamp);
  gl.SamplerParameterf(id, GL_TEXTURE_MAX_LOD, desc.lodMaxClamp);
  if (anisotropySupported && desc.maxAnisotropy > 1) {
    gl.SamplerParameterf(id, GL_TEXTURE_MAX_ANISOTROPY_EXT, static_cast<GLfloat>(desc.maxAnisotropy));
  }
  census::samplers.add(0);
  return Sampler{.id = id};
}

void destroy_sampler(Sampler& sampler) noexcept {
  if (sampler.id != 0) {
    census::samplers.sub(0);
    gl.DeleteSamplers(1, &sampler.id);
    note_sampler_destroyed(sampler.id);
    sampler.id = 0;
  }
}

} // namespace aurora::gl

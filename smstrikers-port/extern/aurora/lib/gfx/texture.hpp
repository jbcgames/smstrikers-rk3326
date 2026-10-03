#pragma once
#include <dolphin/gx.h>

#include <utility>
#include <vector>

#include "common.hpp"

namespace aurora::gfx {
// A pending texture upload. On GL this is a direct glTexSubImage2D from CPU memory
// (the WebGPU staging-buffer + CopyBufferToTexture path is gone): `range` indexes
// the frame's textureUpload ByteBuffer, or `data` points at caller-owned bytes.
struct TextureUpload {
  gl::Texture tex;
  gl::Origin3D origin;
  gl::Extent3D size;
  uint32_t level = 0;
  uint32_t bytesPerRow = 0;    // source row stride; 0 = tightly packed
  Range range;                 // offset/length into the frame textureUpload buffer
  const uint8_t* data = nullptr; // when set, upload from here instead of the frame buffer

  TextureUpload() noexcept = default;
  TextureUpload(gl::Texture tex, gl::Origin3D origin, gl::Extent3D size, uint32_t level, uint32_t bytesPerRow,
                Range range) noexcept
  : tex(tex), origin(origin), size(size), level(level), bytesPerRow(bytesPerRow), range(range) {}
};
void queue_texture_upload(TextureUpload upload);
void queue_texture_upload_data(const uint8_t* data, uint32_t bytesPerRow, uint32_t rowsPerImage, gl::Texture tex,
                               gl::Origin3D origin, gl::Extent3D size, uint32_t level = 0);

struct TextureFormatInfo {
  uint8_t blockWidth;
  uint8_t blockHeight;
  uint8_t blockSize;
  bool compressed;
};
TextureFormatInfo format_info(gl::TextureFormat format) noexcept;
uint64_t calc_texture_size(gl::TextureFormat format, uint32_t width, uint32_t height, uint32_t mips) noexcept;
bool is_block_aligned(gl::TextureFormat format, uint32_t width, uint32_t height) noexcept;

constexpr u32 InvalidTextureFormat = -1;
// In GL the sample view and attachment view are just the texture; both fields keep
// their names (call sites unchanged) but hold copies of the same gl::Texture.
struct TextureRef {
  gl::Texture texture;
  gl::Texture sampleTextureView;
  gl::Texture attachmentTextureView;
  gl::Extent3D size;
  gl::TextureFormat format;
  uint32_t mipCount;
  u32 gxFormat;
  bool hasArbitraryMips = false;
  bool isReplacement = false;

  TextureRef(gl::Texture texture, gl::Texture sampleTextureView, gl::Texture attachmentTextureView, gl::Extent3D size,
             gl::TextureFormat format, uint32_t mipCount, u32 gxFormat)
  : texture(texture)
  , sampleTextureView(sampleTextureView)
  , attachmentTextureView(attachmentTextureView)
  , size(size)
  , format(format)
  , mipCount(mipCount)
  , gxFormat(gxFormat) {}

  // Owns the GL texture: the views are copies of the same gl::Texture, so exactly one
  // glDeleteTextures per ref. Without this every dropped handle (TLUT rebuild, tex-data
  // version bump, area unload) leaked its GL texture into pinned driver memory -- the
  // G31 OOM. Deletion marshals to the render worker (context owner); see texture.cpp.
  ~TextureRef();
  TextureRef(const TextureRef&) = delete;
  TextureRef& operator=(const TextureRef&) = delete;
};

// Deferred GL-texture deletion (see ~TextureRef in texture.cpp for why deletes must
// not jump the render-worker queue). Any thread may defer; end_frame() snapshots the
// accumulated list into the frame's end-of-frame worker item, which destroys them
// after every pass of the frame has executed.
void defer_texture_destroy(const gl::Texture& texture) noexcept;
std::vector<gl::Texture> take_deferred_texture_destroys() noexcept;

TextureHandle new_static_texture_2d(uint32_t width, uint32_t height, uint32_t mips, u32 gxFormat,
                                    ArrayRef<uint8_t> data, bool tlut, const char* label) noexcept;
TextureHandle new_dynamic_texture_2d(uint32_t width, uint32_t height, uint32_t mips, u32 gxFormat,
                                     const char* label) noexcept;
TextureHandle new_render_texture(uint32_t width, uint32_t height, u32 gxFormat, const char* label) noexcept;
TextureHandle new_conv_texture(uint32_t width, uint32_t height, u32 gxFormat, const char* label) noexcept;
void write_texture(TextureRef& ref, ArrayRef<uint8_t> data) noexcept;
}; // namespace aurora::gfx

struct GXTexObj_ {
  u32 mode0 = 0;
  u32 mode1 = 0;
  u32 image0 = UINT32_MAX;
  u32 image3 = 0;
  const void* userData = nullptr;
  const void* data = nullptr;
  u32 mWidth = 0;
  u32 mHeight = 0;
  u32 mFormat = aurora::gfx::InvalidTextureFormat;
  GXTlut tlut = GX_TLUT0;
  u32 texObjId = 0;
  u32 texDataVersion = 0;
  u8 flags = 0;

  static constexpr u32 get_bits(u32 reg, u32 size, u32 shift) noexcept { return (reg >> shift) & ((1u << size) - 1); }

  u32 width() const noexcept { return mWidth != 0 ? mWidth : get_bits(image0, 10, 0) + 1 & 0x3FF; }
  u32 height() const noexcept { return mHeight != 0 ? mHeight : get_bits(image0, 10, 10) + 1 & 0x3FF; }
  u32 raw_format() const noexcept { return get_bits(image0, 4, 20); }
  u32 format() const noexcept { return mFormat != aurora::gfx::InvalidTextureFormat ? mFormat : raw_format(); }
  GXTexWrapMode wrap_s() const noexcept { return static_cast<GXTexWrapMode>(get_bits(mode0, 2, 0)); }
  GXTexWrapMode wrap_t() const noexcept { return static_cast<GXTexWrapMode>(get_bits(mode0, 2, 2)); }
  GXTexFilter min_filter() const noexcept {
    constexpr GXTexFilter kHwToGxFilter[8] = {
        GX_NEAR, GX_NEAR_MIP_NEAR, GX_LIN_MIP_NEAR, GX_NEAR, GX_LINEAR, GX_NEAR_MIP_LIN, GX_LIN_MIP_LIN, GX_NEAR,
    };
    return kHwToGxFilter[get_bits(mode0, 3, 5)];
  }
  GXTexFilter mag_filter() const noexcept { return get_bits(mode0, 1, 4) != 0 ? GX_LINEAR : GX_NEAR; }
  GXBool has_mips() const noexcept { return (flags & 1u) != 0 ? GX_TRUE : GX_FALSE; }
  u32 mip_count() const noexcept { return has_mips() ? std::max<u32>(static_cast<u32>(max_lod()) + 1, 1u) : 1; }
  GXBool do_edge_lod() const noexcept { return get_bits(mode0, 1, 8) == 0 ? GX_TRUE : GX_FALSE; }
  float lod_bias() const noexcept { return static_cast<float>(static_cast<int8_t>(get_bits(mode0, 8, 9))) / 32.0f; }
  GXAnisotropy max_aniso() const noexcept { return static_cast<GXAnisotropy>(get_bits(mode0, 2, 19)); }
  GXBool bias_clamp() const noexcept { return get_bits(mode0, 1, 21) != 0 ? GX_TRUE : GX_FALSE; }
  float min_lod() const noexcept { return static_cast<float>(get_bits(mode1, 8, 0)) / 16.0f; }
  float max_lod() const noexcept { return static_cast<float>(get_bits(mode1, 8, 8)) / 16.0f; }

  // Custom flag for texture caching
  bool no_cache() const noexcept { return (flags & 0x80) != 0; }
  void set_no_cache(bool value) noexcept { flags = value ? flags | 0x80 : flags & ~0x80; }

  // Hacky workaround for an instances where incremental IDs are used for GXCopyTex, but the copy tex was invalidated
  // and the texture reference is still present.
  bool has_data() const noexcept { return reinterpret_cast<uintptr_t>(data) >= 0x10000; }
};
static_assert(sizeof(GXTexObj_) <= sizeof(GXTexObj), "GXTexObj too small!");
struct GXTlutObj_ {
  u32 tlut = 0;
  u32 loadTlut0 = 0;
  u16 numEntries = 0;
  const void* data = nullptr;
  GXTlutFmt format = GX_TL_IA8;
  u32 tlutObjId = 0;
  u32 tlutDataVersion = 0;
  u8 flags = 0;

  // Custom flag for texture caching
  bool no_cache() const noexcept { return (flags & 0x80) != 0; }
  void set_no_cache(bool value) noexcept { flags = value ? flags | 0x80 : flags & ~0x80; }
};
static_assert(sizeof(GXTlutObj_) <= sizeof(GXTlutObj), "GXTlutObj too small!");

namespace aurora::gfx {
struct TextureBind {
  TextureHandle ref;
  GXTexObj_ texObj;

  TextureBind() noexcept = default;
  TextureBind(const GXTexObj_& obj, TextureHandle handle) noexcept : ref(std::move(handle)), texObj(obj) {}
  void reset() noexcept { ref.reset(); }
  [[nodiscard]] gl::SamplerDescriptor get_descriptor() const noexcept;
  operator bool() const noexcept { return ref.operator bool(); }
};
} // namespace aurora::gfx

#include "gx.hpp"

#include "pipeline.hpp"
#include "../dolphin/vi/vi_internal.hpp"
#include "../webgpu/gpu.hpp"
#include "../internal.hpp"
#include "../gfx/common.hpp"
#include "../gfx/tex_palette_conv.hpp"
#include "../gfx/texture.hpp"
#include "../gfx/texture_convert.hpp"
#include "../gfx/texture_replacement.hpp"
#include "gx_fmt.hpp"

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <tracy/Tracy.hpp>

#include <atomic>
#include <bit>
#include <cfloat>
#include <cmath>
#include <mutex>
#include <optional>
#include <utility>

static aurora::Module Log("aurora::gx");

namespace aurora::gx {
using webgpu::g_graphicsConfig;

GXState g_gxState{};

// The 1x1 fills bound into unused GX texture slots. GL has no separate
// bind-group-layout / pipeline-layout objects (Dawn concepts), so those are gone;
// binding is expressed directly as a gl::BindingSet. Real GL objects for the
// empties are created on the render worker in Phase 2.
static gl::Texture sEmptyTexture;
static gl::Sampler sEmptySampler;
gl::BindingSet g_emptyTextureBindGroup;

namespace {
struct DynamicPaletteKey {
  const void* sourceIdentity = nullptr;
  u32 width = 0;
  u32 height = 0;
  u32 format = 0;

  bool operator==(const DynamicPaletteKey& rhs) const = default;
  template <typename H>
  friend H AbslHashValue(H h, const DynamicPaletteKey& key) {
    return H::combine(std::move(h), key.sourceIdentity, key.width, key.height, key.format);
  }
};

struct DynamicPaletteEntry {
  gfx::TextureHandle handle;
  u32 sourceRevision = 0;
  u32 tlutDataVersion = 0;
  u32 lastUsedFrame = 0;
};

struct CachedTextureEntry {
  gfx::TextureHandle handle;
  u32 texDataVersion = 0;
  u32 tlutObjId = 0;
  u32 tlutDataVersion = 0;
  u32 lastUsedFrame = 0;
};

struct CachedTlutTextureEntry {
  gfx::TextureHandle handle;
  u32 tlutDataVersion = 0;
  u32 lastUsedFrame = 0;
};

struct TlutObjectCache {
  CachedTlutTextureEntry tlutTexture;
  absl::flat_hash_map<DynamicPaletteKey, DynamicPaletteEntry> dynamicPaletteTextures;
  absl::flat_hash_set<u32> staticTextureUsers;
};

absl::flat_hash_map<u32, CachedTextureEntry> s_textureObjectCaches;
absl::flat_hash_map<u32, TlutObjectCache> s_tlutObjectCaches;
std::atomic_bool s_staticTextureCacheClearPending = false;

void do_clear_static_texture_cache() noexcept {
  s_textureObjectCaches.clear();
  for (auto& [_, cache] : s_tlutObjectCaches) {
    cache.staticTextureUsers.clear();
  }
}

DynamicPaletteKey make_dynamic_palette_key(const GXTexObj_& obj, const GXState::CopyTextureRef& source) {
  return {
      .sourceIdentity = source.handle.get(),
      .width = obj.width(),
      .height = obj.height(),
      .format = obj.format(),
  };
}

void clear_texture_dependency(u32 texObjId, u32 tlutObjId) {
  if (texObjId == 0 || tlutObjId == 0) {
    return;
  }
  if (auto it = s_tlutObjectCaches.find(tlutObjId); it != s_tlutObjectCaches.end()) {
    it->second.staticTextureUsers.erase(texObjId);
    if (!it->second.tlutTexture.handle && it->second.dynamicPaletteTextures.empty() &&
        it->second.staticTextureUsers.empty()) {
      s_tlutObjectCaches.erase(it);
    }
  }
}

void store_cached_texture(const GXTexObj_& obj, gfx::TextureHandle handle, u32 tlutObjId = 0, u32 tlutDataVersion = 0) {
  if (obj.texObjId == 0) {
    return;
  }

  auto& entry = s_textureObjectCaches[obj.texObjId];
  if (entry.tlutObjId != tlutObjId) {
    clear_texture_dependency(obj.texObjId, entry.tlutObjId);
  }

  entry.handle = std::move(handle);
  entry.texDataVersion = obj.texDataVersion;
  entry.tlutObjId = tlutObjId;
  entry.tlutDataVersion = tlutDataVersion;
  entry.lastUsedFrame = gfx::current_frame();

  if (tlutObjId != 0) {
    s_tlutObjectCaches[tlutObjId].staticTextureUsers.insert(obj.texObjId);
  }
}

gfx::TextureHandle get_tlut_texture(const GXTlutObj_& tlut) {
  if (tlut.tlutObjId != 0) {
    auto& cache = s_tlutObjectCaches[tlut.tlutObjId];
    if (cache.tlutTexture.handle && cache.tlutTexture.tlutDataVersion == tlut.tlutDataVersion) {
      cache.tlutTexture.lastUsedFrame = gfx::current_frame();
      return cache.tlutTexture.handle;
    }
    cache.dynamicPaletteTextures.clear();
    for (const u32 texObjId : cache.staticTextureUsers) {
      s_textureObjectCaches.erase(texObjId);
    }
    cache.staticTextureUsers.clear();
  }

  const auto handle = gfx::new_static_texture_2d(
      tlut.numEntries, 1, 1, gfx::tlut_texture_format(tlut.format),
      {static_cast<const u8*>(tlut.data), static_cast<size_t>(tlut.numEntries) * sizeof(u16)}, true, "Loaded TLUT");
  if (tlut.tlutObjId != 0) {
    auto& cache = s_tlutObjectCaches[tlut.tlutObjId];
    cache.tlutTexture.handle = handle;
    cache.tlutTexture.tlutDataVersion = tlut.tlutDataVersion;
    cache.tlutTexture.lastUsedFrame = gfx::current_frame();
  }
  return handle;
}

gfx::TextureHandle resolve_static_texture(const GXTexObj_& obj) {
  ZoneScoped;
  if (s_staticTextureCacheClearPending.exchange(false, std::memory_order_acq_rel)) {
    do_clear_static_texture_cache();
  }

  if (obj.texObjId != 0) {
    if (const auto it = s_textureObjectCaches.find(obj.texObjId); it != s_textureObjectCaches.end()) {
      auto& entry = it->second;
      if (entry.handle && entry.texDataVersion == obj.texDataVersion && entry.tlutObjId == 0) {
        entry.lastUsedFrame = gfx::current_frame();
        return entry.handle;
      }
    }
  }

  gfx::TextureHandle handle;
  if (const auto replacement = gfx::texture_replacement::find_replacement(obj); replacement.has_value()) {
    handle = *replacement;
  } else {
#if DEBUG
    const auto name = gfx::texture_replacement::build_texture_replacement_name(obj);
    const auto nameStr = name.c_str();
#else
    const auto nameStr = "GX Static Texture";
#endif
    handle = gfx::new_static_texture_2d(obj.width(), obj.height(), obj.mip_count(), obj.format(),
                                        {static_cast<const uint8_t*>(obj.data), UINT32_MAX}, false, nameStr);
  }
  if (!obj.no_cache()) {
    store_cached_texture(obj, handle);
  }
  return handle;
}

gfx::TextureHandle resolve_static_palette_texture(const GXTexObj_& obj, const GXTlutObj_& tlut) {
  ZoneScoped;
  if (s_staticTextureCacheClearPending.exchange(false, std::memory_order_acq_rel)) {
    do_clear_static_texture_cache();
  }

  if (obj.texObjId != 0) {
    if (const auto it = s_textureObjectCaches.find(obj.texObjId); it != s_textureObjectCaches.end()) {
      auto& entry = it->second;
      if (entry.handle && entry.texDataVersion == obj.texDataVersion && entry.tlutObjId == tlut.tlutObjId &&
          entry.tlutDataVersion == tlut.tlutDataVersion) {
        entry.lastUsedFrame = gfx::current_frame();
        return entry.handle;
      }
    }
  }

  gfx::TextureHandle handle;
  if (const auto replacement = gfx::texture_replacement::find_replacement(obj, tlut); replacement.has_value()) {
    handle = *replacement;
  } else {
    auto converted = gfx::convert_texture_palette(
        obj.format(), obj.width(), obj.height(), obj.mip_count(), {static_cast<const u8*>(obj.data), UINT32_MAX},
        tlut.format, tlut.numEntries, {static_cast<const u8*>(tlut.data), static_cast<size_t>(tlut.numEntries) * 2});
    if (converted.data.empty()) {
      return {};
    }
    handle =
        gfx::new_static_texture_2d(obj.width(), obj.height(), obj.mip_count(), GX_TF_RGBA8_PC,
                                   {converted.data.data(), converted.data.size()}, false, "GX Static Palette Texture");
    handle->hasArbitraryMips = converted.hasArbitraryMips;
  }
  if (!obj.no_cache() && !tlut.no_cache()) {
    store_cached_texture(obj, handle, tlut.tlutObjId, tlut.tlutDataVersion);
  }
  return handle;
}

gfx::TextureHandle resolve_dynamic_palette_texture(const GXTexObj_& obj, const GXState::CopyTextureRef& source,
                                                   const GXTlutObj_& tlut) {
  ZoneScoped;

  const auto tlutHandle = get_tlut_texture(tlut);
  auto& tlutCache = s_tlutObjectCaches[tlut.tlutObjId];
  auto& entry = tlutCache.dynamicPaletteTextures[make_dynamic_palette_key(obj, source)];
  entry.lastUsedFrame = gfx::current_frame();
  if (!entry.handle) {
    // Use source size instead of target (logical) size
    entry.handle = gfx::new_conv_texture(source.handle->size.width, source.handle->size.height, GX_TF_RGBA8,
                                         "GX Dynamic Palette Texture");
  }
  if (entry.sourceRevision != source.revision || entry.tlutDataVersion != tlut.tlutDataVersion) {
    gfx::queue_palette_conv({
        .variant = obj.format() == GX_TF_C4 ? gfx::tex_palette_conv::Variant::FromFloat4
                                            : gfx::tex_palette_conv::Variant::FromFloat8,
        .src = source.handle,
        .dst = entry.handle,
        .tlut = tlutHandle,
    });
    entry.sourceRevision = source.revision;
    entry.tlutDataVersion = tlut.tlutDataVersion;
  }
  return entry.handle;
}

u32 resolved_format_for_handle(const gfx::TextureHandle& handle) {
  if (!handle) {
    return GX_TF_RGBA8;
  }
  if (handle->gxFormat != gfx::InvalidTextureFormat) {
    return handle->gxFormat;
  }
  return GX_TF_RGBA8_PC;
}

template <typename T>
T round_away_from_zero(float value) noexcept {
  return static_cast<T>(value < 0.0f ? std::floor(value) : std::ceil(value));
}

std::pair<f32, f32> polygon_offset_for_cull_mode(GXCullMode cullMode) noexcept {
  if (cullMode == GX_CULL_FRONT) {
    return {g_gxState.backOffset, g_gxState.backScale};
  }
  return {g_gxState.frontOffset, g_gxState.frontScale};
}
} // namespace

Vec2<uint32_t> logical_fb_size() noexcept {
  return gfx::is_offscreen() ? gfx::get_render_target_size() : vi::configured_fb_size();
}

gfx::Viewport map_logical_viewport(const gfx::Viewport& logicalViewport) noexcept {
  if (g_gxState.viewportPolicy == AURORA_VIEWPORT_NATIVE) {
    return logicalViewport;
  }

  const auto [logicalFbWidth, logicalFbHeight] = logical_fb_size();
  const auto [targetWidth, targetHeight] = gfx::get_render_target_size();
  if (logicalFbWidth == 0 || logicalFbHeight == 0 || targetWidth == 0 || targetHeight == 0) {
    return logicalViewport;
  }

  const float scaleX = static_cast<float>(targetWidth) / static_cast<float>(logicalFbWidth);
  const float scaleY = static_cast<float>(targetHeight) / static_cast<float>(logicalFbHeight);
  return {
      .left = logicalViewport.left * scaleX,
      .top = logicalViewport.top * scaleY,
      .width = logicalViewport.width * scaleX,
      .height = logicalViewport.height * scaleY,
      .znear = logicalViewport.znear,
      .zfar = logicalViewport.zfar,
  };
}

gfx::ClipRect map_logical_scissor(const gfx::ClipRect& logicalScissor) noexcept {
  if (g_gxState.viewportPolicy == AURORA_VIEWPORT_NATIVE) {
    return logicalScissor;
  }

  const auto [logicalFbWidth, logicalFbHeight] = logical_fb_size();
  const auto [targetWidth, targetHeight] = gfx::get_render_target_size();
  if (logicalFbWidth == 0 || logicalFbHeight == 0 || targetWidth == 0 || targetHeight == 0) {
    return logicalScissor;
  }

  const float scaleX = static_cast<float>(targetWidth) / static_cast<float>(logicalFbWidth);
  const float scaleY = static_cast<float>(targetHeight) / static_cast<float>(logicalFbHeight);

  const float left = static_cast<float>(logicalScissor.x) * scaleX;
  const float top = static_cast<float>(logicalScissor.y) * scaleY;
  const float right = static_cast<float>(logicalScissor.x + logicalScissor.width) * scaleX;
  const float bottom = static_cast<float>(logicalScissor.y + logicalScissor.height) * scaleY;

  const auto mappedLeft = std::clamp(static_cast<int32_t>(std::floor(left)), 0, static_cast<int32_t>(targetWidth));
  const auto mappedTop = std::clamp(static_cast<int32_t>(std::floor(top)), 0, static_cast<int32_t>(targetHeight));
  const auto mappedRight =
      std::clamp(static_cast<int32_t>(std::ceil(right)), mappedLeft, static_cast<int32_t>(targetWidth));
  const auto mappedBottom =
      std::clamp(static_cast<int32_t>(std::ceil(bottom)), mappedTop, static_cast<int32_t>(targetHeight));

  return {
      .x = mappedLeft,
      .y = mappedTop,
      .width = mappedRight - mappedLeft,
      .height = mappedBottom - mappedTop,
  };
}

void set_logical_viewport(const gfx::Viewport& viewport) noexcept {
  g_gxState.logicalViewport = viewport;
  set_render_viewport(map_logical_viewport(viewport));
}

void set_render_viewport(const gfx::Viewport& viewport) noexcept {
  g_gxState.renderViewport = viewport;
  gfx::set_viewport(viewport);
}

void set_logical_scissor(const gfx::ClipRect& scissor) noexcept {
  g_gxState.logicalScissor = scissor;
  set_render_scissor(map_logical_scissor(g_gxState.logicalScissor));
}

void set_render_scissor(const gfx::ClipRect& scissor) noexcept {
  g_gxState.renderScissor = scissor;
  gfx::set_scissor(scissor);
}

const gfx::TextureBind& get_texture(GXTexMapID id) noexcept { return g_gxState.textures[static_cast<size_t>(id)]; }

void evict_texture_object(u32 texObjId) noexcept {
  if (const auto it = s_textureObjectCaches.find(texObjId); it != s_textureObjectCaches.end()) {
    clear_texture_dependency(texObjId, it->second.tlutObjId);
    s_textureObjectCaches.erase(it);
  }
  // If there is a loaded slot with this ID, mark it as no_cache to avoid inserting it when it's resolved.
  // This also handles the case where the texture was created, loaded, and immediately destroyed before we resolved it.
  for (auto& obj : g_gxState.loadedTextures) {
    if (obj.texObjId == texObjId) {
      obj.set_no_cache(true);
    }
  }
}

void evict_tlut_object(u32 tlutObjId) noexcept {
  if (const auto it = s_tlutObjectCaches.find(tlutObjId); it != s_tlutObjectCaches.end()) {
    for (const u32 texObjId : it->second.staticTextureUsers) {
      s_textureObjectCaches.erase(texObjId);
    }
    s_tlutObjectCaches.erase(it);
  }
  // If there is a loaded slot with this ID, mark it as no_cache to avoid inserting it when it's resolved.
  // This also handles the case where the texture was created, loaded, and immediately destroyed before we resolved it.
  for (auto& obj : g_gxState.loadedTluts) {
    if (obj.tlutObjId == tlutObjId) {
      obj.set_no_cache(true);
    }
  }
}

void clear_copy_texture_cache() noexcept {
  g_gxState.copyTextures.clear();
  g_gxState.copyTextureCache.clear();
  for (auto& [_, cache] : s_tlutObjectCaches) {
    cache.dynamicPaletteTextures.clear();
  }
}

void clear_static_texture_cache() noexcept { s_staticTextureCacheClearPending.store(true, std::memory_order_release); }

void expire_texture_object_caches() noexcept {
  // Ages out cache entries whose game-side GXTexObj is gone. GXInitTexObj mints a fresh
  // texObjId every call and almost nothing calls GXDestroyTexObj (the game frees textures
  // by wholesale JKR heap teardown on area/actor unload), so without this sweep every
  // orphaned entry pins its GL texture forever -- the ~1 texture/s live-count climb that
  // OOMs the 1 GB G31. Eviction is safe: a later use is a cache miss that re-decodes from
  // obj.data, which must be alive for the texobj to be drawn again.
  constexpr u32 SweepPeriod = 64;
  constexpr u32 RetainFrames = 1800; // ~60 s at 30 Hz

  const u32 frame = gfx::current_frame();
  if (frame == UINT32_MAX || frame % SweepPeriod != 0) {
    return;
  }
  ZoneScoped;

  // Anything currently referenced by a texmap/tlut slot must survive regardless of age: a
  // texture can stay bound (and drawn) indefinitely without re-resolving, because
  // resolve_sampled_textures short-circuits on an unchanged bind.
  absl::flat_hash_set<u32> boundTexObjs;
  absl::flat_hash_set<u32> boundTluts;
  for (const auto& obj : g_gxState.loadedTextures) {
    if (obj.texObjId != 0) {
      boundTexObjs.insert(obj.texObjId);
    }
  }
  for (const auto& bind : g_gxState.textures) {
    if (bind.texObj.texObjId != 0) {
      boundTexObjs.insert(bind.texObj.texObjId);
    }
  }
  for (const auto& tlut : g_gxState.loadedTluts) {
    if (tlut.tlutObjId != 0) {
      boundTluts.insert(tlut.tlutObjId);
    }
  }

  for (auto it = s_textureObjectCaches.begin(); it != s_textureObjectCaches.end();) {
    if (!boundTexObjs.contains(it->first) && frame - it->second.lastUsedFrame > RetainFrames) {
      clear_texture_dependency(it->first, it->second.tlutObjId);
      s_textureObjectCaches.erase(it++);
    } else {
      ++it;
    }
  }
  for (auto it = s_tlutObjectCaches.begin(); it != s_tlutObjectCaches.end();) {
    auto& cache = it->second;
    for (auto dp = cache.dynamicPaletteTextures.begin(); dp != cache.dynamicPaletteTextures.end();) {
      if (frame - dp->second.lastUsedFrame > RetainFrames) {
        cache.dynamicPaletteTextures.erase(dp++);
      } else {
        ++dp;
      }
    }
    if (cache.tlutTexture.handle && !boundTluts.contains(it->first) &&
        frame - cache.tlutTexture.lastUsedFrame > RetainFrames) {
      cache.tlutTexture = {};
    }
    if (!cache.tlutTexture.handle && cache.dynamicPaletteTextures.empty() && cache.staticTextureUsers.empty()) {
      s_tlutObjectCaches.erase(it++);
    } else {
      ++it;
    }
  }
}

void evict_copy_texture(const void* dest) noexcept {
  absl::flat_hash_set<const void*> sourceIdentities;
  if (const auto it = g_gxState.copyTextures.find(dest); it != g_gxState.copyTextures.end()) {
    if (it->second.handle) {
      sourceIdentities.insert(it->second.handle.get());
    }
    g_gxState.copyTextures.erase(it);
  }

  for (auto it = g_gxState.copyTextureCache.begin(); it != g_gxState.copyTextureCache.end();) {
    if (it->first.dest == dest) {
      if (it->second.handle) {
        sourceIdentities.insert(it->second.handle.get());
      }
      g_gxState.copyTextureCache.erase(it++);
    } else {
      ++it;
    }
  }

  if (sourceIdentities.empty()) {
    return;
  }

  for (auto& [_, cache] : s_tlutObjectCaches) {
    for (auto it = cache.dynamicPaletteTextures.begin(); it != cache.dynamicPaletteTextures.end();) {
      if (sourceIdentities.contains(it->first.sourceIdentity)) {
        cache.dynamicPaletteTextures.erase(it++);
      } else {
        ++it;
      }
    }
  }
}

void resolve_sampled_textures(const ShaderInfo& info) noexcept {
  ZoneScoped;

  for (u32 i = 0; i < MaxTextures; ++i) {
    if (!info.sampledTextures.test(i)) {
      continue;
    }

    GXTexObj_ obj = g_gxState.loadedTextures[i];
    auto& textureBind = g_gxState.textures[i];
    if (obj.texObjId != 0 && obj.texObjId == textureBind.texObj.texObjId &&
        obj.texDataVersion == textureBind.texObj.texDataVersion) {
      // Texture bind unchanged
      continue;
    }

    gfx::TextureHandle handle;
    const auto copyIt = g_gxState.copyTextures.find(obj.data);
    const GXState::CopyTextureRef* copyRef = copyIt != g_gxState.copyTextures.end() ? &copyIt->second : nullptr;
    if (is_palette_format(obj.format())) {
      const auto tlutIdx = static_cast<size_t>(obj.tlut);
      if (tlutIdx < g_gxState.loadedTluts.size()) {
        const auto& tlut = g_gxState.loadedTluts[tlutIdx];
        if (tlut.data != nullptr) {
          if (copyRef != nullptr) {
            handle = resolve_dynamic_palette_texture(obj, *copyRef, tlut);
          } else if (obj.has_data()) {
            handle = resolve_static_palette_texture(obj, tlut);
          }
        }
      }
    } else if (copyRef != nullptr) {
      handle = copyRef->handle;
    } else if (obj.has_data()) {
      handle = resolve_static_texture(obj);
    }

    obj.mFormat = resolved_format_for_handle(handle);
    textureBind = gfx::TextureBind{obj, std::move(handle)};
  }
}

static inline gfx::BlendFactor to_blend_factor(GXBlendFactor fac, bool isDst) {
  switch (fac) {
    DEFAULT_FATAL("invalid blend factor {}", underlying(fac));
  case GX_BL_ZERO:
    return gfx::BlendFactor::Zero;
  case GX_BL_ONE:
    return gfx::BlendFactor::One;
  case GX_BL_SRCCLR: // + GX_BL_DSTCLR
    if (isDst) {
      return gfx::BlendFactor::Src;
    } else {
      return gfx::BlendFactor::Dst;
    }
  case GX_BL_INVSRCCLR: // + GX_BL_INVDSTCLR
    if (isDst) {
      return gfx::BlendFactor::OneMinusSrc;
    } else {
      return gfx::BlendFactor::OneMinusDst;
    }
  case GX_BL_SRCALPHA:
    return gfx::BlendFactor::SrcAlpha;
  case GX_BL_INVSRCALPHA:
    return gfx::BlendFactor::OneMinusSrcAlpha;
  case GX_BL_DSTALPHA:
    return gfx::BlendFactor::DstAlpha;
  case GX_BL_INVDSTALPHA:
    return gfx::BlendFactor::OneMinusDstAlpha;
  }
}

static inline gfx::CompareFunction to_compare_function(GXCompare func) {
  switch (func) {
    DEFAULT_FATAL("invalid depth fn {}", underlying(func));
  case GX_NEVER:
    return gfx::CompareFunction::Never;
  case GX_LESS:
    return UseReversedZ ? gfx::CompareFunction::Greater : gfx::CompareFunction::Less;
  case GX_EQUAL:
    return gfx::CompareFunction::Equal;
  case GX_LEQUAL:
    return UseReversedZ ? gfx::CompareFunction::GreaterEqual : gfx::CompareFunction::LessEqual;
  case GX_GREATER:
    return UseReversedZ ? gfx::CompareFunction::Less : gfx::CompareFunction::Greater;
  case GX_NEQUAL:
    return gfx::CompareFunction::NotEqual;
  case GX_GEQUAL:
    return UseReversedZ ? gfx::CompareFunction::LessEqual : gfx::CompareFunction::GreaterEqual;
  case GX_ALWAYS:
    return gfx::CompareFunction::Always;
  }
}

// The color+alpha blend equation, resolved from the GX blend/logic-op state. GL
// has no BlendState object; these six fields feed glBlendEquationSeparate /
// glBlendFuncSeparate when the pipeline's state is applied (Phase 3).
struct GLBlendComponents {
  gfx::BlendOperation colorOp = gfx::BlendOperation::Add;
  gfx::BlendFactor colorSrc = gfx::BlendFactor::One;
  gfx::BlendFactor colorDst = gfx::BlendFactor::Zero;
  gfx::BlendOperation alphaOp = gfx::BlendOperation::Add;
  gfx::BlendFactor alphaSrc = gfx::BlendFactor::One;
  gfx::BlendFactor alphaDst = gfx::BlendFactor::Zero;
};

static inline GLBlendComponents to_blend_state(GXBlendMode mode, GXBlendFactor srcFac, GXBlendFactor dstFac,
                                               GXLogicOp op, u32 dstAlpha) {
  GLBlendComponents blend;
  switch (mode) {
    DEFAULT_FATAL("unsupported blend mode {}", underlying(mode));
  case GX_BM_NONE:
    blend.colorOp = gfx::BlendOperation::Add;
    blend.colorSrc = gfx::BlendFactor::One;
    blend.colorDst = gfx::BlendFactor::Zero;
    break;
  case GX_BM_BLEND:
    blend.colorOp = gfx::BlendOperation::Add;
    blend.colorSrc = to_blend_factor(srcFac, false);
    blend.colorDst = to_blend_factor(dstFac, true);
    break;
  case GX_BM_SUBTRACT:
    blend.colorOp = gfx::BlendOperation::ReverseSubtract;
    blend.colorSrc = gfx::BlendFactor::One;
    blend.colorDst = gfx::BlendFactor::One;
    break;
  case GX_BM_LOGIC:
    switch (op) {
      DEFAULT_FATAL("unsupported logic op {}", underlying(op));
    case GX_LO_CLEAR:
      blend.colorOp = gfx::BlendOperation::Add;
      blend.colorSrc = gfx::BlendFactor::Zero;
      blend.colorDst = gfx::BlendFactor::Zero;
      break;
    case GX_LO_COPY:
      blend.colorOp = gfx::BlendOperation::Add;
      blend.colorSrc = gfx::BlendFactor::One;
      blend.colorDst = gfx::BlendFactor::Zero;
      break;
    case GX_LO_NOOP:
      blend.colorOp = gfx::BlendOperation::Add;
      blend.colorSrc = gfx::BlendFactor::Zero;
      blend.colorDst = gfx::BlendFactor::One;
      break;
    }
    break;
  }
  if (dstAlpha != UINT32_MAX) {
    blend.alphaOp = gfx::BlendOperation::Add;
    blend.alphaSrc = gfx::BlendFactor::Constant;
    blend.alphaDst = gfx::BlendFactor::Zero;
  } else {
    blend.alphaOp = blend.colorOp;
    blend.alphaSrc = blend.colorSrc;
    blend.alphaDst = blend.colorDst;
  }
  return blend;
}

static inline gfx::ColorWriteMask to_write_mask(bool colorUpdate, bool alphaUpdate) {
  gfx::ColorWriteMask writeMask = gfx::ColorWriteMask::None;
  if (colorUpdate) {
    writeMask = writeMask | gfx::ColorWriteMask::Red | gfx::ColorWriteMask::Green | gfx::ColorWriteMask::Blue;
  }
  if (alphaUpdate) {
    writeMask = writeMask | gfx::ColorWriteMask::Alpha;
  }
  return writeMask;
}

static inline gfx::CullMode to_cull_mode(GXCullMode gx_cullMode) {
  switch (gx_cullMode) {
  case GX_CULL_FRONT:
    return gfx::CullMode::Front;
  case GX_CULL_BACK:
  case GX_CULL_ALL:
    return gfx::CullMode::Back;
  case GX_CULL_NONE:
    return gfx::CullMode::None;
  default:
    return gfx::CullMode::None;
  }
}

// Bake the GX pipeline state into the backend-neutral BakedState. `program` is the
// already-linked GLSL program (0 during Phase 1, where draws don't execute yet).
// The fixed-function state here is applied by lib/gl/state.cpp at draw time; note
// FrontFace stays in WebGPU terms (CW) and the S1a winding flip happens once there.
gl::Pipeline build_pipeline(const PipelineConfig& config, uint32_t program, const char* label) noexcept {
  ZoneScoped;
  const float depthBias = (UseReversedZ ? -1.0f : 1.0f) * std::bit_cast<float>(config.polygonOffsetBits);
  const float depthBiasSlopeScale = (UseReversedZ ? -1.0f : 1.0f) * std::bit_cast<float>(config.polygonOffsetScaleBits);

  const auto blend = to_blend_state(config.blendMode, config.blendFacSrc, config.blendFacDst, config.blendOp,
                                    config.dstAlpha);

  gl::BakedState state{};
  // Blend is always attached (matches Dawn always supplying a blend state); an
  // One/Zero/Add equation is an identity passthrough, so this is a no-op there.
  state.blendEnabled = true;
  state.colorOp = blend.colorOp;
  state.colorSrc = blend.colorSrc;
  state.colorDst = blend.colorDst;
  state.alphaOp = blend.alphaOp;
  state.alphaSrc = blend.alphaSrc;
  state.alphaDst = blend.alphaDst;
  state.writeMask = config.cullMode == GX_CULL_ALL ? gfx::ColorWriteMask::None
                                                   : to_write_mask(config.colorUpdate, config.alphaUpdate);

  state.depthTest = config.depthCompare;
  state.depthWrite = config.depthCompare && config.depthUpdate;
  state.depthCompare = config.depthCompare ? to_compare_function(config.depthFunc) : gfx::CompareFunction::Always;

  state.cull = to_cull_mode(config.cullMode);
  state.frontFace = gfx::FrontFace::CW;
  state.polygonOffset = depthBias != 0.0f || depthBiasSlopeScale != 0.0f;
  state.depthBiasSlope = depthBiasSlopeScale;
  state.depthBiasUnits = depthBias;

  // TriangleStrip batches separate strips with the max Uint16 index (0xffff) as an
  // implicit primitive restart; a triangle *list* must NOT enable restart (S6).
  state.topology =
      config.triangleStripTopology != 0 ? gfx::PrimitiveTopology::TriangleStrip : gfx::PrimitiveTopology::TriangleList;
  state.primitiveRestart = config.triangleStripTopology != 0;

  // Native vertex layout as a present-attr bitmask over canonical GX attr order
  // (GX_VA_PNMTXIDX..GX_VA_TEX7). The draw path (pass.cpp) decodes it into the VAO;
  // bit positions == GXAttr values == the GLSL emitter's packed `layout(location)`.
  uint32_t vertexLayout = 0;
  for (int i = GX_VA_PNMTXIDX; i <= GX_VA_TEX7; ++i) {
    if (config.shaderConfig.attrs[i].attrType != GX_NONE) {
      vertexLayout |= (1u << i);
    }
  }

  return gl::Pipeline{
      .program = program,
      .state = state,
      .vertexLayout = vertexLayout,
  };
}

void populate_pipeline_config(PipelineConfig& config, GXPrimitive primitive, GXVtxFmt fmt) noexcept {
  ZoneScoped;

  const auto& vtxFmt = g_gxState.vtxFmts[fmt];
  config.shaderConfig.fogType = g_gxState.fog.type;
  u8 vtxOffset = 0;
  for (int i = GX_VA_PNMTXIDX; i <= GX_VA_TEX7; ++i) {
    const auto attr = static_cast<GXAttr>(i);
    const auto type = g_gxState.vtxDesc[i];
    auto& mapping = config.shaderConfig.attrs[i];
    if (type == GX_NONE) {
      mapping = {};
      continue;
    }
    const auto& attrFmt = vtxFmt.attrs[i];
    const auto cnt = comp_cnt_count(attr, attrFmt.cnt);
    const bool nbt3 = attr == GX_VA_NRM && attrFmt.cnt == GX_NRM_NBT3;
    mapping = AttrConfig{
        .attrType = static_cast<u8>(type),
        .cnt = cnt,
        .compType = static_cast<u8>(attrFmt.type),
        .offset = vtxOffset,
        .stride = 0,
        .frac = attrFmt.frac,
        .le = false,
        .nbt3 = nbt3,
    };
    switch (type) {
    case GX_DIRECT: {
      vtxOffset += comp_type_size(attr, attrFmt.type) * cnt;
      break;
    }
    case GX_INDEX8:
      mapping.stride = g_gxState.arrays[i].stride;
      mapping.le = g_gxState.arrays[i].le;
      vtxOffset += nbt3 ? 3 : 1;
      break;
    case GX_INDEX16:
      mapping.stride = g_gxState.arrays[i].stride;
      mapping.le = g_gxState.arrays[i].le;
      vtxOffset += nbt3 ? 6 : 2;
      break;
    default:
      Log.fatal("populate_pipeline_config: Invalid vertex type {}", type);
    }
  }
  config.shaderConfig.vtxStride = vtxOffset;
  if (primitive == GX_LINES) {
    config.shaderConfig.lineMode = 1;
  } else if (primitive == GX_LINESTRIP) {
    config.shaderConfig.lineMode = 2;
  } else if (primitive == GX_POINTS) {
    config.shaderConfig.lineMode = 3;
  } else {
    config.shaderConfig.lineMode = 0;
  }
  config.shaderConfig.tevSwapTable = g_gxState.tevSwapTable;
  for (u8 i = 0; i < g_gxState.numTevStages; ++i) {
    config.shaderConfig.tevStages[i] = g_gxState.tevStages[i];
  }
  config.shaderConfig.tevStageCount = g_gxState.numTevStages;
  for (u8 i = 0; i < g_gxState.numIndStages; ++i) {
    config.shaderConfig.indStages[i] = g_gxState.indStages[i];
  }
  config.shaderConfig.numIndStages = g_gxState.numIndStages;
  for (u8 i = 0; i < MaxColorChannels; ++i) {
    const auto& cc = g_gxState.colorChannelConfig[i];
    if (cc.lightingEnabled) {
      config.shaderConfig.colorChannels[i] = cc;
    } else {
      // Only matSrc matters when lighting disabled
      config.shaderConfig.colorChannels[i] = {
          .matSrc = cc.matSrc,
      };
    }
  }
  for (u8 i = 0; i < g_gxState.numTexGens; ++i) {
    config.shaderConfig.tcgs[i] = g_gxState.tcgs[i];
  }
  if (g_gxState.alphaCompare) {
    config.shaderConfig.alphaCompare = g_gxState.alphaCompare;
  }
  const auto cullMode = config.shaderConfig.lineMode == 0 ? g_gxState.cullMode : GX_CULL_NONE;
  const auto [polygonOffset, polygonOffsetScale] = polygon_offset_for_cull_mode(cullMode);
  config = {
      .msaaSamples = gfx::get_sample_count(),
      .shaderConfig = config.shaderConfig,
      .depthFunc = g_gxState.depthFunc,
      .cullMode = cullMode,
      .blendMode = g_gxState.blendMode,
      .blendFacSrc = g_gxState.blendFacSrc,
      .blendFacDst = g_gxState.blendFacDst,
      .blendOp = g_gxState.blendOp,
      .dstAlpha = g_gxState.dstAlpha,
      .polygonOffsetBits = std::bit_cast<uint32_t>(polygonOffset),
      .polygonOffsetScaleBits = std::bit_cast<uint32_t>(polygonOffsetScale),
      .polygonOffsetClampBits = std::bit_cast<uint32_t>(g_gxState.clamp),
      .depthCompare = g_gxState.depthCompare,
      .depthUpdate = cullMode == GX_CULL_ALL ? false : g_gxState.depthUpdate,
      .alphaUpdate = cullMode == GX_CULL_ALL ? false : g_gxState.alphaUpdate,
      .colorUpdate = cullMode == GX_CULL_ALL ? false : g_gxState.colorUpdate,
  };
}

GXBindGroups build_bind_groups(const ShaderInfo& info) noexcept {
  ZoneScoped;

  if (!info.sampledTextures.any() && !info.sampledIndTextures.any()) {
    // Don't bother re-binding anything
    return {};
  }

  // Up to 8 texture+sampler pairs; unused slots get the 1x1 empty fill. Texture
  // unit i == GX slot i; the GLSL sampler uniforms are bound to those units at link.
  gl::BindingSet set{};
  for (u32 i = 0; i < MaxTextures; ++i) {
    const auto& tex = g_gxState.textures[i];
    auto& binding = set.textures[i];
    if (tex && (info.sampledTextures[i] || info.sampledIndTextures[i])) {
      binding.texture = tex.ref->sampleTextureView.id;
      binding.sampler = gfx::sampler_ref(tex.get_descriptor()).id;
    } else {
      binding.texture = sEmptyTexture.id;
      binding.sampler = sEmptySampler.id;
    }
  }
  return {
      .textureBindGroup = gfx::bind_group_ref(set),
  };
}

void initialize() noexcept {
  // GL has no bind-group-layout / pipeline-layout objects to build. But unused GX
  // texture slots still need a *real* GL texture + sampler bound: a slot left at id 0
  // binds the incomplete default texture, which shows up as glBindTexture(No Resource)
  // in captures and which strict drivers (Mali/PowerVR) can reject on a draw whose
  // GLSL declares that sampler. Build a 1x1 fill here. These are GL objects, so they
  // must be created on the render worker (initialize() runs on the main thread, which
  // does not own the render context); create_gl_texture/create_gl_sampler marshal the
  // work to the worker and block. `renderable=true` makes create_texture zero-fill
  // level 0, so the fill reads opaque black rather than sampling uninitialized GPU
  // memory (Normalcy Doctrine) — the flag is otherwise inert for a 1x1 that is only
  // ever bound as a sampler, never an FBO attachment.
  sEmptyTexture = gfx::create_gl_texture(webgpu::g_graphicsConfig.surfaceConfiguration.format, gl::Extent3D{1, 1, 1}, 1,
                                         /*renderable=*/true);
  sEmptySampler = gfx::create_gl_sampler(gl::SamplerDescriptor{});
  g_emptyTextureBindGroup = gl::BindingSet{};
}

void shutdown() noexcept {
  sEmptySampler = {};
  sEmptyTexture = {};
  g_emptyTextureBindGroup = {};
  for (auto& item : g_gxState.textures) {
    item.ref.reset();
  }
  s_textureObjectCaches.clear();
  s_tlutObjectCaches.clear();
  g_gxState.loadedTextures.fill({});
  g_gxState.loadedTluts.fill({});
  clear_copy_texture_cache();
  clear_shader_program_cache();
}
} // namespace aurora::gx

namespace aurora {
static gl::AddressMode gl_address_mode(GXTexWrapMode mode) {
  switch (mode) {
    DEFAULT_FATAL("invalid wrap mode {}", underlying(mode));
  case GX_CLAMP:
    return gl::AddressMode::ClampToEdge;
  case GX_REPEAT:
    return gl::AddressMode::Repeat;
  case GX_MIRROR:
    return gl::AddressMode::MirrorRepeat;
  }
}

static std::pair<gl::FilterMode, gl::MipmapFilterMode> gl_filter_mode(GXTexFilter filter) {
  switch (filter) {
    DEFAULT_FATAL("invalid filter mode {}", static_cast<int>(filter));
  case GX_NEAR:
    return {gl::FilterMode::Nearest, gl::MipmapFilterMode::Undefined};
  case GX_LINEAR:
    return {gl::FilterMode::Linear, gl::MipmapFilterMode::Undefined};
  case GX_NEAR_MIP_NEAR:
    return {gl::FilterMode::Nearest, gl::MipmapFilterMode::Nearest};
  case GX_LIN_MIP_NEAR:
    return {gl::FilterMode::Linear, gl::MipmapFilterMode::Nearest};
  case GX_NEAR_MIP_LIN:
    return {gl::FilterMode::Nearest, gl::MipmapFilterMode::Linear};
  case GX_LIN_MIP_LIN:
    return {gl::FilterMode::Linear, gl::MipmapFilterMode::Linear};
  }
}

static u16 gl_aniso(GXAnisotropy aniso) {
  switch (aniso) {
    DEFAULT_FATAL("invalid aniso {}", static_cast<int>(aniso));
  case GX_ANISO_1:
  case GX_MAX_ANISOTROPY:
    return 1;
  case GX_ANISO_2:
    return std::max<u16>(aurora::webgpu::g_graphicsConfig.textureAnisotropy / 2, 1);
  case GX_ANISO_4:
    return std::max<u16>(aurora::webgpu::g_graphicsConfig.textureAnisotropy, 1);
  }
}

gl::SamplerDescriptor aurora::gfx::TextureBind::get_descriptor() const noexcept {
  auto [minFilter, mipFilter] = gl_filter_mode(texObj.min_filter());
  auto [magFilter, _] = gl_filter_mode(texObj.mag_filter());
  const bool mipsEnabled = mipFilter != gl::MipmapFilterMode::Undefined;
  float minLod = texObj.min_lod();
  float maxLod = texObj.max_lod();
  u16 maxAnisotropy = gl_aniso(texObj.max_aniso());
  if (ref && ref->isReplacement) {
    minLod = 0.f;
    maxLod = 1000.f;
    if (!mipsEnabled) {
      mipFilter = gl::MipmapFilterMode::Nearest;
    }
  } else if (mipFilter == gl::MipmapFilterMode::Undefined) {
    minLod = 0.f;
    maxLod = 0.f;
  }
  if ((ref && ref->hasArbitraryMips) || !mipsEnabled) {
    maxAnisotropy = 1;
  } else if (maxAnisotropy > 1) {
    magFilter = gl::FilterMode::Linear;
    minFilter = gl::FilterMode::Linear;
    mipFilter = gl::MipmapFilterMode::Linear;
  }
  return {
      .addressU = gl_address_mode(texObj.wrap_s()),
      .addressV = gl_address_mode(texObj.wrap_t()),
      .addressW = gl::AddressMode::Repeat,
      .magFilter = magFilter,
      .minFilter = minFilter,
      .mipmapFilter = mipFilter,
      .lodMinClamp = minLod,
      .lodMaxClamp = maxLod,
      .maxAnisotropy = maxAnisotropy,
  };
}
} // namespace aurora

#pragma once

#include <aurora/aurora.h>
#include <aurora/math.hpp>

#include "../gl/handles.hpp"
#include "../gl/pass.hpp"
#include "../gl/textures.hpp"
#include "../gl/buffers.hpp"

#include <array>
#include <cstdint>

struct SDL_Window;

// The hand-rolled GLES backend. This interface kept the historical
// `aurora::webgpu` namespace and function names through the Dawn->GL cutover so
// the ~150 call sites did not have to churn; the *types* are now GL
// (gl::Texture/Buffer/Sampler, gl vocabulary) instead of wgpu::. lib/gl/device.cpp
// implements it. (Phase 7 renames the namespace/paths to gl::.)
namespace aurora::webgpu {
struct SurfaceConfiguration {
  gl::TextureFormat format = gl::TextureFormat::RGBA8Unorm;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t presentMode = 0; // vsync hint; GL present uses SDL_GL_SetSwapInterval
};
struct GraphicsConfig {
  SurfaceConfiguration surfaceConfiguration;
  gl::TextureFormat depthFormat = gl::TextureFormat::Depth32Float;
  uint32_t msaaSamples = 1;
  uint16_t textureAnisotropy = 0;
};
// One texture (+ its sampler). WebGPU's separate TextureView collapses into the
// texture on GL, so `view` is just a copy of `texture`; call sites that passed a
// view as a pass attachment now pass the texture handle.
struct TextureWithSampler {
  gl::Texture texture;
  gl::Texture view;
  gl::Extent3D size;
  gl::TextureFormat format = gl::TextureFormat::Undefined;
  gl::Sampler sampler;
};
struct Viewport {
  float left;
  float top;
  float width;
  float height;
  float znear;
  float zfar;

  bool operator==(const Viewport& rhs) const {
    return left == rhs.left && top == rhs.top && width == rhs.width && height == rhs.height && znear == rhs.znear &&
           zfar == rhs.zfar;
  }
  bool operator!=(const Viewport& rhs) const { return !(*this == rhs); }
};

extern AuroraBackend g_backendType; // resolves to OpenGLES on every live path
extern GraphicsConfig g_graphicsConfig;
extern TextureWithSampler g_frameBuffer;
extern TextureWithSampler g_frameBufferResolved;
extern TextureWithSampler g_depthBuffer;
extern bool g_hasCoreFeatures;
extern bool g_bcTexturesSupported;
extern bool g_astcTexturesSupported;
extern bool g_textureComponentSwizzleSupported;
// GL_EXT_buffer_storage present AND glBufferStorage resolved: enables persistent-mapped
// write-combine buffers (the device fast path that bypasses libmali's copy worker). When false,
// gl::create_buffer/upload_buffer transparently fall back to glBufferData + glBufferSubData.
extern bool g_bufferStorageSupported;
// GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, queried at init (replaces Dawn's
// minUniformBufferOffsetAlignment limit). Feeds gfx::align_uniform.
extern uint32_t g_uniformBufferOffsetAlignment;

bool initialize(AuroraBackend backend, bool allowCpu);
void shutdown();
void release_surface() noexcept;
bool refresh_surface(bool recreate = true);
void resize_swapchain(uint32_t width, uint32_t height, uint32_t nativeWidth, uint32_t nativeHeight, bool force = false);
// (Re)create the EFB color + depth render targets at the configured surface size. Must be
// called on the render worker (context current); gfx::initialize invokes it once the
// worker owns the context, resize_swapchain marshals it here on later resizes.
void create_frame_targets();
TextureWithSampler create_render_texture(uint32_t width, uint32_t height, bool multisampled);
const TextureWithSampler& present_source() noexcept;
void set_resampler(AuroraSampler sampler) noexcept;
AuroraSampler get_resampler() noexcept;
Viewport calculate_present_viewport(uint32_t surface_width, uint32_t surface_height, uint32_t content_width,
                                    uint32_t content_height) noexcept;
// The native present-destination size (window drawable in pixels), distinct from the possibly
// internalResolutionScale-downscaled EFB render size in surfaceConfiguration. Used to size the UI
// at native resolution regardless of the game's internal resolution scale.
uint32_t present_surface_width() noexcept;
uint32_t present_surface_height() noexcept;
// Present the finished frame's scene. Runs on the render worker (owns the GL context).
// Binds targetFbo, clears to black (letterbox bars) and blits the finished EFB into the
// centered content rect. Does NOT swap -- the caller composites the RmlUi overlay + imgui
// on top, then swaps (desktop) or publishes the slot (device). targetFbo is 0 (window
// default framebuffer) on desktop, or the acquired EFB slot's worker FBO on the device path.
void present_frame(uint32_t targetFbo = 0) noexcept;
// Composite a UI overlay texture over the just-presented scene, into the same centered content
// rect of targetFbo. overlay=true blends it premultiplied (UI over scene); overlay=false draws it
// opaque (the UI already seeded the scene as its backdrop). Runs on the render worker.
void composite_ui_overlay(const gl::Texture& texture, const gl::Sampler& sampler, bool overlay,
                          uint32_t targetFbo = 0) noexcept;
// Drop the state-cache shadow (present/composite/imgui all issued out-of-band GL) and swap the
// window's back buffer. Runs on the render worker, after all overlays are composited.
void present_swap() noexcept;
// Read the window's default-framebuffer back buffer and write it as a binary PPM (the
// desktop A/B screenshot tool). Must run on the render worker before the swap. Phase 5
// wires it to an F12 key / console command.
void screenshot(const char* path) noexcept;

} // namespace aurora::webgpu

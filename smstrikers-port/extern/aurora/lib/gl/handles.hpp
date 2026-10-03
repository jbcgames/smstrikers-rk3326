#pragma once

// Value-type GPU resource handles for the GL backend. These replace the wgpu
// object types that flow through aurora's gfx/gx interfaces:
//   wgpu::Buffer      -> gl::Buffer
//   wgpu::Texture     -> gl::Texture   (+ its two TextureViews collapse into it)
//   wgpu::Sampler     -> gl::Sampler
//   wgpu::RenderPipeline -> gl::Pipeline
//   cached wgpu::BindGroup -> gl::BindingSet
// All are trivially copyable POD handles: ownership lives in the create/destroy
// paths (buffers.cpp, textures.cpp, program_cache.cpp), not in the handle.

#include "../gfx/gfx_types.hpp"

#include <array>
#include <cstdint>

namespace aurora::gl {

using GLuint = uint32_t;

struct Buffer {
  GLuint id = 0;
  uint32_t target = 0; // GL_ARRAY_BUFFER / GL_UNIFORM_BUFFER / ...
  uint64_t size = 0;
  // Non-null when the buffer was created with a persistent-coherent mapping (GL_EXT_buffer_storage):
  // upload_buffer memcpys straight into it, no glBufferSubData / driver copy. Null == mutable buffer
  // updated with glBufferSubData. Owned by the create/destroy path; the handle is still trivially
  // copyable (aliases the same mapping, same as it aliases the same GL name).
  void* mapped = nullptr;
  explicit operator bool() const { return id != 0; }
  bool operator==(const Buffer& o) const { return id == o.id; }
};

struct Texture {
  GLuint id = 0;
  TextureFormat format = TextureFormat::Undefined;
  Extent3D size{};
  uint32_t mips = 1;
  bool renderable = false; // has an attachable format (RGBA8/depth); FBO color/depth target
  explicit operator bool() const { return id != 0; }
  bool operator==(const Texture& o) const { return id == o.id; }
};

struct Sampler {
  GLuint id = 0;
  explicit operator bool() const { return id != 0; }
  bool operator==(const Sampler& o) const { return id == o.id; }
};

// The compiled program plus its baked fixed-function state. BakedState is filled
// out in Phase 3 (the GX/RmlUi state translation); Phase 1 only needs the program
// handle to exist so pass/pipeline plumbing type-checks.
struct BakedState {
  // Blend
  bool blendEnabled = false;
  BlendOperation colorOp = BlendOperation::Add;
  BlendFactor colorSrc = BlendFactor::One;
  BlendFactor colorDst = BlendFactor::Zero;
  BlendOperation alphaOp = BlendOperation::Add;
  BlendFactor alphaSrc = BlendFactor::One;
  BlendFactor alphaDst = BlendFactor::Zero;
  ColorWriteMask writeMask = ColorWriteMask::All;
  // Depth
  bool depthTest = false;
  bool depthWrite = false;
  CompareFunction depthCompare = CompareFunction::Always;
  // Raster
  CullMode cull = CullMode::None;
  FrontFace frontFace = FrontFace::CCW;
  bool polygonOffset = false;
  float depthBiasSlope = 0.f;
  float depthBiasUnits = 0.f;
  // Topology
  PrimitiveTopology topology = PrimitiveTopology::TriangleList;
  bool primitiveRestart = false;
  // Stencil (RmlUi only)
  bool stencilTest = false;
  CompareFunction stencilCompare = CompareFunction::Always;
  uint32_t stencilReadMask = 0xFF;
  uint32_t stencilWriteMask = 0xFF;
  // stencil ops stored as raw GL enums, resolved in Phase 5:
  uint32_t stencilFail = 0;
  uint32_t stencilDepthFail = 0;
  uint32_t stencilPass = 0;
};

// Sentinel `vertexLayout` value marking an RmlUi geometry pipeline (interleaved
// Rml::Vertex: pos Float32x2, tex_coord Float32x2, colour Unorm8x4). GX layouts are
// present-attr bitmasks over bits 0..20, so the top bit never collides; the draw path
// (pass.cpp) binds the fixed RmlUi VAO instead of decoding a GX attr mask for it.
inline constexpr uint32_t kRmlGeometryVertexLayout = 0x80000000u;

struct Pipeline {
  GLuint program = 0;
  BakedState state{};
  uint32_t vertexLayout = 0; // GX present-attr bitmask, kRmlGeometryVertexLayout, or 0 (attribute-less)
  explicit operator bool() const { return program != 0; }
};

inline constexpr uint32_t kMaxTextureBindings = 8;
inline constexpr uint32_t kMaxBufferBindings = 2;

// The cached, replay-ready form of a wgpu BindGroup. A GX texture group carries
// up to 8 texture+sampler pairs; a uniform group carries one buffer bound with a
// dynamic offset at draw time. RmlUi groups mix both.
struct BindingSet {
  struct TextureBinding {
    GLuint texture = 0;
    GLuint sampler = 0;
  };
  struct BufferBinding {
    GLuint buffer = 0;
    uint32_t binding = 0; // GL uniform-block binding point
    uint32_t offset = 0;  // static base offset (dynamic offset added at draw)
    uint32_t size = 0;
    bool dynamic = false;
  };
  std::array<TextureBinding, kMaxTextureBindings> textures{};
  std::array<BufferBinding, kMaxBufferBindings> buffers{};
  uint8_t bufferCount = 0;
};

} // namespace aurora::gl

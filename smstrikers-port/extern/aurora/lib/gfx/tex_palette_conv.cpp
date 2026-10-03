#include "tex_palette_conv.hpp"

#include "../internal.hpp"
#include "../webgpu/gpu.hpp"
#include "common.hpp"
#include "texture.hpp"

#include "../gl/fbo_cache.hpp"
#include "../gl/gl_core.hpp"
#include "../gl/program.hpp"
#include "../gl/state.hpp"
#include "../gl/textures.hpp"
#include "render_worker.hpp"

#include <string>

namespace aurora::gfx::tex_palette_conv {
static Module Log("aurora::gfx::tex_palette_conv");

// Shared fullscreen-triangle vertex shader (no crop transform: paletted textures resolve
// 1:1). Matches the WGSL vs_main.
constexpr char kVertexSource[] = R"(#version 300 es
out vec2 v_uv;
void main() {
  const vec2 positions[3] = vec2[3](vec2(-1.0, 1.0), vec2(-1.0, -3.0), vec2(3.0, 1.0));
  const vec2 uvs[3] = vec2[3](vec2(0.0, 0.0), vec2(0.0, 2.0), vec2(2.0, 0.0));
  gl_Position = vec4(positions[gl_VertexID], 0.0, 1.0);
  v_uv = uvs[gl_VertexID];
}
)";

// Direct: R16I index texture (isampler2D) -> TLUT lookup. texelFetch for both (integer
// index, no filtering) -- the WGSL textureLoad maps 1:1.
constexpr char kFragDirect[] = R"(#version 300 es
precision highp float;
precision highp int;
uniform highp isampler2D src;
uniform sampler2D tlut;
in vec2 v_uv;
out vec4 out_color;
void main() {
  ivec2 coord = ivec2(floor(v_uv * vec2(textureSize(src, 0))));
  int idx = texelFetch(src, coord, 0).r;
  out_color = texelFetch(tlut, ivec2(idx, 0), 0);
}
)";

// FromFloat8: R8 texture -> 8-bit index -> TLUT.
// The index is stored as an exact k/255 unorm; recover it by ROUNDING, not truncating.
// In fp32, k/255 * 255.0 evaluates to k - epsilon (e.g. 40/255 -> 39.99999), so int()
// truncates every exact index down to k-1 -- a one-entry palette shift that turns the
// Ordon minimap's ground tint black (empirically confirmed vs the TLUT in a capture:
// index 40 = dark green, index 39 = a black run). The WGSL reference used i32(r*255.0);
// this is a deliberate divergence (rounding is the GX-correct reconstruction).
constexpr char kFragFromFloat8[] = R"(#version 300 es
precision highp float;
precision highp int;
uniform sampler2D src;
uniform sampler2D tlut;
in vec2 v_uv;
out vec4 out_color;
void main() {
  ivec2 coord = ivec2(floor(v_uv * vec2(textureSize(src, 0))));
  float r = texelFetch(src, coord, 0).r;
  out_color = texelFetch(tlut, ivec2(int(r * 255.0 + 0.5), 0), 0);
}
)";

// FromFloat4: R8 texture -> 4-bit index -> TLUT. Round for the same reason as FromFloat8.
constexpr char kFragFromFloat4[] = R"(#version 300 es
precision highp float;
precision highp int;
uniform sampler2D src;
uniform sampler2D tlut;
in vec2 v_uv;
out vec4 out_color;
void main() {
  ivec2 coord = ivec2(floor(v_uv * vec2(textureSize(src, 0))));
  float r = texelFetch(src, coord, 0).r;
  out_color = texelFetch(tlut, ivec2(int(r * 15.0 + 0.5), 0), 0);
}
)";

static gl::Pipeline g_directPipeline;
static gl::Pipeline g_fromFloat8Pipeline;
static gl::Pipeline g_fromFloat4Pipeline;
static gl::Sampler g_sampler;

static gl::BakedState palette_state() {
  gl::BakedState state{};
  state.blendEnabled = false;
  state.depthTest = false;
  state.depthWrite = false;
  state.cull = gl::CullMode::None;
  state.writeMask = gl::ColorWriteMask::All;
  state.topology = gl::PrimitiveTopology::TriangleList;
  return state;
}

// Compile a palette program (worker context current) and bind its samplers: `src` index
// texture -> unit 0, `tlut` -> unit 1.
static gl::Pipeline create_pipeline(const char* fragSource, const char* label) {
  const gl::GLuint program = gl::compile_program(kVertexSource, fragSource, label);
  if (program != 0) {
    gl::gl.UseProgram(program);
    const gl::GLint srcLoc = gl::gl.GetUniformLocation(program, "src");
    if (srcLoc >= 0) {
      gl::gl.Uniform1i(srcLoc, 0);
    }
    const gl::GLint tlutLoc = gl::gl.GetUniformLocation(program, "tlut");
    if (tlutLoc >= 0) {
      gl::gl.Uniform1i(tlutLoc, 1);
    }
    gl::gl.UseProgram(0);
    gl::gl.Flush();
  }
  return gl::Pipeline{.program = program, .state = palette_state(), .vertexLayout = 0};
}

static const gl::Pipeline& pipeline_for_variant(Variant variant) {
  switch (variant) {
  case Variant::Direct:
    return g_directPipeline;
  case Variant::FromFloat8:
    return g_fromFloat8Pipeline;
  case Variant::FromFloat4:
    return g_fromFloat4Pipeline;
  }
  FATAL("invalid palette conv variant {}", static_cast<int>(variant));
}

void initialize() {
  render_worker::enqueue_work([] {
    g_directPipeline = create_pipeline(kFragDirect, "TexPaletteConv Direct");
    g_fromFloat8Pipeline = create_pipeline(kFragFromFloat8, "TexPaletteConv FromFloat8");
    g_fromFloat4Pipeline = create_pipeline(kFragFromFloat4, "TexPaletteConv FromFloat4");
    // Nearest sampler for the index/TLUT texelFetches (integer index texture cannot be
    // linear-filtered; ClampToEdge + Nearest defaults are correct).
    gl::SamplerDescriptor desc{};
    g_sampler = gl::create_sampler(desc, webgpu::g_graphicsConfig.textureAnisotropy > 0);
  });
  render_worker::synchronize();
}

void shutdown() {
  g_directPipeline = {};
  g_fromFloat8Pipeline = {};
  g_fromFloat4Pipeline = {};
  g_sampler = {};
}

void run(const ConvRequest& req) {
  const auto& pipeline = pipeline_for_variant(req.variant);
  if (pipeline.program == 0 || !req.dst || req.dst->attachmentTextureView.id == 0 || !req.src || !req.tlut) {
    return;
  }
  const gl::GLuint fbo = gl::get_framebuffer(req.dst->attachmentTextureView);
  gl::gl.BindFramebuffer(gl::GL_FRAMEBUFFER, fbo);

  gl::gl.Disable(gl::GL_SCISSOR_TEST);
  gl::gl.ColorMask(gl::GL_TRUE, gl::GL_TRUE, gl::GL_TRUE, gl::GL_TRUE);
  const gl::GLfloat clearColor[4]{0.f, 0.f, 0.f, 0.f};
  gl::gl.ClearBufferfv(gl::GL_COLOR, 0, clearColor);
  gl::reset_state_cache();

  gl::set_viewport_gl(0, 0, static_cast<gl::GLsizei>(req.dst->size.width),
                      static_cast<gl::GLsizei>(req.dst->size.height), 0.f, 1.f);
  gl::use_program(pipeline.program);
  gl::apply_baked_state(pipeline.state);
  gl::bind_texture_unit(0, req.src->sampleTextureView.id, g_sampler.id);
  gl::bind_texture_unit(1, req.tlut->sampleTextureView.id, g_sampler.id);
  gl::bind_vertex_array(0);
  gl::gl.DrawArrays(gl::GL_TRIANGLES, 0, 3);
  gl::gl.Enable(gl::GL_SCISSOR_TEST);
}

} // namespace aurora::gfx::tex_palette_conv

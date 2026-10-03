#include "tex_copy_conv.hpp"

#include "../internal.hpp"
#include "../gx/gx.hpp"
#include "../gx/gx_fmt.hpp"
#include "../webgpu/gpu.hpp"
#include "common.hpp"
#include "texture.hpp"
#include "texture_convert.hpp"

#include "../gl/fbo_cache.hpp"
#include "../gl/gl_core.hpp"
#include "../gl/program.hpp"
#include "../gl/state.hpp"
#include "../gl/textures.hpp"
#include "render_worker.hpp"

#include <absl/container/flat_hash_map.h>
#include <array>

namespace aurora::gfx::tex_copy_conv {
static Module Log("aurora::gfx::tex_copy_conv");

// Depth EFB copies (Z16) and the R32Float depth snapshot never run on the live TP path
// (depth_peek is stubbed, GXPeekZ tolerates "no snapshot"), and they were compat-gated
// OFF on device under Dawn. The GLES query_caps() now reports core features true on
// desktop too, so we can no longer piggyback that flag -- gate depth copies on this
// dedicated constexpr instead. Kept OFF for device parity ("port later or never"); a
// depth resolve that reaches run() Log.fatals on the missing pipeline (assert-if-reached).
// The Z16 / depth-snapshot conversions were dropped here (recoverable from the Dawn
// reference at 1ed7d00 if a future target needs them).
constexpr bool kEnableDepthCopies = false;

// GX uniform block binding point for the UV-transform (crop) uniform. Distinct from the
// GX draw path's block (also 0, but a different program) -- binding points are global GL
// state, re-bound per conversion draw, so reuse is fine.
constexpr uint32_t kUvBlockBinding = 0;

// Shared fullscreen-triangle vertex shader: emit the big triangle from gl_VertexID and
// map its [0,1] UV through the crop transform (offset + scale). Matches the WGSL vs_main.
constexpr char kVertexSource[] = R"(#version 300 es
layout(std140) uniform UVTransform {
  vec2 uv_offset;
  vec2 uv_scale;
};
out vec2 v_uv;
void main() {
  const vec2 positions[3] = vec2[3](vec2(-1.0, 1.0), vec2(-1.0, -3.0), vec2(3.0, 1.0));
  const vec2 uvs[3] = vec2[3](vec2(0.0, 0.0), vec2(0.0, 2.0), vec2(2.0, 0.0));
  gl_Position = vec4(positions[gl_VertexID], 0.0, 1.0);
  v_uv = uvs[gl_VertexID] * uv_scale + uv_offset;
}
)";

// Shared fragment preamble: highp (TEV-style intensity needs fp32), the src sampler on
// unit 0, and the ITU-R BT.601 luma + 4-bit quantize helpers used by the intensity/CTF
// conversions. Per-format fragment bodies are appended.
constexpr std::string_view kFragPreamble = R"(#version 300 es
precision highp float;
uniform sampler2D src;
in vec2 v_uv;
out vec4 out_color;
float intensity(vec3 rgb) { return dot(rgb, vec3(0.257, 0.504, 0.098)) + 16.0 / 255.0; }
float quantize4(float v) { return floor(v * 16.0) / 15.0; }
)";

// Passthrough blit (scaling copies)
constexpr std::string_view FragPassthrough = R"(
void main() { out_color = texture(src, v_uv); }
)";
// GX_TF_I4: 4-bit intensity, quantized
constexpr std::string_view FragI4 = R"(
void main() { float i = quantize4(intensity(texture(src, v_uv).rgb)); out_color = vec4(i, i, i, i); }
)";
// GX_TF_I8: 8-bit intensity
constexpr std::string_view FragI8 = R"(
void main() { float i = intensity(texture(src, v_uv).rgb); out_color = vec4(i, i, i, i); }
)";
// GX_TF_IA4: 4-bit intensity + 4-bit alpha
constexpr std::string_view FragIA4 = R"(
void main() {
  vec4 c = texture(src, v_uv);
  float i = quantize4(intensity(c.rgb));
  out_color = vec4(i, i, i, quantize4(c.a));
}
)";
// GX_TF_IA8: 8-bit intensity + 8-bit alpha
constexpr std::string_view FragIA8 = R"(
void main() { vec4 c = texture(src, v_uv); float i = intensity(c.rgb); out_color = vec4(i, i, i, c.a); }
)";
// GX_TF_RGB565: blit, alpha forced to 1
constexpr std::string_view FragRGB565 = R"(
void main() { out_color = vec4(texture(src, v_uv).rgb, 1.0); }
)";
// GX_CTF_R4
constexpr std::string_view FragR4 = R"(
void main() { float r = quantize4(texture(src, v_uv).r); out_color = vec4(r, r, r, r); }
)";
// GX_CTF_RA4
constexpr std::string_view FragRA4 = R"(
void main() { vec4 c = texture(src, v_uv); float r = quantize4(c.r); out_color = vec4(r, r, r, quantize4(c.a)); }
)";
// GX_CTF_RA8
constexpr std::string_view FragRA8 = R"(
void main() { vec4 c = texture(src, v_uv); out_color = vec4(c.r, c.r, c.r, c.a); }
)";
// GX_CTF_A8
constexpr std::string_view FragA8 = R"(
void main() { float a = texture(src, v_uv).a; out_color = vec4(a, a, a, a); }
)";
// GX_CTF_R8
constexpr std::string_view FragR8 = R"(
void main() { float r = texture(src, v_uv).r; out_color = vec4(r, r, r, r); }
)";
// GX_CTF_G8
constexpr std::string_view FragG8 = R"(
void main() { float g = texture(src, v_uv).g; out_color = vec4(g, g, g, g); }
)";
// GX_CTF_B8
constexpr std::string_view FragB8 = R"(
void main() { float b = texture(src, v_uv).b; out_color = vec4(b, b, b, b); }
)";
// GX_CTF_RG8
constexpr std::string_view FragRG8 = R"(
void main() { vec4 c = texture(src, v_uv); out_color = vec4(c.r, c.r, c.r, c.g); }
)";
// GX_CTF_GB8
constexpr std::string_view FragGB8 = R"(
void main() { vec4 c = texture(src, v_uv); out_color = vec4(c.g, c.g, c.g, c.b); }
)";

struct ConvSpec {
  GXTexFmt fmt;
  std::string_view fragBody;
  const char* label;
};

constexpr std::array ConvSpecs{
    ConvSpec{GX_TF_I4, FragI4, "TexCopyConv I4"},        ConvSpec{GX_TF_I8, FragI8, "TexCopyConv I8"},
    ConvSpec{GX_TF_IA4, FragIA4, "TexCopyConv IA4"},     ConvSpec{GX_TF_IA8, FragIA8, "TexCopyConv IA8"},
    ConvSpec{GX_TF_RGB565, FragRGB565, "TexCopyConv RGB565"},
    ConvSpec{GX_CTF_R4, FragR4, "TexCopyConv R4"},       ConvSpec{GX_CTF_RA4, FragRA4, "TexCopyConv RA4"},
    ConvSpec{GX_CTF_RA8, FragRA8, "TexCopyConv RA8"},    ConvSpec{GX_CTF_A8, FragA8, "TexCopyConv A8"},
    ConvSpec{GX_CTF_R8, FragR8, "TexCopyConv R8"},       ConvSpec{GX_CTF_G8, FragG8, "TexCopyConv G8"},
    ConvSpec{GX_CTF_B8, FragB8, "TexCopyConv B8"},       ConvSpec{GX_CTF_RG8, FragRG8, "TexCopyConv RG8"},
    ConvSpec{GX_CTF_GB8, FragGB8, "TexCopyConv GB8"},
};

static gl::Sampler g_nearestSampler;
static gl::Sampler g_linearSampler;
static absl::flat_hash_map<GXTexFmt, gl::Pipeline> g_pipelines;
static gl::Pipeline g_blitPipeline;

// Bake the shared conversion fixed-function state: single RGBA8 target, full color write,
// no blend/depth/cull, triangle list. (All conversions share this; only the program and
// the src filter differ.)
static gl::BakedState conv_state() {
  gl::BakedState state{};
  state.blendEnabled = false;
  state.depthTest = false;
  state.depthWrite = false;
  state.cull = gl::CullMode::None;
  state.writeMask = gl::ColorWriteMask::All;
  state.topology = gl::PrimitiveTopology::TriangleList;
  return state;
}

// Compile a conversion program (worker context current) and wire its bindings: the
// UVTransform block -> binding kUvBlockBinding, the `src` sampler -> texture unit 0.
static gl::Pipeline create_pipeline(std::string_view fragBody, const char* label) {
  const std::string frag = std::string{kFragPreamble} + std::string{fragBody};
  const gl::GLuint program = gl::compile_program(kVertexSource, frag.c_str(), label);
  if (program != 0) {
    const gl::GLuint blockIndex = gl::gl.GetUniformBlockIndex(program, "UVTransform");
    if (blockIndex != 0xFFFFFFFFu) {
      gl::gl.UniformBlockBinding(program, blockIndex, kUvBlockBinding);
    }
    gl::gl.UseProgram(program);
    const gl::GLint srcLoc = gl::gl.GetUniformLocation(program, "src");
    if (srcLoc >= 0) {
      gl::gl.Uniform1i(srcLoc, 0);
    }
    gl::gl.UseProgram(0);
    gl::gl.Flush();
  }
  return gl::Pipeline{.program = program, .state = conv_state(), .vertexLayout = 0};
}

bool needs_conversion(const GXTexFmt fmt) { return g_pipelines.contains(fmt); }

void initialize() {
  // Program compile + sampler creation need the render context current -- marshal to the
  // worker (initialize() itself runs on the main thread, after the worker sync).
  render_worker::enqueue_work([] {
    g_blitPipeline = create_pipeline(FragPassthrough, "TexCopyConv Blit");
    g_pipelines.clear();
    for (const auto& spec : ConvSpecs) {
      if (to_gl(spec.fmt) != gl::TextureFormat::RGBA8Unorm) {
        Log.fatal("Unexpected output format for {}", static_cast<int>(spec.fmt));
      }
      g_pipelines[spec.fmt] = create_pipeline(spec.fragBody, spec.label);
    }
    // Depth conversions (Z16) are gated off (kEnableDepthCopies); none created.

    const bool aniso = webgpu::g_graphicsConfig.textureAnisotropy > 0;
    gl::SamplerDescriptor nearest{}; // ClampToEdge + Nearest defaults
    g_nearestSampler = gl::create_sampler(nearest, aniso);
    gl::SamplerDescriptor linear{};
    linear.magFilter = gl::FilterMode::Linear;
    linear.minFilter = gl::FilterMode::Linear;
    g_linearSampler = gl::create_sampler(linear, aniso);
  });
  render_worker::synchronize();
}

void shutdown() {
  g_pipelines.clear();
  g_blitPipeline = {};
  g_nearestSampler = {};
  g_linearSampler = {};
}

// Render one fullscreen-triangle conversion draw into req.dst (worker, context current).
// Mirrors the Dawn execute(): clear the destination to 0, bind src + UV uniform, Draw(3).
static void execute(const ConvRequest& req, const gl::Pipeline& pipeline) {
  if (pipeline.program == 0) {
    return;
  }
  if (gx::is_depth_format(req.fmt) && !kEnableDepthCopies) {
    return; // depth copies gated off (device parity)
  }
  if (!req.dst || req.dst->attachmentTextureView.id == 0) {
    return;
  }
  const gl::GLuint fbo = gl::get_framebuffer(req.dst->attachmentTextureView);
  gl::gl.BindFramebuffer(gl::GL_FRAMEBUFFER, fbo);

  // loadOp = Clear(0,0,0,0): scissor-off, full color mask (S7). The raw clear + FBO switch
  // desyncs the state-cache shadow, so reset it before the draw re-applies baked state.
  gl::gl.Disable(gl::GL_SCISSOR_TEST);
  gl::gl.ColorMask(gl::GL_TRUE, gl::GL_TRUE, gl::GL_TRUE, gl::GL_TRUE);
  const gl::GLfloat clearColor[4]{0.f, 0.f, 0.f, 0.f};
  gl::gl.ClearBufferfv(gl::GL_COLOR, 0, clearColor);
  gl::reset_state_cache();

  gl::set_viewport_gl(0, 0, static_cast<gl::GLsizei>(req.dst->size.width),
                      static_cast<gl::GLsizei>(req.dst->size.height), 0.f, 1.f);
  gl::use_program(pipeline.program);
  gl::apply_baked_state(pipeline.state);
  const gl::GLuint sampler = req.sampleFilter == SampleFilter::Linear ? g_linearSampler.id : g_nearestSampler.id;
  gl::bind_texture_unit(0, req.srcView.id, sampler);
  gl::bind_uniform_range(kUvBlockBinding, g_uniformBuffer.id, req.uniformRange.offset, req.uniformRange.size);
  gl::bind_vertex_array(0);
  gl::gl.DrawArrays(gl::GL_TRIANGLES, 0, 3);
  gl::gl.Enable(gl::GL_SCISSOR_TEST);
}

void run(const ConvRequest& req) {
  const auto it = g_pipelines.find(req.fmt);
  if (it == g_pipelines.end()) {
    Log.fatal("No copy conversion pipeline for format {}", static_cast<int>(req.fmt));
  }
  execute(req, it->second);
}

void blit(const ConvRequest& req) { execute(req, g_blitPipeline); }

// Depth snapshot (R32Float) is gated off; GXPeekZ tolerates its absence.
bool snapshot_depth_supported() noexcept { return kEnableDepthCopies; }

void snapshot_depth(const gl::Texture& srcDepth, uint32_t msaaSamples, const gl::Texture& dst) {
  (void)srcDepth;
  (void)msaaSamples;
  (void)dst;
}

} // namespace aurora::gfx::tex_copy_conv

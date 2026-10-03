#include "clear.hpp"

#include "../gl/program.hpp"
#include "../webgpu/gpu.hpp"
#include "tracy/Tracy.hpp"

namespace aurora::gfx::clear {

using webgpu::g_graphicsConfig;

namespace {
gl::ColorWriteMask clear_write_mask(bool clearColor, bool clearAlpha) {
  auto writeMask = gl::ColorWriteMask::None;
  if (clearColor) {
    writeMask = writeMask | gl::ColorWriteMask::Red | gl::ColorWriteMask::Green | gl::ColorWriteMask::Blue;
  }
  if (clearAlpha) {
    writeMask = writeMask | gl::ColorWriteMask::Alpha;
  }
  return writeMask;
}

// The clear "pipeline" is a fullscreen triangle whose fragment always writes 1.0;
// the blend state (src = Constant, dst = Zero) turns that into "write the blend
// constant" and the write mask restricts it to the requested channels. Every clear
// config shares this one program -- only the baked fixed-function state differs --
// so it is compiled once on the render worker (S2/S8: attribute-less, no z remap).
gl::GLuint s_clearProgram = 0;

constexpr char kVertexSource[] = R"(#version 300 es
void main() {
  const vec2 positions[3] = vec2[3](vec2(-1.0, 1.0), vec2(-1.0, -3.0), vec2(3.0, 1.0));
  gl_Position = vec4(positions[gl_VertexID], 0.0, 1.0);
}
)";

constexpr char kFragmentSource[] = R"(#version 300 es
precision highp float;
out vec4 fragColor;
void main() { fragColor = vec4(1.0); }
)";
} // namespace

// Called on the render worker during gfx::initialize (context current). Idempotent.
void init_program() {
  if (s_clearProgram == 0) {
    s_clearProgram = gl::compile_program(kVertexSource, kFragmentSource, "EFB Clear");
  }
}

gl::Pipeline create_pipeline(const PipelineConfig& config) {
  ZoneScoped;
  gl::BakedState state{};
  // color = blendConstant * 1 + dst * 0 => write the constant, masked by writeMask
  state.blendEnabled = true;
  state.colorOp = gl::BlendOperation::Add;
  state.colorSrc = gl::BlendFactor::Constant;
  state.colorDst = gl::BlendFactor::Zero;
  state.alphaOp = gl::BlendOperation::Add;
  state.alphaSrc = gl::BlendFactor::Constant;
  state.alphaDst = gl::BlendFactor::Zero;
  state.writeMask = clear_write_mask(config.clearColor, config.clearAlpha);
  state.depthTest = true;
  state.depthWrite = config.clearDepth;
  state.depthCompare = gl::CompareFunction::Always;
  state.cull = gl::CullMode::None;
  state.topology = gl::PrimitiveTopology::TriangleList;
  // create_pipeline can run off the worker (pipeline cache / recording thread), so it
  // issues no GL: it references the program init_program() compiled on the worker.
  return gl::Pipeline{
      .program = s_clearProgram,
      .state = state,
      .vertexLayout = 0,
  };
}

void render(const DrawData& data, gl::PassEncoder& pass, const gl::Extent3D& targetSize) {
  if (!bind_pipeline(data.pipeline, pass)) {
    return;
  }

  pass.SetBlendConstant(&data.color);
  pass.SetViewport(0.f, 0.f, static_cast<float>(targetSize.width), static_cast<float>(targetSize.height), data.depth,
                   data.depth);
  pass.SetScissorRect(0, 0, targetSize.width, targetSize.height);
  pass.Draw(3);
}
} // namespace aurora::gfx::clear

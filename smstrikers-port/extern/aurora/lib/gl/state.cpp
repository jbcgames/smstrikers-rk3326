#include "state.hpp"

#include <array>
#include <cstring>

namespace aurora::gl {
namespace {

// ---- gfx vocabulary -> GL enum mappers ----
GLenum blend_factor_gl(BlendFactor f) {
  switch (f) {
  case BlendFactor::Zero:
    return GL_ZERO;
  case BlendFactor::One:
    return GL_ONE;
  case BlendFactor::Src:
    return GL_SRC_COLOR;
  case BlendFactor::OneMinusSrc:
    return GL_ONE_MINUS_SRC_COLOR;
  case BlendFactor::SrcAlpha:
    return GL_SRC_ALPHA;
  case BlendFactor::OneMinusSrcAlpha:
    return GL_ONE_MINUS_SRC_ALPHA;
  case BlendFactor::Dst:
    return GL_DST_COLOR;
  case BlendFactor::OneMinusDst:
    return GL_ONE_MINUS_DST_COLOR;
  case BlendFactor::DstAlpha:
    return GL_DST_ALPHA;
  case BlendFactor::OneMinusDstAlpha:
    return GL_ONE_MINUS_DST_ALPHA;
  case BlendFactor::SrcAlphaSaturated:
    return GL_SRC_ALPHA_SATURATE;
  case BlendFactor::Constant:
    return GL_CONSTANT_COLOR;
  case BlendFactor::OneMinusConstant:
    return GL_ONE_MINUS_CONSTANT_COLOR;
  }
  return GL_ONE;
}

GLenum blend_op_gl(BlendOperation op) {
  switch (op) {
  case BlendOperation::Add:
    return GL_FUNC_ADD;
  case BlendOperation::Subtract:
    return GL_FUNC_SUBTRACT;
  case BlendOperation::ReverseSubtract:
    return GL_FUNC_REVERSE_SUBTRACT;
  case BlendOperation::Min:
    return GL_MIN;
  case BlendOperation::Max:
    return GL_MAX;
  }
  return GL_FUNC_ADD;
}

GLenum compare_gl(CompareFunction f) {
  switch (f) {
  case CompareFunction::Never:
    return GL_NEVER;
  case CompareFunction::Less:
    return GL_LESS;
  case CompareFunction::Equal:
    return GL_EQUAL;
  case CompareFunction::LessEqual:
    return GL_LEQUAL;
  case CompareFunction::Greater:
    return GL_GREATER;
  case CompareFunction::NotEqual:
    return GL_NOTEQUAL;
  case CompareFunction::GreaterEqual:
    return GL_GEQUAL;
  case CompareFunction::Always:
  case CompareFunction::Undefined:
    return GL_ALWAYS;
  }
  return GL_ALWAYS;
}

// S1a: front-face winding. GX/WebGPU declares the front face as CW. The port originally
// remapped that to GL_CCW to compensate for GL-native rasterization (no projection
// Y-flip) — the standard "GL flips window-space winding vs WebGPU" argument. Empirically
// that produced INVERTED back-face culling on screen: camera-facing surfaces were culled
// and away-facing ones kept — a single-sided ground plane (its one face points up at the
// camera) vanished to black, Link showed his face instead of the back of his head, and
// walls showed their backsides. Mapping GX winding straight through (CW->GL_CW) makes the
// front face the front face again.
// NOTE: our hand-rolled backend does NOT do the internal projection Y-flip that Dawn's own
// GL backend applies, so the winding parity vs the Dawn path differs by one reflection —
// which is why the naive "map CW->CCW" S1a rule (written against Dawn's behavior) is wrong
// here. The image is already upright, so this only touches facing, not orientation.
GLenum front_face_gl(FrontFace f) { return f == FrontFace::CW ? GL_CW : GL_CCW; }

constexpr GLuint kInvalidId = 0xFFFFFFFFu;
constexpr int kUnknown = -1;

struct TextureUnit {
  GLuint texture = kInvalidId;
  GLuint sampler = kInvalidId;
};
struct UboBinding {
  GLuint buffer = kInvalidId;
  GLuint offset = kInvalidId;
  GLuint size = kInvalidId;
};

struct Shadow {
  GLuint program;
  GLuint vao;
  GLuint activeUnit;
  // Blend
  int blendEnabled;
  GLenum bColorSrc, bColorDst, bAlphaSrc, bAlphaDst;
  GLenum bColorOp, bAlphaOp;
  int colorMaskR, colorMaskG, colorMaskB, colorMaskA;
  float blendColorR, blendColorG, blendColorB, blendColorA;
  // Depth
  int depthTest, depthWrite;
  GLenum depthFunc;
  // Raster
  int cullEnabled;
  GLenum cullFace, frontFace;
  int polygonOffset;
  float poFactor, poUnits;
  int primitiveRestart;
  // Stencil
  int stencilTest;
  GLenum stencilFunc;
  GLint stencilRef;
  GLuint stencilReadMask, stencilWriteMask;
  GLenum sFail, sDepthFail, sPass;
  // Viewport / scissor
  GLint vpX, vpY;
  GLsizei vpW, vpH;
  float depthNear, depthFar;
  GLint scX, scY;
  GLsizei scW, scH;

  std::array<TextureUnit, kMaxTextureBindings> textures;
  std::array<UboBinding, kMaxBufferBindings> ubos;
};

Shadow s;

void invalidate() {
  // Sentinels that no legitimate value equals, so the next apply re-issues.
  std::memset(&s, 0xFF, sizeof(s));
  // 0xFF-fill leaves tri-state ints as kUnknown (-1) and ids as kInvalidId; the
  // float fields become NaN-ish, which compares unequal to any real value.
}

void set_cap(GLenum cap, int& shadowFlag, bool enable) {
  const int want = enable ? 1 : 0;
  if (shadowFlag != want) {
    if (enable) {
      gl.Enable(cap);
    } else {
      gl.Disable(cap);
    }
    shadowFlag = want;
  }
}

void apply_stencil_func() {
  gl.StencilFuncSeparate(GL_FRONT, s.stencilFunc, s.stencilRef, s.stencilReadMask);
  gl.StencilFuncSeparate(GL_BACK, s.stencilFunc, s.stencilRef, s.stencilReadMask);
}

} // namespace

void reset_state_cache() noexcept { invalidate(); }

void invalidate_texture_bindings() noexcept {
  s.activeUnit = kInvalidId;
  for (auto& u : s.textures) {
    u.texture = kInvalidId;
    u.sampler = kInvalidId;
  }
}

void note_texture_destroyed(GLuint texture) noexcept {
  if (texture == 0) {
    return;
  }
  for (auto& u : s.textures) {
    if (u.texture == texture) {
      u.texture = 0; // the delete reverted the real binding to 0
    }
  }
}

void note_sampler_destroyed(GLuint sampler) noexcept {
  if (sampler == 0) {
    return;
  }
  for (auto& u : s.textures) {
    if (u.sampler == sampler) {
      u.sampler = 0;
    }
  }
}

void note_buffer_destroyed(GLuint buffer) noexcept {
  if (buffer == 0) {
    return;
  }
  for (auto& b : s.ubos) {
    if (b.buffer == buffer) {
      b = UboBinding{}; // sentinel-filled: force a full BindBufferRange re-issue
    }
  }
}

void apply_baked_state(const BakedState& state) {
  // Blend
  set_cap(GL_BLEND, s.blendEnabled, state.blendEnabled);
  if (state.blendEnabled) {
    const GLenum cs = blend_factor_gl(state.colorSrc);
    const GLenum cd = blend_factor_gl(state.colorDst);
    const GLenum as = blend_factor_gl(state.alphaSrc);
    const GLenum ad = blend_factor_gl(state.alphaDst);
    if (cs != s.bColorSrc || cd != s.bColorDst || as != s.bAlphaSrc || ad != s.bAlphaDst) {
      gl.BlendFuncSeparate(cs, cd, as, ad);
      s.bColorSrc = cs, s.bColorDst = cd, s.bAlphaSrc = as, s.bAlphaDst = ad;
    }
    const GLenum co = blend_op_gl(state.colorOp);
    const GLenum ao = blend_op_gl(state.alphaOp);
    if (co != s.bColorOp || ao != s.bAlphaOp) {
      gl.BlendEquationSeparate(co, ao);
      s.bColorOp = co, s.bAlphaOp = ao;
    }
  }

  // Color write mask
  const int mr = any(state.writeMask & ColorWriteMask::Red) ? 1 : 0;
  const int mg = any(state.writeMask & ColorWriteMask::Green) ? 1 : 0;
  const int mb = any(state.writeMask & ColorWriteMask::Blue) ? 1 : 0;
  const int ma = any(state.writeMask & ColorWriteMask::Alpha) ? 1 : 0;
  if (mr != s.colorMaskR || mg != s.colorMaskG || mb != s.colorMaskB || ma != s.colorMaskA) {
    gl.ColorMask(static_cast<GLboolean>(mr), static_cast<GLboolean>(mg), static_cast<GLboolean>(mb),
                 static_cast<GLboolean>(ma));
    s.colorMaskR = mr, s.colorMaskG = mg, s.colorMaskB = mb, s.colorMaskA = ma;
  }

  // Depth
  set_cap(GL_DEPTH_TEST, s.depthTest, state.depthTest);
  const int dw = state.depthWrite ? 1 : 0;
  if (dw != s.depthWrite) {
    gl.DepthMask(static_cast<GLboolean>(dw));
    s.depthWrite = dw;
  }
  if (state.depthTest) {
    const GLenum df = compare_gl(state.depthCompare);
    if (df != s.depthFunc) {
      gl.DepthFunc(df);
      s.depthFunc = df;
    }
  }

  // Raster: cull + winding
  set_cap(GL_CULL_FACE, s.cullEnabled, state.cull != CullMode::None);
  if (state.cull != CullMode::None) {
    const GLenum cf = state.cull == CullMode::Front ? GL_FRONT : GL_BACK;
    if (cf != s.cullFace) {
      gl.CullFace(cf);
      s.cullFace = cf;
    }
  }
  const GLenum ff = front_face_gl(state.frontFace);
  if (ff != s.frontFace) {
    gl.FrontFace(ff);
    s.frontFace = ff;
  }

  // Polygon offset
  set_cap(GL_POLYGON_OFFSET_FILL, s.polygonOffset, state.polygonOffset);
  if (state.polygonOffset && (state.depthBiasSlope != s.poFactor || state.depthBiasUnits != s.poUnits)) {
    gl.PolygonOffset(state.depthBiasSlope, state.depthBiasUnits);
    s.poFactor = state.depthBiasSlope, s.poUnits = state.depthBiasUnits;
  }

  // Primitive restart (S6): only strips enable it; the pipeline's flag decides.
  set_cap(GL_PRIMITIVE_RESTART_FIXED_INDEX, s.primitiveRestart, state.primitiveRestart);

  // Stencil (RmlUi)
  set_cap(GL_STENCIL_TEST, s.stencilTest, state.stencilTest);
  if (state.stencilTest) {
    const GLenum sc = compare_gl(state.stencilCompare);
    if (sc != s.stencilFunc || state.stencilReadMask != s.stencilReadMask) {
      s.stencilFunc = sc;
      s.stencilReadMask = state.stencilReadMask;
      apply_stencil_func();
    }
    if (state.stencilWriteMask != s.stencilWriteMask) {
      gl.StencilMaskSeparate(GL_FRONT, state.stencilWriteMask);
      gl.StencilMaskSeparate(GL_BACK, state.stencilWriteMask);
      s.stencilWriteMask = state.stencilWriteMask;
    }
    if (state.stencilFail != s.sFail || state.stencilDepthFail != s.sDepthFail || state.stencilPass != s.sPass) {
      gl.StencilOpSeparate(GL_FRONT, state.stencilFail, state.stencilDepthFail, state.stencilPass);
      gl.StencilOpSeparate(GL_BACK, state.stencilFail, state.stencilDepthFail, state.stencilPass);
      s.sFail = state.stencilFail, s.sDepthFail = state.stencilDepthFail, s.sPass = state.stencilPass;
    }
  }
}

void use_program(GLuint program) {
  if (program != s.program) {
    gl.UseProgram(program);
    s.program = program;
  }
}

void bind_vertex_array(GLuint vao) {
  if (vao != s.vao) {
    gl.BindVertexArray(vao);
    s.vao = vao;
  }
}

void bind_texture_unit(uint32_t unit, GLuint texture, GLuint sampler) {
  if (unit >= kMaxTextureBindings) {
    return;
  }
  auto& u = s.textures[unit];
  if (u.texture != texture) {
    if (s.activeUnit != unit) {
      gl.ActiveTexture(GL_TEXTURE0 + unit);
      s.activeUnit = unit;
    }
    gl.BindTexture(GL_TEXTURE_2D, texture);
    u.texture = texture;
  }
  if (u.sampler != sampler) {
    gl.BindSampler(unit, sampler);
    u.sampler = sampler;
  }
}

void bind_uniform_range(uint32_t index, GLuint buffer, uint32_t offset, uint32_t size) {
  if (index >= kMaxBufferBindings) {
    return;
  }
  auto& b = s.ubos[index];
  if (b.buffer != buffer || b.offset != offset || b.size != size) {
    gl.BindBufferRange(GL_UNIFORM_BUFFER, index, buffer, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(size));
    b.buffer = buffer, b.offset = offset, b.size = size;
  }
}

void set_viewport_gl(GLint x, GLint y, GLsizei width, GLsizei height, GLfloat nearDepth, GLfloat farDepth) {
  if (x != s.vpX || y != s.vpY || width != s.vpW || height != s.vpH) {
    gl.Viewport(x, y, width, height);
    s.vpX = x, s.vpY = y, s.vpW = width, s.vpH = height;
  }
  if (nearDepth != s.depthNear || farDepth != s.depthFar) {
    gl.DepthRangef(nearDepth, farDepth);
    s.depthNear = nearDepth, s.depthFar = farDepth;
  }
}

void set_scissor_gl(GLint x, GLint y, GLsizei width, GLsizei height) {
  if (x != s.scX || y != s.scY || width != s.scW || height != s.scH) {
    gl.Scissor(x, y, width, height);
    s.scX = x, s.scY = y, s.scW = width, s.scH = height;
  }
}

void set_blend_color(float r, float g, float b, float a) {
  if (r != s.blendColorR || g != s.blendColorG || b != s.blendColorB || a != s.blendColorA) {
    gl.BlendColor(r, g, b, a);
    s.blendColorR = r, s.blendColorG = g, s.blendColorB = b, s.blendColorA = a;
  }
}

void set_stencil_reference(uint32_t reference) {
  const GLint ref = static_cast<GLint>(reference);
  if (ref != s.stencilRef) {
    s.stencilRef = ref;
    apply_stencil_func();
  }
}

} // namespace aurora::gl

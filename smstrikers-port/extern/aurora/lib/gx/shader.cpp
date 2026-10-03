#include "../gfx/common.hpp"

#include "../gl/program.hpp"
#include "../internal.hpp"
#include "../webgpu/gpu.hpp"
#include "gx.hpp"
#include "gx_fmt.hpp"
#include "shader_info.hpp"

#include <dolphin/gx/GXEnum.h>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <mutex>
#include <string_view>
#include <utility>

#include "tracy/Tracy.hpp"

namespace aurora::gx {
using namespace fmt::literals;
using namespace std::string_literals;
using namespace std::string_view_literals;

static Module Log("aurora::gfx::gx");

absl::flat_hash_set<gfx::ShaderRef> g_seenShaders;

// GLSL program dedup: distinct GX PipelineConfigs that differ only in fixed-function state (blend,
// depth, cull) share the same ShaderConfig and therefore the same linked GL program. Compiling once
// and returning the shared GL name across those pipelines cuts cold-boot compiles (the long pole)
// and keeps the program-binary cache smaller. Programs are never deleted mid-run (they live with the
// context), so sharing one GLuint across pipelines is safe. Single-compiler by construction (builds
// run on the pipeline-compiler thread, or the render worker in threadless mode); the mutex is cheap
// insurance, not contended.
static std::mutex g_programCacheMutex;
static absl::flat_hash_map<gfx::ShaderRef, uint32_t> g_programByShaderHash;

static inline std::string_view chan_comp(GXTevColorChan chan) noexcept {
  switch (chan) {
  case GX_CH_RED:
    return "r";
  case GX_CH_GREEN:
    return "g";
  case GX_CH_BLUE:
    return "b";
  case GX_CH_ALPHA:
    return "a";
  default:
    return "?";
  }
}

static bool is_alpha_bump_channel(GXChannelID id) noexcept { return id == GX_ALPHA_BUMP || id == GX_ALPHA_BUMPN; }

static std::string tev_mask_expr(const std::string& value, u32 mask) {
  // t_IndTexCoord is already expanded into the 0..255 indirect sample domain.
  return fmt::format("(f32(u32({}) & 0x{:X}u) / 255.0)", value, mask);
}

static std::string alpha_bump_sel(size_t stageIdx, const ShaderConfig& config, const TevStage& stage) {
  if (stage.indTexStage >= config.numIndStages || stage.indTexAlphaSel == GX_ITBA_OFF) {
    return "0.0";
  }

  std::string baseCoord;
  switch (stage.indTexAlphaSel) {
    DEFAULT_FATAL("invalid indTexAlphaSel {} for stage {}", underlying(stage.indTexAlphaSel), stageIdx);
  case GX_ITBA_S:
    baseCoord = fmt::format("t_IndTexCoord{}.x", underlying(stage.indTexStage));
    break;
  case GX_ITBA_T:
    baseCoord = fmt::format("t_IndTexCoord{}.y", underlying(stage.indTexStage));
    break;
  case GX_ITBA_U:
    baseCoord = fmt::format("t_IndTexCoord{}.z", underlying(stage.indTexStage));
    break;
  case GX_ITBA_OFF:
    return "0.0";
  }

  switch (stage.indTexFormat) {
    DEFAULT_FATAL("invalid indirect format {} for stage {}", underlying(stage.indTexFormat), stageIdx);
  case GX_ITF_8:
    return tev_mask_expr(baseCoord, 0xF8u);
  case GX_ITF_5:
    return tev_mask_expr(baseCoord, 0xE0u);
  case GX_ITF_4:
    return tev_mask_expr(baseCoord, 0xF0u);
  case GX_ITF_3:
    return tev_mask_expr(baseCoord, 0xF8u);
  }
}

static bool uses_texture_sample(const TevStage& stage) noexcept {
  const auto& c = stage.colorPass;
  const auto& a = stage.alphaPass;
  return c.a == GX_CC_TEXC || c.a == GX_CC_TEXA || c.b == GX_CC_TEXC || c.b == GX_CC_TEXA || c.c == GX_CC_TEXC ||
         c.c == GX_CC_TEXA || c.d == GX_CC_TEXC || c.d == GX_CC_TEXA || a.a == GX_CA_TEXA || a.b == GX_CA_TEXA ||
         a.c == GX_CA_TEXA || a.d == GX_CA_TEXA;
}

u8 color_channel(GXChannelID id) noexcept {
  switch (id) {
    DEFAULT_FATAL("unimplemented color channel {}", id);
  case GX_COLOR0:
  case GX_ALPHA0:
  case GX_COLOR0A0:
    return 0;
  case GX_COLOR1:
  case GX_ALPHA1:
  case GX_COLOR1A1:
    return 1;
  }
}

static std::string color_arg_reg(GXTevColorArg arg, size_t stageIdx, const ShaderConfig& config,
                                 const TevStage& stage) {
  switch (arg) {
    DEFAULT_FATAL("invalid color arg {}", underlying(arg));
  case GX_CC_CPREV:
    return "prev.rgb";
  case GX_CC_APREV:
    return "vec3f(prev.a)";
  case GX_CC_C0:
    return "tevreg0.rgb";
  case GX_CC_A0:
    return "vec3f(tevreg0.a)";
  case GX_CC_C1:
    return "tevreg1.rgb";
  case GX_CC_A1:
    return "vec3f(tevreg1.a)";
  case GX_CC_C2:
    return "tevreg2.rgb";
  case GX_CC_A2:
    return "vec3f(tevreg2.a)";
  case GX_CC_TEXC: {
    CHECK(stage.texMapId != GX_TEXMAP_NULL, "unmapped texture for stage {}", stageIdx);
    CHECK(stage.texMapId >= GX_TEXMAP0 && stage.texMapId <= GX_TEXMAP7, "invalid texture {} for stage {}",
          underlying(stage.texMapId), stageIdx);
    const auto& swap = config.tevSwapTable[stage.tevSwapTex];
    return fmt::format("sampled{}.{}{}{}", stageIdx, chan_comp(swap.red), chan_comp(swap.green), chan_comp(swap.blue));
  }
  case GX_CC_TEXA: {
    CHECK(stage.texMapId != GX_TEXMAP_NULL, "unmapped texture for stage {}", stageIdx);
    CHECK(stage.texMapId >= GX_TEXMAP0 && stage.texMapId <= GX_TEXMAP7, "invalid texture {} for stage {}",
          underlying(stage.texMapId), stageIdx);
    const auto& swap = config.tevSwapTable[stage.tevSwapTex];
    return fmt::format("vec3f(sampled{}.{})", stageIdx, chan_comp(swap.alpha));
  }
  case GX_CC_RASC: {
    // CHECK(stage.channelId != GX_COLOR_NULL, "unmapped color channel for stage {}", stageIdx);
    if (stage.channelId == GX_COLOR_ZERO || stage.channelId == GX_COLOR_NULL) {
      return "vec3f(0.0)";
    }
    if (is_alpha_bump_channel(stage.channelId)) {
      std::string alpha = alpha_bump_sel(stageIdx, config, stage);
      if (stage.channelId == GX_ALPHA_BUMPN) {
        alpha = fmt::format("({} * (255.0 / 248.0))", alpha);
      }
      return fmt::format("vec3f({})", alpha);
    }
    u32 idx = color_channel(stage.channelId);
    const auto& swap = config.tevSwapTable[stage.tevSwapRas];
    return fmt::format("rast{}.{}{}{}", idx, chan_comp(swap.red), chan_comp(swap.green), chan_comp(swap.blue));
  }
  case GX_CC_RASA: {
    // CHECK(stage.channelId != GX_COLOR_NULL, "unmapped color channel for stage {}", stageIdx);
    if (stage.channelId == GX_COLOR_ZERO || stage.channelId == GX_COLOR_NULL) {
      return "vec3f(0.0)";
    }
    if (is_alpha_bump_channel(stage.channelId)) {
      std::string alpha = alpha_bump_sel(stageIdx, config, stage);
      if (stage.channelId == GX_ALPHA_BUMPN) {
        alpha = fmt::format("({} * (255.0 / 248.0))", alpha);
      }
      return fmt::format("vec3f({})", alpha);
    }
    u32 idx = color_channel(stage.channelId);
    const auto& swap = config.tevSwapTable[stage.tevSwapRas];
    return fmt::format("vec3f(rast{}.{})", idx, chan_comp(swap.alpha));
  }
  case GX_CC_ONE:
    return "vec3f(1.0)";
  case GX_CC_HALF:
    return "vec3f(0.5)";
  case GX_CC_KONST: {
    switch (stage.kcSel) {
      DEFAULT_FATAL("invalid kcSel {}", underlying(stage.kcSel));
    case GX_TEV_KCSEL_8_8:
      return "vec3f(1.0)";
    case GX_TEV_KCSEL_7_8:
      return "vec3f(7.0/8.0)";
    case GX_TEV_KCSEL_6_8:
      return "vec3f(6.0/8.0)";
    case GX_TEV_KCSEL_5_8:
      return "vec3f(5.0/8.0)";
    case GX_TEV_KCSEL_4_8:
      return "vec3f(4.0/8.0)";
    case GX_TEV_KCSEL_3_8:
      return "vec3f(3.0/8.0)";
    case GX_TEV_KCSEL_2_8:
      return "vec3f(2.0/8.0)";
    case GX_TEV_KCSEL_1_8:
      return "vec3f(1.0/8.0)";
    case GX_TEV_KCSEL_K0:
      return "ubuf.kcolor0.rgb";
    case GX_TEV_KCSEL_K1:
      return "ubuf.kcolor1.rgb";
    case GX_TEV_KCSEL_K2:
      return "ubuf.kcolor2.rgb";
    case GX_TEV_KCSEL_K3:
      return "ubuf.kcolor3.rgb";
    case GX_TEV_KCSEL_K0_R:
      return "vec3f(ubuf.kcolor0.r)";
    case GX_TEV_KCSEL_K1_R:
      return "vec3f(ubuf.kcolor1.r)";
    case GX_TEV_KCSEL_K2_R:
      return "vec3f(ubuf.kcolor2.r)";
    case GX_TEV_KCSEL_K3_R:
      return "vec3f(ubuf.kcolor3.r)";
    case GX_TEV_KCSEL_K0_G:
      return "vec3f(ubuf.kcolor0.g)";
    case GX_TEV_KCSEL_K1_G:
      return "vec3f(ubuf.kcolor1.g)";
    case GX_TEV_KCSEL_K2_G:
      return "vec3f(ubuf.kcolor2.g)";
    case GX_TEV_KCSEL_K3_G:
      return "vec3f(ubuf.kcolor3.g)";
    case GX_TEV_KCSEL_K0_B:
      return "vec3f(ubuf.kcolor0.b)";
    case GX_TEV_KCSEL_K1_B:
      return "vec3f(ubuf.kcolor1.b)";
    case GX_TEV_KCSEL_K2_B:
      return "vec3f(ubuf.kcolor2.b)";
    case GX_TEV_KCSEL_K3_B:
      return "vec3f(ubuf.kcolor3.b)";
    case GX_TEV_KCSEL_K0_A:
      return "vec3f(ubuf.kcolor0.a)";
    case GX_TEV_KCSEL_K1_A:
      return "vec3f(ubuf.kcolor1.a)";
    case GX_TEV_KCSEL_K2_A:
      return "vec3f(ubuf.kcolor2.a)";
    case GX_TEV_KCSEL_K3_A:
      return "vec3f(ubuf.kcolor3.a)";
    }
  }
  case GX_CC_ZERO:
    return "vec3f(0.0)";
  }
}

static std::string alpha_arg_reg(GXTevAlphaArg arg, size_t stageIdx, const ShaderConfig& config,
                                 const TevStage& stage) {
  switch (arg) {
    DEFAULT_FATAL("invalid alpha arg {}", underlying(arg));
  case GX_CA_APREV:
    return "prev.a";
  case GX_CA_A0:
    return "tevreg0.a";
  case GX_CA_A1:
    return "tevreg1.a";
  case GX_CA_A2:
    return "tevreg2.a";
  case GX_CA_TEXA: {
    CHECK(stage.texMapId != GX_TEXMAP_NULL, "unmapped texture for stage {}", stageIdx);
    CHECK(stage.texMapId >= GX_TEXMAP0 && stage.texMapId <= GX_TEXMAP7, "invalid texture {} for stage {}",
          underlying(stage.texMapId), stageIdx);
    const auto& swap = config.tevSwapTable[stage.tevSwapTex];
    return fmt::format("sampled{}.{}", stageIdx, chan_comp(swap.alpha));
  }
  case GX_CA_RASA: {
    // CHECK(stage.channelId != GX_COLOR_NULL, "unmapped color channel for stage {}", stageIdx);
    if (stage.channelId == GX_COLOR_ZERO || stage.channelId == GX_COLOR_NULL) {
      return "0.0";
    }
    if (is_alpha_bump_channel(stage.channelId)) {
      std::string alpha = alpha_bump_sel(stageIdx, config, stage);
      if (stage.channelId == GX_ALPHA_BUMPN) {
        alpha = fmt::format("({} * (255.0 / 248.0))", alpha);
      }
      return alpha;
    }
    u32 idx = color_channel(stage.channelId);
    const auto& swap = config.tevSwapTable[stage.tevSwapRas];
    return fmt::format("rast{}.{}", idx, chan_comp(swap.alpha));
  }
  case GX_CA_KONST: {
    switch (stage.kaSel) {
      DEFAULT_FATAL("invalid kaSel {}", underlying(stage.kaSel));
    case GX_TEV_KASEL_8_8:
      return "1.0";
    case GX_TEV_KASEL_7_8:
      return "(7.0/8.0)";
    case GX_TEV_KASEL_6_8:
      return "(6.0/8.0)";
    case GX_TEV_KASEL_5_8:
      return "(5.0/8.0)";
    case GX_TEV_KASEL_4_8:
      return "(4.0/8.0)";
    case GX_TEV_KASEL_3_8:
      return "(3.0/8.0)";
    case GX_TEV_KASEL_2_8:
      return "(2.0/8.0)";
    case GX_TEV_KASEL_1_8:
      return "(1.0/8.0)";
    case GX_TEV_KASEL_K0_R:
      return "ubuf.kcolor0.r";
    case GX_TEV_KASEL_K1_R:
      return "ubuf.kcolor1.r";
    case GX_TEV_KASEL_K2_R:
      return "ubuf.kcolor2.r";
    case GX_TEV_KASEL_K3_R:
      return "ubuf.kcolor3.r";
    case GX_TEV_KASEL_K0_G:
      return "ubuf.kcolor0.g";
    case GX_TEV_KASEL_K1_G:
      return "ubuf.kcolor1.g";
    case GX_TEV_KASEL_K2_G:
      return "ubuf.kcolor2.g";
    case GX_TEV_KASEL_K3_G:
      return "ubuf.kcolor3.g";
    case GX_TEV_KASEL_K0_B:
      return "ubuf.kcolor0.b";
    case GX_TEV_KASEL_K1_B:
      return "ubuf.kcolor1.b";
    case GX_TEV_KASEL_K2_B:
      return "ubuf.kcolor2.b";
    case GX_TEV_KASEL_K3_B:
      return "ubuf.kcolor3.b";
    case GX_TEV_KASEL_K0_A:
      return "ubuf.kcolor0.a";
    case GX_TEV_KASEL_K1_A:
      return "ubuf.kcolor1.a";
    case GX_TEV_KASEL_K2_A:
      return "ubuf.kcolor2.a";
    case GX_TEV_KASEL_K3_A:
      return "ubuf.kcolor3.a";
    }
  }
  case GX_CA_ZERO:
    return "0.0";
  }
}

static std::string tev_op(GXTevOp op, std::string_view bias, std::string_view scale, std::string_view a,
                          std::string_view b, std::string_view c, std::string_view d, std::string_view zero) {
  // GLSL has no WGSL select(false, true, cond); the equivalent is the ternary
  // (cond ? true : false). Value args here can be scalar (alpha) or vec3 (color) and
  // the compare is a scalar bool, so mix(vec,vec,bool) is unavailable — ternary it is.
  switch (op) {
    DEFAULT_FATAL("unimplemented tev op {}", underlying(op));
  case GX_TEV_ADD:
  case GX_TEV_SUB: {
    std::string_view neg = op == GX_TEV_SUB ? "-"sv : ""sv;
    return fmt::format("(({0}mix({1}, {2}, {3}) + {4}){5}){6}", neg, a, b, c, d, bias, scale);
  }
  case GX_TEV_COMP_R8_GT:
    return fmt::format("((round({0}.r * 255.0) > round({1}.r * 255.0)) ? {2} : {3}) + {4}", a, b, c, zero, d);
  case GX_TEV_COMP_R8_EQ:
    return fmt::format("((round({0}.r * 255.0) == round({1}.r * 255.0)) ? {2} : {3}) + {4}", a, b, c, zero, d);
  case GX_TEV_COMP_GR16_GT:
    return fmt::format(
        "((round(dot({0}.rg * 255.0, vec2(1.0, 256.0))) > round(dot({1}.rg * 255.0, vec2(1.0, 256.0)))) ? {2} : {3})"
        " + {4}",
        a, b, c, zero, d);
  case GX_TEV_COMP_GR16_EQ:
    return fmt::format(
        "((round(dot({0}.rg * 255.0, vec2(1.0, 256.0))) == round(dot({1}.rg * 255.0, vec2(1.0, 256.0)))) ? {2} : {3})"
        " + {4}",
        a, b, c, zero, d);
  case GX_TEV_COMP_BGR24_GT:
    return fmt::format(
        "((round(dot({0}.rgb * 255.0, vec3(1.0, 256.0, 65536.0))) > round(dot({1}.rgb * 255.0, "
        "vec3(1.0, 256.0, 65536.0)))) ? {2} : {3}) + {4}",
        a, b, c, zero, d);
  case GX_TEV_COMP_BGR24_EQ:
    return fmt::format(
        "((round(dot({0}.rgb * 255.0, vec3(1.0, 256.0, 65536.0))) == round(dot({1}.rgb * 255.0, "
        "vec3(1.0, 256.0, 65536.0)))) ? {2} : {3}) + {4}",
        a, b, c, zero, d);
  case GX_TEV_COMP_RGB8_GT:
    return fmt::format("((round({0} * 255.0) > round({1} * 255.0)) ? {2} : {3}) + {4}", a, b, c, zero, d);
  case GX_TEV_COMP_RGB8_EQ:
    return fmt::format("((round({0} * 255.0) == round({1} * 255.0)) ? {2} : {3}) + {4}", a, b, c, zero, d);
  }
}

static std::string tev_color_op(GXTevOp op, std::string_view bias, std::string_view scale, bool clamp,
                                std::string_view a, std::string_view b, std::string_view c, std::string_view d) {
  const auto overflow = [](std::string_view reg) { return fmt::format("tev_overflow_vec3f({})", reg); };
  std::string expr = tev_op(op, bias, scale, overflow(a), overflow(b), overflow(c), d, "vec3(0)"sv);
  return clamp ? fmt::format("clamp({}, vec3f(0.0), vec3f(1.0))", expr)
               : fmt::format("clamp({}, vec3f(-4.0), vec3f(4.0))", expr);
}

static std::string tev_alpha_op(GXTevOp op, std::string_view bias, std::string_view scale, bool clamp,
                                std::string_view a, std::string_view b, std::string_view c, std::string_view d) {
  const auto overflow = [](std::string_view reg) { return fmt::format("tev_overflow_f32({})", reg); };
  std::string expr = tev_op(op, bias, scale, overflow(a), overflow(b), overflow(c), d, "0.0"sv);
  return clamp ? fmt::format("clamp({}, 0.0, 1.0)", expr) : fmt::format("clamp({}, -4.0, 4.0)", expr);
}

static std::string_view tev_bias(GXTevBias bias) {
  switch (bias) {
    DEFAULT_FATAL("invalid tev bias {}", underlying(bias));
  case GX_TB_ZERO:
    return ""sv;
  case GX_TB_ADDHALF:
    return " + 0.5"sv;
  case GX_TB_SUBHALF:
    return " - 0.5"sv;
  }
}

struct AlphaCompareExpr {
  std::string expr;
  int constant = -1;
};

static AlphaCompareExpr alpha_compare_const(bool value) { return {value ? "true"s : "false"s, value ? 1 : 0}; }

static AlphaCompareExpr alpha_compare_not(const AlphaCompareExpr& expr) {
  if (expr.constant != -1) {
    return alpha_compare_const(expr.constant == 0);
  }
  return {fmt::format("!{}", expr.expr), -1};
}

static AlphaCompareExpr alpha_compare_and(const AlphaCompareExpr& lhs, const AlphaCompareExpr& rhs) {
  if (lhs.constant == 0 || rhs.constant == 0) {
    return alpha_compare_const(false);
  }
  if (lhs.constant == 1) {
    return rhs;
  }
  if (rhs.constant == 1) {
    return lhs;
  }
  return {fmt::format("({} && {})", lhs.expr, rhs.expr), -1};
}

static AlphaCompareExpr alpha_compare_or(const AlphaCompareExpr& lhs, const AlphaCompareExpr& rhs) {
  if (lhs.constant == 1 || rhs.constant == 1) {
    return alpha_compare_const(true);
  }
  if (lhs.constant == 0) {
    return rhs;
  }
  if (rhs.constant == 0) {
    return lhs;
  }
  return {fmt::format("({} || {})", lhs.expr, rhs.expr), -1};
}

static AlphaCompareExpr alpha_compare_xor(const AlphaCompareExpr& lhs, const AlphaCompareExpr& rhs) {
  if (lhs.constant != -1 && rhs.constant != -1) {
    return alpha_compare_const(lhs.constant != rhs.constant);
  }
  if (lhs.constant == 0) {
    return rhs;
  }
  if (rhs.constant == 0) {
    return lhs;
  }
  if (lhs.constant == 1) {
    return alpha_compare_not(rhs);
  }
  if (rhs.constant == 1) {
    return alpha_compare_not(lhs);
  }
  return {fmt::format("({} != {})", lhs.expr, rhs.expr), -1};
}

static AlphaCompareExpr alpha_compare_xnor(const AlphaCompareExpr& lhs, const AlphaCompareExpr& rhs) {
  if (lhs.constant != -1 && rhs.constant != -1) {
    return alpha_compare_const(lhs.constant == rhs.constant);
  }
  if (lhs.constant == 0) {
    return alpha_compare_not(rhs);
  }
  if (rhs.constant == 0) {
    return alpha_compare_not(lhs);
  }
  if (lhs.constant == 1) {
    return rhs;
  }
  if (rhs.constant == 1) {
    return lhs;
  }
  return {fmt::format("({} == {})", lhs.expr, rhs.expr), -1};
}

static AlphaCompareExpr alpha_compare(GXCompare comp, u8 ref) {
  const auto iref = static_cast<u32>(ref);
  switch (comp) {
    DEFAULT_FATAL("invalid alpha comp {}", underlying(comp));
  case GX_NEVER:
    return alpha_compare_const(false);
  case GX_LESS:
    if (ref == 0) {
      return alpha_compare_const(false);
    }
    return {fmt::format("(alphaCompare < {}u)", iref), -1};
  case GX_LEQUAL:
    if (ref == 255) {
      return alpha_compare_const(true);
    }
    return {fmt::format("(alphaCompare <= {}u)", iref), -1};
  case GX_EQUAL:
    return {fmt::format("(alphaCompare == {}u)", iref), -1};
  case GX_NEQUAL:
    return {fmt::format("(alphaCompare != {}u)", iref), -1};
  case GX_GEQUAL:
    if (ref == 0) {
      return alpha_compare_const(true);
    }
    return {fmt::format("(alphaCompare >= {}u)", iref), -1};
  case GX_GREATER:
    if (ref == 255) {
      return alpha_compare_const(false);
    }
    return {fmt::format("(alphaCompare > {}u)", iref), -1};
  case GX_ALWAYS:
    return alpha_compare_const(true);
  }
}

static std::string_view tev_scale(GXTevScale scale) {
  switch (scale) {
    DEFAULT_FATAL("invalid tev scale {}", underlying(scale));
  case GX_CS_SCALE_1:
    return ""sv;
  case GX_CS_SCALE_2:
    return " * 2.0"sv;
  case GX_CS_SCALE_4:
    return " * 4.0"sv;
  case GX_CS_DIVIDE_2:
    return " / 2.0"sv;
  }
}

static inline std::string vtx_attr(const ShaderConfig& config, GXAttr attr) {
  const auto type = config.attrs[attr].attrType;
  if (type == GX_NONE) {
    if (attr == GX_VA_PNMTXIDX) {
      return "ubuf.current_pnmtx";
    }
    if (attr == GX_VA_NRM) {
      // Default normal
      return "vec3f(1.0, 0.0, 0.0)"s;
    }
    if (attr == GX_VA_CLR0 || attr == GX_VA_CLR1) {
      return "vec4f(0.0, 0.0, 0.0, 0.0)"s;
    }
    UNLIKELY FATAL("unmapped vtx attr {}", underlying(attr));
  }
  if (attr == GX_VA_POS) {
    return "in_pos"s;
  }
  if (attr == GX_VA_NRM) {
    return "in_nrm"s;
  }
  if (attr == GX_VA_CLR0 || attr == GX_VA_CLR1) {
    const auto idx = attr - GX_VA_CLR0;
    return fmt::format("in_clr{}", idx);
  }
  if (attr >= GX_VA_TEX0 && attr <= GX_VA_TEX7) {
    const auto idx = attr - GX_VA_TEX0;
    return fmt::format("in_tex{}_uv", idx);
  }
  if (attr == GX_VA_PNMTXIDX) {
    return "in_pnmtxidx"s;
  }
  if (attr >= GX_VA_TEX0MTXIDX && attr <= GX_VA_TEX7MTXIDX) {
    const auto idx = attr - GX_VA_TEX0MTXIDX;
    return fmt::format("in_texmtxidx{}", idx);
  }
  UNLIKELY FATAL("unhandled vtx attr {}", underlying(attr));
}

constexpr std::array<std::string_view, GX_CC_ZERO + 1> TevColorArgNames{
    "CPREV"sv, "APREV"sv, "C0"sv,   "A0"sv,   "C1"sv,  "A1"sv,   "C2"sv,    "A2"sv,
    "TEXC"sv,  "TEXA"sv,  "RASC"sv, "RASA"sv, "ONE"sv, "HALF"sv, "KONST"sv, "ZERO"sv,
};
constexpr std::array<std::string_view, GX_CA_ZERO + 1> TevAlphaArgNames{
    "APREV"sv, "A0"sv, "A1"sv, "A2"sv, "TEXA"sv, "RASA"sv, "KONST"sv, "ZERO"sv,
};

auto fetch_fixed16_attr(std::string_view fetchFn, const AttrConfig& mapping, std::string_view buf, 
                        std::string_view offs, bool le) -> std::string {
  // Some Adreno drivers appear sensitive to generated shaders that route 2- and
  // 3-component fixed-16 vertex attributes through reusable vector fetch helpers.
  // Emitting scalar component fetches at the call site avoids observed artifacts.
  if (mapping.cnt == 2) {
    const auto comp0 = fmt::format("{}_1(&{}, {} + 0u, {}u, {})", fetchFn, buf, offs, mapping.frac, le);
    const auto comp1 = fmt::format("{}_1(&{}, {} + 2u, {}u, {})", fetchFn, buf, offs, mapping.frac, le);
    return fmt::format("vec2f({}, {})", comp0, comp1);
  }
  if (mapping.cnt == 3) {
    const auto comp0 = fmt::format("{}_1(&{}, {} + 0u, {}u, {})", fetchFn, buf, offs, mapping.frac, le);
    const auto comp1 = fmt::format("{}_1(&{}, {} + 2u, {}u, {})", fetchFn, buf, offs, mapping.frac, le);
    const auto comp2 = fmt::format("{}_1(&{}, {} + 4u, {}u, {})", fetchFn, buf, offs, mapping.frac, le);
    return fmt::format("vec3f({}, {}, {})", comp0, comp1, comp2);
  }
  return fmt::format("{}_{}(&{}, {}, {}u, {})", fetchFn, mapping.cnt, buf, offs, mapping.frac, le);
}

auto fetch_attr(const AttrConfig& mapping, std::string_view buf, std::string_view offs, bool le) -> std::string {
  switch (mapping.compType) {
  case GX_U8:
    return fmt::format("fetch_u8_{}(&{}, {}, {}, {})", mapping.cnt, buf, offs, mapping.frac, le);
  case GX_S8:
    return fmt::format("fetch_s8_{}(&{}, {}, {}, {})", mapping.cnt, buf, offs, mapping.frac, le);
  case GX_U16:
    return fetch_fixed16_attr("fetch_u16"sv, mapping, buf, offs, le);
  case GX_S16:
    return fetch_fixed16_attr("fetch_s16"sv, mapping, buf, offs, le);
  case GX_F32:
    return fmt::format("fetch_f32_{}(&{}, {}, {})", mapping.cnt, buf, offs, le);
  case GX_RGBA8:
    return fmt::format("unpack4x8unorm(load_u32_raw(&{}, {}))", buf, offs);
  default:
    Log.fatal("fetch_attr: Unimplemented {}", static_cast<GXCompType>(mapping.compType));
  }
}

auto fetch_color_attr(const AttrConfig& mapping, std::string_view buf, std::string_view offs, bool le) -> std::string {
  switch (mapping.compType) {
  case GX_RGB565:
    return fmt::format("fetch_rgb565(&{}, {}, {})", buf, offs, le);
  case GX_RGB8:
    return fmt::format("fetch_rgb8(&{}, {}, {})", buf, offs, le);
  case GX_RGBX8:
    return fmt::format("fetch_rgbx8(&{}, {}, {})", buf, offs, le);
  case GX_RGBA4:
    return fmt::format("fetch_rgba4(&{}, {}, {})", buf, offs, le);
  case GX_RGBA6:
    return fmt::format("fetch_rgba6(&{}, {}, {})", buf, offs, le);
  case GX_RGBA8:
    return fmt::format("fetch_rgba8(&{}, {}, {})", buf, offs, le);
  default:
    Log.fatal("fetch_color_attr: Unimplemented {}", static_cast<GXCompType>(mapping.compType));
  }
}

struct AttrAddress {
  std::string offs;
  std::string_view buf;
  bool le;
};

auto attr_address(const AttrConfig& mapping, GXAttr attr, std::string_view vidx, u32 vtxStride, u32 dlExtra, u32 within)
    -> AttrAddress {
  const u32 dlOffset = mapping.offset + dlExtra;
  if (mapping.attrType == GX_INDEX8) {
    return {fmt::format("ubuf.array_start[{}] + raw_fetch_u8_1(&vbuf, ubuf.vtx_start + {} * {}u + {}u) * {}u + {}u",
                        attr - GX_VA_POS, vidx, vtxStride, dlOffset, mapping.stride, within),
            "abuf"sv, mapping.le};
  }
  if (mapping.attrType == GX_INDEX16) {
    return {
        fmt::format("ubuf.array_start[{}] + raw_fetch_u16_1(&vbuf, ubuf.vtx_start + {} * {}u + {}u, false) * {}u + {}u",
                    attr - GX_VA_POS, vidx, vtxStride, dlOffset, mapping.stride, within),
        "abuf"sv, mapping.le};
  }
  return {fmt::format("ubuf.vtx_start + {} * {}u + {}u", vidx, vtxStride, dlOffset + within), "vbuf"sv, false};
}

auto attr_load(const ShaderConfig& config, GXAttr attr, std::string_view vidx) -> std::string {
  const auto& mapping = config.attrs[attr];
  if (mapping.attrType == GX_NONE) {
    return vtx_attr(config, attr);
  }
  const auto [offs, buf, le] = attr_address(mapping, attr, vidx, config.vtxStride, 0u, 0u);
  switch (attr) {
  case GX_VA_PNMTXIDX:
    return fmt::format("(raw_fetch_u8_1(&{}, {}) / 3u)", buf, offs);
  case GX_VA_TEX0MTXIDX:
  case GX_VA_TEX1MTXIDX:
  case GX_VA_TEX2MTXIDX:
  case GX_VA_TEX3MTXIDX:
  case GX_VA_TEX4MTXIDX:
  case GX_VA_TEX5MTXIDX:
  case GX_VA_TEX6MTXIDX:
  case GX_VA_TEX7MTXIDX:
    return fmt::format("raw_fetch_u8_1(&{}, {})", buf, offs);
  case GX_VA_POS: {
    const auto posLoad = fetch_attr(mapping, buf, offs, le);
    if (mapping.cnt == 2) {
      return fmt::format("vec3f({}, 0.0)", posLoad);
    }
    return posLoad;
  }
  case GX_VA_NRM:
    // NBT: normal only here; binormal/tangent loaded via attr_load_nbt_slice
    if (mapping.cnt > 3) {
      auto nrmMapping = mapping;
      nrmMapping.cnt = 3;
      return fetch_attr(nrmMapping, buf, offs, le);
    }
    return fetch_attr(mapping, buf, offs, le);
  case GX_VA_CLR0:
  case GX_VA_CLR1:
    return fetch_color_attr(mapping, buf, offs, le);
  case GX_VA_TEX0:
  case GX_VA_TEX1:
  case GX_VA_TEX2:
  case GX_VA_TEX3:
  case GX_VA_TEX4:
  case GX_VA_TEX5:
  case GX_VA_TEX6:
  case GX_VA_TEX7: {
    const auto texLoad = fetch_attr(mapping, buf, offs, le);
    if (mapping.cnt == 1) {
      return fmt::format("vec2f({}, 0.0)", texLoad);
    }
    return texLoad;
  }
  default:
    Log.fatal("attr_load: Unimplemented {}", attr);
  }
}

enum class NbtSlice : u8 {
  N,
  B,
  T,
};

auto attr_load_nbt_slice(const ShaderConfig& config, NbtSlice slice, std::string_view vidx) -> std::string {
  const auto& mapping = config.attrs[GX_VA_NRM];
  if (mapping.attrType == GX_NONE || mapping.cnt != 9) {
    Log.fatal("attr_load_nbt_slice: GX_TG_BINRM/TANGENT requires GX_NRM_NBT or GX_NRM_NBT3");
  }
  const auto sliceIdx = static_cast<u32>(slice);
  const auto compsize = comp_type_size(GX_VA_NRM, static_cast<GXCompType>(mapping.compType));
  u32 dlExtra = 0;
  if (mapping.nbt3) {
    if (mapping.attrType == GX_INDEX8) {
      dlExtra = sliceIdx;
    } else if (mapping.attrType == GX_INDEX16) {
      dlExtra = sliceIdx * 2u;
    }
  }
  const u32 within = sliceIdx * 3u * compsize;
  const auto [offs, buf, le] = attr_address(mapping, GX_VA_NRM, vidx, config.vtxStride, dlExtra, within);
  auto sliceMapping = mapping;
  sliceMapping.cnt = 3;
  return fetch_attr(sliceMapping, buf, offs, le);
}

static constexpr std::string_view nbt_slice_local(NbtSlice slice) noexcept {
  return slice == NbtSlice::B ? "in_binrm" : "in_tangent";
}

static constexpr bool is_emboss_texgen(GXTexGenType type) noexcept {
  return type >= GX_TG_BUMP0 && type <= GX_TG_BUMP7;
}

auto lighting_func(const ShaderConfig& config, const ColorChannelConfig& cc, u8 i, bool alpha) -> std::string {
  // GLSL, per-vertex only (UsePerPixelLighting is a dead constexpr in this fork). The
  // vertex-color channel drives a varying `v_ccN`; `mv_pos`/`mv_nrm` are the vertex
  // main()'s locals. WGSL `var`->typed decls, `select(f,t,c)`->ternary.
  std::string_view swizzle = alpha ? ".a"sv : ""sv;
  const std::string outVar = fmt::format("v_cc{}", i);
  std::string_view posVar = "mv_pos"sv;
  std::string ambSrc, matSrc;
  if (cc.ambSrc == GX_SRC_VTX) {
    ambSrc = vtx_attr(config, static_cast<GXAttr>(GX_VA_CLR0 + i));
  } else if (cc.ambSrc == GX_SRC_REG) {
    ambSrc = fmt::format("ubuf.cc{0}{1}_amb", i, alpha ? "a"sv : ""sv);
  }
  if (cc.matSrc == GX_SRC_VTX) {
    matSrc = vtx_attr(config, static_cast<GXAttr>(GX_VA_CLR0 + i));
  } else if (cc.matSrc == GX_SRC_REG) {
    matSrc = fmt::format("ubuf.cc{0}{1}_mat", i, alpha ? "a"sv : ""sv);
  }
  if (!cc.lightingEnabled) {
    return fmt::format("\n    {0}{2} = {1}{2};", outVar, matSrc, swizzle);
  }
  GXDiffuseFn diffFn = cc.diffFn;
  std::string lightAttnFn;
  if (cc.attnFn == GX_AF_NONE) {
    lightAttnFn = "attn = 1.0;"s;
  } else if (cc.attnFn == GX_AF_SPOT) {
    lightAttnFn = fmt::format(R"""(
          float cosraw = dot(ldir, light.dir);
          float cosine = (cosraw > 0.0) ? min(cosraw, 1.0) : 0.0;
          float cos_attn = dot(light.cos_att, vec3(1.0, cosine, cosine * cosine));
          float dist_attn = dot(light.dist_att, vec3(1.0, dist, dist2));
          attn = max(0.0, cos_attn / dist_attn);)""");
  } else if (cc.attnFn == GX_AF_SPEC) {
    std::string dist_attn = diffFn != GX_DF_NONE
                                ? "max(0.0, dot(normalize(light.dist_att), vec3(1.0, attn, attn * attn)));"
                                : "max(0.0, dot(light.dist_att, vec3(1.0, attn, attn * attn)));";
    lightAttnFn = fmt::format(R"""(
          float cosraw = dot(mv_nrm, light.dir);
          attn = (cosraw > 0.0 && dot(mv_nrm, ldir) >= 0.0) ? min(cosraw, 1.0) : 0.0;
          float cos_attn = dot(light.cos_att, vec3(1.0, attn, attn * attn));
          float dist_attn = {0};
          attn = max(0.0, cos_attn / dist_attn);)""",
                              dist_attn);
  }
  std::string_view lightDiffFn;
  if (diffFn == GX_DF_NONE) {
    lightDiffFn = "1.0"sv;
  } else if (diffFn == GX_DF_SIGN) {
    lightDiffFn = "dot(ldir, mv_nrm)"sv;
  } else if (diffFn == GX_DF_CLAMP) {
    lightDiffFn = "max(0.0, dot(ldir, mv_nrm))"sv;
  }
  return fmt::format(R"""(
    {{
      vec4 lighting = {5};
      for (uint li = 0u; li < {1}u; li++) {{
          if ((ubuf.lightState{0}{9} & (1u << li)) == 0u) {{ continue; }}
          Light light = ubuf.lights[li];
          vec3 ldir = light.pos - {6};
          float dist2 = dot(ldir, ldir);
          float dist = sqrt(dist2);
          ldir = ldir / dist;
          float attn;{2}
          float diff = {3};
          lighting = lighting + (attn * diff * light.color);
      }}
      {7}{8} = ({4} * clamp(lighting, vec4(0.0), vec4(1.0))){8};
    }})""",
                     i, GX::MaxLights, lightAttnFn, lightDiffFn, matSrc, ambSrc, posVar, outVar, swizzle,
                     alpha ? "a"sv : ""sv);
}

namespace {
// Final WGSL->GLSL vocabulary pass over an assembled shader body. The structural
// pieces (uniform block, varyings, vertex inputs, `let`/`var` decls, select, %,
// textureSampleBias, out./in.) are already GLSL by the time we get here; this only
// rewrites the constructor/cast spellings that the shared expression helpers
// (color_arg_reg, vtx_attr, tev_color_op, ...) still emit in WGSL form. Every
// remaining `vecNf(`/`f32(`/... is a constructor or cast, so the rename is safe.
void glslify_vocab(std::string& s) {
  static const std::pair<std::string_view, std::string_view> kSubs[]{
      {"vec2f(", "vec2("},  {"vec3f(", "vec3("},  {"vec4f(", "vec4("},  {"vec2u(", "uvec2("},
      {"vec3u(", "uvec3("}, {"vec4u(", "uvec4("}, {"vec2i(", "ivec2("}, {"vec3i(", "ivec3("},
      {"vec4i(", "ivec4("}, {"f32(", "float("},   {"u32(", "uint("},    {"i32(", "int("},
  };
  for (const auto& [from, to] : kSubs) {
    std::string::size_type pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
      s.replace(pos, from.size(), to);
      pos += to.size();
    }
  }
}

struct GlslProgram {
  std::string vertex;
  std::string fragment;
};

// The hand-rolled GLES emitter. Emits a `#version 300 es` vertex+fragment pair from
// the same ShaderConfig/ShaderInfo the WGSL emitter used. Native vertex fetch only:
// the storage byte-load prelude, the @group(0) SSBOs and the line/point instanced-quad
// expansion are dead on every live path (kSkipStorageVertexFetch) and are not ported.
// Seams: S1 GL-native orientation (no proj Y-flip; winding flip lives in state.cpp),
// S2 the one clip-Z remap line, S3 combined sampler2D + texture(...) bias, S4 std140
// (the CPU build_uniform layout is already std140-compatible — every field is scalar/
// vec2 at its natural offset or vec4/mat at 16B alignment; array_start is uvec4[3]).
GlslProgram emit_glsl(const ShaderConfig& config, const ShaderInfo& info) {
  std::string structDefs; // struct Light / Fog, shared by both stages (in the UBO)
  std::string uniformFields;
  std::string samplerDecls;
  std::string vertexInputs;
  std::string varyingsOut; // vertex `out ...;`
  std::string varyingsIn;  // fragment `in ...;` (same names)
  std::string vsBody;
  std::string fsPre;
  std::string fsBody;

  const auto addVarying = [&](std::string_view type, const std::string& name) {
    varyingsOut += fmt::format("\nout {} {};", type, name);
    varyingsIn += fmt::format("\nin {} {};", type, name);
  };

  // Native inputs: one packed `layout(location = N) in` per present attr, in canonical
  // GX attr order. MUST match native_vertex_layout()/build_native_layout() — the VAO
  // in the draw path assigns the same packed locations and per-attr GL types (S5:
  // matrix indices are integer attributes via glVertexAttribIPointer).
  {
    u32 location = 0;
    const auto addInput = [&](GXAttr attr, std::string_view type) {
      if (config.attrs[attr].attrType == GX_NONE) {
        return;
      }
      vertexInputs += fmt::format("\nlayout(location = {}) in {} {};", location++, type, vtx_attr(config, attr));
    };
    addInput(GX_VA_PNMTXIDX, "uint");
    for (GXAttr attr = GX_VA_TEX0MTXIDX; attr <= GX_VA_TEX7MTXIDX; attr = static_cast<GXAttr>(attr + 1)) {
      addInput(attr, "uint");
    }
    addInput(GX_VA_POS, "vec3");
    addInput(GX_VA_NRM, "vec3");
    addInput(GX_VA_CLR0, "vec4");
    addInput(GX_VA_CLR1, "vec4");
    for (GXAttr attr = GX_VA_TEX0; attr <= GX_VA_TEX7; attr = static_cast<GXAttr>(attr + 1)) {
      addInput(attr, "vec2");
    }
  }

  // ---- Vertex transform ----
  if (config.attrs[GX_VA_PNMTXIDX].attrType == GX_NONE) {
    vsBody += "\n    uint in_pnmtxidx = ubuf.current_pnmtx;";
  }
  vsBody += fmt::format(
      "\n    vec3 mv_pos = vec4({0}, 1.0) * ubuf.postex_mtx[in_pnmtxidx];"
      "\n    gl_Position = vec4(mv_pos, 1.0) * ubuf.proj;",
      vtx_attr(config, GX_VA_POS));
  // Reversed-Z (matches the depth compare/range/clear flips), then the GL clip-space
  // remap (S2): WebGPU clips z in [0,w], GL in [-w,w]. window depth stays bit-identical.
  if constexpr (UseReversedZ) {
    vsBody += "\n    gl_Position.z = -gl_Position.z;";
  } else {
    vsBody += "\n    gl_Position.z += gl_Position.w;";
  }
  vsBody += "\n    gl_Position.z = gl_Position.z * 2.0 - gl_Position.w;";
  if (info.usesNormals) {
    vsBody += fmt::format(
        "\n    vec3 nrm_tmp = vec4({0}, 0.0) * ubuf.nrm_mtx[in_pnmtxidx];"
        "\n    vec3 mv_nrm = (dot(nrm_tmp, nrm_tmp) > 1e-10) ? normalize(nrm_tmp) : nrm_tmp;",
        vtx_attr(config, GX_VA_NRM));
  }

  uniformFields += "\n    mat4 proj;";
  uniformFields += fmt::format("\n    mat3x4 postex_mtx[{}];", MaxPnMtx + MaxTexMtx);
  if (info.usesNormals) {
    uniformFields += fmt::format("\n    mat3x4 nrm_mtx[{}];", MaxPnMtx);
  }

  // ---- TEV register writes (fragment) ----
  static std::array regName{"prev"sv, "tevreg0"sv, "tevreg1"sv, "tevreg2"sv};
  for (u32 idx = 0; idx < config.tevStageCount; ++idx) {
    const auto& stage = config.tevStages[idx];
    {
      std::string_view outReg = regName[stage.colorOp.outReg];
      std::string op = tev_color_op(
          stage.colorOp.op, tev_bias(stage.colorOp.bias), tev_scale(stage.colorOp.scale), stage.colorOp.clamp,
          color_arg_reg(stage.colorPass.a, idx, config, stage), color_arg_reg(stage.colorPass.b, idx, config, stage),
          color_arg_reg(stage.colorPass.c, idx, config, stage), color_arg_reg(stage.colorPass.d, idx, config, stage));
      fsBody += fmt::format("\n    // TEV stage {2}\n    {0} = vec4({1}, {0}.a);", outReg, op, idx);
    }
    {
      std::string_view outReg = regName[stage.alphaOp.outReg];
      std::string op = tev_alpha_op(
          stage.alphaOp.op, tev_bias(stage.alphaOp.bias), tev_scale(stage.alphaOp.scale), stage.alphaOp.clamp,
          alpha_arg_reg(stage.alphaPass.a, idx, config, stage), alpha_arg_reg(stage.alphaPass.b, idx, config, stage),
          alpha_arg_reg(stage.alphaPass.c, idx, config, stage), alpha_arg_reg(stage.alphaPass.d, idx, config, stage));
      fsBody += fmt::format("\n    {0}.a = {1};", outReg, op);
    }
  }
  {
    const auto& lastStage = config.tevStages[config.tevStageCount - 1];
    if (lastStage.colorOp.outReg != 0) {
      fsBody += fmt::format("\n    prev = vec4({0}.rgb, prev.a);", regName[lastStage.colorOp.outReg]);
    }
    if (lastStage.alphaOp.outReg != 0) {
      fsBody += fmt::format("\n    prev.a = {0}.a;", regName[lastStage.alphaOp.outReg]);
    }
  }

  if (info.loadsTevReg.test(0)) {
    uniformFields += "\n    vec4 tevprev;";
    fsPre += "\n    vec4 prev = ubuf.tevprev;";
  } else {
    fsPre += "\n    vec4 prev;";
  }
  for (int i = 1 /* Skip TEVPREV */; i < info.loadsTevReg.size(); ++i) {
    if (info.loadsTevReg.test(i)) {
      uniformFields += fmt::format("\n    vec4 tevreg{};", i - 1);
      fsPre += fmt::format("\n    vec4 tevreg{0} = ubuf.tevreg{0};", i - 1);
    } else if (info.writesTevReg.test(i)) {
      fsPre += fmt::format("\n    vec4 tevreg{0};", i - 1);
    }
  }

  if (info.lightingEnabled) {
    uniformFields += fmt::format(FMT_STRING(R"""(
    Light lights[{}];
    uint lightState0;
    uint lightState1;
    uint lightState0a;
    uint lightState1a;)"""),
                                 GX::MaxLights);
    structDefs +=
        "\nstruct Light {\n"
        "    vec3 pos;\n"
        "    vec3 dir;\n"
        "    vec4 color;\n"
        "    vec3 cos_att;\n"
        "    vec3 dist_att;\n"
        "};\n";
  }

  // ---- Color channels (raster colors) ----
  for (int i = 0; i < info.sampledColorChannels.size(); ++i) {
    if (!info.sampledColorChannels.test(i)) {
      continue;
    }
    const auto& cc = config.colorChannels[i];
    const auto& cca = config.colorChannels[i + GX_ALPHA0];
    if (cc.lightingEnabled && cc.ambSrc == GX_SRC_REG) {
      uniformFields += fmt::format("\n    vec4 cc{0}_amb;", i);
    }
    if (cc.matSrc == GX_SRC_REG) {
      uniformFields += fmt::format("\n    vec4 cc{0}_mat;", i);
    }
    if (cca.lightingEnabled && cca.ambSrc == GX_SRC_REG) {
      uniformFields += fmt::format("\n    vec4 cc{0}a_amb;", i);
    }
    if (cca.matSrc == GX_SRC_REG) {
      uniformFields += fmt::format("\n    vec4 cc{0}a_mat;", i);
    }
    addVarying("vec4", fmt::format("v_cc{}", i));
    vsBody += lighting_func(config, cc, i, false);
    vsBody += lighting_func(config, cca, i, true);
    fsPre += fmt::format("\n    vec4 rast{0} = v_cc{0};", i);
  }
  for (int i = 0; i < info.sampledKColors.size(); ++i) {
    if (info.sampledKColors.test(i)) {
      uniformFields += fmt::format("\n    vec4 kcolor{};", i);
    }
  }

  // ---- Texture coordinate generation ----
  for (int i = 0; i < info.sampledTexCoords.size(); ++i) {
    if (!info.sampledTexCoords.test(i)) {
      continue;
    }
    const auto& tcg = config.tcgs[i];
    if (tcg.type == GX_TG_MTX3x4) {
      addVarying("vec3", fmt::format("v_tex{}_uvw", i));
    } else {
      addVarying("vec2", fmt::format("v_tex{}_uv", i));
    }
    if (is_emboss_texgen(tcg.type)) {
      const u32 lightIdx = tcg.type - GX_TG_BUMP0;
      vsBody += fmt::format(
          "\n    vec3 bump_ldir{0} = normalize(ubuf.lights[{1}].pos - mv_pos);"
          "\n    vec3 bump_tan{0} = vec4(in_tangent, 0.0) * ubuf.nrm_mtx[in_pnmtxidx];"
          "\n    vec3 bump_bin{0} = vec4(in_binrm, 0.0) * ubuf.nrm_mtx[in_pnmtxidx];"
          "\n    v_tex{0}_uv = tc{2}_proj.xy + vec2(dot(bump_ldir{0}, bump_tan{0}), dot(bump_ldir{0}, bump_bin{0}));",
          i, lightIdx, tcg.embossSrc);
      fsPre += fmt::format("\n    vec2 tex{0}_uv = v_tex{0}_uv.xy;", i);
      continue;
    }
    if (tcg.src >= GX_TG_TEX0 && tcg.src <= GX_TG_TEX7) {
      vsBody += fmt::format("\n    vec4 tc{} = vec4({}, 1.0, 1.0);", i,
                            vtx_attr(config, GXAttr(GX_VA_TEX0 + (tcg.src - GX_TG_TEX0))));
    } else if (tcg.src == GX_TG_POS) {
      vsBody += fmt::format("\n    vec4 tc{} = vec4({}, 1.0);", i, vtx_attr(config, GX_VA_POS));
    } else if (tcg.src == GX_TG_NRM) {
      vsBody += fmt::format("\n    vec4 tc{} = vec4({}, 1.0);", i, vtx_attr(config, GX_VA_NRM));
    } else if (tcg.src == GX_TG_COLOR0) {
      vsBody += fmt::format("\n    vec4 tc{} = {};", i, vtx_attr(config, GX_VA_CLR0));
    } else if (tcg.src == GX_TG_COLOR1) {
      vsBody += fmt::format("\n    vec4 tc{} = {};", i, vtx_attr(config, GX_VA_CLR1));
    } else if (tcg.src == GX_TG_BINRM) {
      vsBody += fmt::format("\n    vec4 tc{} = vec4({}, 1.0);", i, nbt_slice_local(NbtSlice::B));
    } else if (tcg.src == GX_TG_TANGENT) {
      vsBody += fmt::format("\n    vec4 tc{} = vec4({}, 1.0);", i, nbt_slice_local(NbtSlice::T));
    } else
      UNLIKELY FATAL("unhandled tcg src {}", underlying(tcg.src));
    if (tcg.type == GX_TG_MTX2x4 || tcg.type == GX_TG_MTX3x4) {
      if (info.indexAttr.test(GX_VA_TEX0MTXIDX + i)) {
        vsBody += fmt::format(
            "\n    vec3 tc{0}_tmp = tc{0} * ubuf.postex_mtx[(in_texmtxidx{0} >= {2}u) ? ({1}u + (in_texmtxidx{0} - {2}u) / 3u) : (in_texmtxidx{0} / 3u)];",
            i, MaxPnMtx, static_cast<u32>(GX_TEXMTX0));
      } else if (tcg.mtx == GX_IDENTITY) {
        vsBody += fmt::format("\n    vec3 tc{0}_tmp = tc{0}.xyz;", i);
      } else {
        const u32 texMtxIdx = tcg.mtx >= GX_TEXMTX0 ? MaxPnMtx + (tcg.mtx - GX_TEXMTX0) / 3 : tcg.mtx / 3;
        vsBody += fmt::format("\n    vec3 tc{0}_tmp = tc{0} * ubuf.postex_mtx[{1}];", i, texMtxIdx);
      }
      if (tcg.type == GX_TG_MTX2x4) {
        vsBody += fmt::format("\n    tc{0}_tmp.z = 1.0;", i);
      }
    } else if (tcg.type == GX_TG_SRTG) {
      vsBody += fmt::format("\n    vec3 tc{0}_tmp = vec3(tc{0}.xy, 1.0);", i);
    }
    if (tcg.normalize) {
      vsBody += fmt::format("\n    tc{0}_tmp = normalize(tc{0}_tmp);", i);
    }
    if (tcg.postMtx == GX_PTIDENTITY) {
      vsBody += fmt::format("\n    vec3 tc{0}_proj = tc{0}_tmp;", i);
    } else {
      u32 postMtxIdx = (tcg.postMtx - GX_PTTEXMTX0) / 3;
      vsBody += fmt::format("\n    vec3 tc{0}_proj = vec4(tc{0}_tmp.xyz, 1.0) * ubuf.postmtx[{1}];", i, postMtxIdx);
    }
    if (tcg.type == GX_TG_MTX3x4) {
      vsBody += fmt::format("\n    v_tex{0}_uvw = tc{0}_proj.xyz;", i);
      fsPre += fmt::format("\n    vec2 tex{0}_uv = v_tex{0}_uvw.xy / v_tex{0}_uvw.z;", i);
    } else {
      vsBody += fmt::format("\n    v_tex{0}_uv = tc{0}_proj.xy;", i);
      fsPre += fmt::format("\n    vec2 tex{0}_uv = v_tex{0}_uv.xy;", i);
    }
  }

  // ---- Indirect texture stages ----
  const auto ind_scale = [](const GXIndTexScale s) -> std::string_view {
    switch (s) {
    case GX_ITS_1:
      return "1.0"sv;
    case GX_ITS_2:
      return "(1.0 / 2.0)"sv;
    case GX_ITS_4:
      return "(1.0 / 4.0)"sv;
    case GX_ITS_8:
      return "(1.0 / 8.0)"sv;
    case GX_ITS_16:
      return "(1.0 / 16.0)"sv;
    case GX_ITS_32:
      return "(1.0 / 32.0)"sv;
    case GX_ITS_64:
      return "(1.0 / 64.0)"sv;
    case GX_ITS_128:
      return "(1.0 / 128.0)"sv;
    case GX_ITS_256:
      return "(1.0 / 256.0)"sv;
    default:
      FATAL("unhandled indirect scale {}", underlying(s));
    }
  };
  for (int i = 0; i < info.usedIndStages.size(); ++i) {
    if (!info.usedIndStages.test(i)) {
      continue;
    }
    const auto& indStage = config.indStages[i];
    const u32 texCoordId = underlying(indStage.texCoordId);
    const u32 texMapId = underlying(indStage.texMapId);
    const auto scaleExpr =
        fmt::format("tex{0}_uv * ubuf.texcoord_scale[{0}].xy * vec2({1}, {2}) / ubuf.tex{3}_size_bias.xy", texCoordId,
                    ind_scale(indStage.scaleS), ind_scale(indStage.scaleT), texMapId);
    fsPre += fmt::format(
        "\n    // Indirect stage {0}"
        "\n    vec3 t_IndTexCoord{0} = 255.0 * texture(tex{1}, {2}, ubuf.tex{1}_size_bias.z).abg;",
        i, texMapId, scaleExpr);
  }
  if (info.usedIndStages.any()) {
    fsPre += "\n    vec2 t_TexCoord = vec2(0.0);";
  }
  for (int i = 0; i < config.tevStageCount; ++i) {
    const auto& stage = config.tevStages[i];
    const bool needsIndirectCoord = stage.indTexMtxId != GX_ITM_OFF;
    const bool hasIndirectStage = stage.indTexStage < config.numIndStages;
    const bool needsTevTexCoord =
        needsIndirectCoord || stage.indTexWrapS != GX_ITW_OFF || stage.indTexWrapT != GX_ITW_OFF || stage.indTexAddPrev;
    const bool needsTextureSample = uses_texture_sample(stage);
    if (!needsTevTexCoord && !needsTextureSample) {
      continue;
    }
    const bool hasBaseTexCoord = stage.texCoordId != GX_TEXCOORD_NULL;
    const bool hasBaseTexture = stage.texMapId != GX_TEXMAP_NULL;
    const bool hasBaseCoord = hasBaseTexCoord && hasBaseTexture;
    std::string uvIn;
    if (needsTevTexCoord) {
      fsPre += fmt::format("\n    // TEV stage {} indirect", i);
      std::string indirectOffsetTexel;
      if (needsIndirectCoord && hasIndirectStage) {
        std::string_view fmtShift;
        switch (stage.indTexFormat) {
        case GX_ITF_8:
          break;
        case GX_ITF_5:
          fmtShift = " / 8.0"sv;
          break;
        case GX_ITF_4:
          fmtShift = " / 16.0"sv;
          break;
        case GX_ITF_3:
          fmtShift = " / 32.0"sv;
          break;
        default:
          FATAL("unhandled indirect format {}", underlying(stage.indTexFormat));
        }
        if (fmtShift.empty()) {
          fsPre += fmt::format("\n    vec3 ind{0}_coord = t_IndTexCoord{1};", i, underlying(stage.indTexStage));
        } else {
          fsPre += fmt::format("\n    vec3 ind{0}_coord = floor(t_IndTexCoord{1}{2});", i,
                               underlying(stage.indTexStage), fmtShift);
        }
        if (stage.indTexBiasSel != GX_ITB_NONE) {
          auto bias = stage.indTexFormat == GX_ITF_8 ? "-128.0"sv : "1.0"sv;
          auto biasS = "0.0"sv, biasT = "0.0"sv, biasU = "0.0"sv;
          if (stage.indTexBiasSel == GX_ITB_S || stage.indTexBiasSel == GX_ITB_ST || stage.indTexBiasSel == GX_ITB_SU ||
              stage.indTexBiasSel == GX_ITB_STU) {
            biasS = "1.0"sv;
          }
          if (stage.indTexBiasSel == GX_ITB_T || stage.indTexBiasSel == GX_ITB_ST || stage.indTexBiasSel == GX_ITB_TU ||
              stage.indTexBiasSel == GX_ITB_STU) {
            biasT = "1.0"sv;
          }
          if (stage.indTexBiasSel == GX_ITB_U || stage.indTexBiasSel == GX_ITB_SU || stage.indTexBiasSel == GX_ITB_TU ||
              stage.indTexBiasSel == GX_ITB_STU) {
            biasU = "1.0"sv;
          }
          fsPre += fmt::format("\n    ind{0}_coord = ind{0}_coord + vec3({1}, {2}, {3}) * {4};", i, biasS, biasT, biasU,
                               bias);
        }
        if (stage.indTexMtxId >= GX_ITM_0 && stage.indTexMtxId <= GX_ITM_2) {
          u32 mtxIdx = stage.indTexMtxId - GX_ITM_0;
          fsPre += fmt::format(
              "\n    vec4 ind{0}_c0 = ubuf.ind_mtx[{1}][0];"
              "\n    vec4 ind{0}_c1 = ubuf.ind_mtx[{1}][1];",
              i, mtxIdx);
          indirectOffsetTexel = fmt::format(
              "vec2("
              "dot(vec3(ind{0}_c0.xz, ind{0}_c1.x), ind{0}_coord), "
              "dot(vec3(ind{0}_c0.yw, ind{0}_c1.y), ind{0}_coord)"
              ") * ind{0}_c1.z",
              i);
        } else if (stage.indTexMtxId >= GX_ITM_S0 && stage.indTexMtxId <= GX_ITM_S2 && hasBaseCoord) {
          u32 mtxIdx = stage.indTexMtxId - GX_ITM_S0;
          u32 regTexCoord = underlying(stage.texCoordId);
          indirectOffsetTexel = fmt::format(
              "tex{1}_uv * ubuf.texcoord_scale[{1}].xy * ind{0}_coord.x"
              " * ubuf.ind_mtx[{2}][1][2] / 256.0",
              i, regTexCoord, mtxIdx);
        } else if (stage.indTexMtxId >= GX_ITM_T0 && stage.indTexMtxId <= GX_ITM_T2 && hasBaseCoord) {
          u32 mtxIdx = stage.indTexMtxId - GX_ITM_T0;
          u32 regTexCoord = underlying(stage.texCoordId);
          indirectOffsetTexel = fmt::format(
              "tex{1}_uv * ubuf.texcoord_scale[{1}].xy * ind{0}_coord.y"
              " * ubuf.ind_mtx[{2}][1][2] / 256.0",
              i, regTexCoord, mtxIdx);
        }
      }
      const bool useSimpleCoords = stage.indTexMtxId == GX_ITM_OFF && !stage.indTexAddPrev;
      // GLSL float `%` -> mod().
      auto wrap_comp = [](GXIndTexWrap wrap, std::string&& coord) -> std::string {
        switch (wrap) {
        case GX_ITW_OFF:
          return std::move(coord);
        case GX_ITW_256:
          return fmt::format("mod({}, 256.0)", coord);
        case GX_ITW_128:
          return fmt::format("mod({}, 128.0)", coord);
        case GX_ITW_64:
          return fmt::format("mod({}, 64.0)", coord);
        case GX_ITW_32:
          return fmt::format("mod({}, 32.0)", coord);
        case GX_ITW_16:
          return fmt::format("mod({}, 16.0)", coord);
        case GX_ITW_0:
          return "0.0";
        default:
          FATAL("unhandled indirect wrap {}", underlying(wrap));
        }
      };
      std::string baseCoordExpr;
      if (hasBaseCoord) {
        u32 texCoordId = underlying(stage.texCoordId);
        if (useSimpleCoords) {
          baseCoordExpr = fmt::format("tex{}_uv", texCoordId);
        } else {
          fsPre += fmt::format("\n    vec2 ind{0}_texel = tex{1}_uv * ubuf.texcoord_scale[{1}].xy;", i, texCoordId);
          baseCoordExpr = fmt::format("ind{}_texel", i);
        }
      }
      std::string wrappedExpr = baseCoordExpr;
      if (!baseCoordExpr.empty() && (stage.indTexWrapS != GX_ITW_OFF || stage.indTexWrapT != GX_ITW_OFF)) {
        wrappedExpr = fmt::format("vec2({}, {})", wrap_comp(stage.indTexWrapS, fmt::format("{}.x", baseCoordExpr)),
                                  wrap_comp(stage.indTexWrapT, fmt::format("{}.y", baseCoordExpr)));
      }
      std::string finalCoord;
      if (!wrappedExpr.empty() && !indirectOffsetTexel.empty()) {
        finalCoord = fmt::format("{} + ({})", wrappedExpr, indirectOffsetTexel);
      } else if (!wrappedExpr.empty()) {
        finalCoord = wrappedExpr;
      } else {
        finalCoord = indirectOffsetTexel;
      }
      if (info.usedIndStages.any() && !finalCoord.empty()) {
        if (stage.indTexAddPrev) {
          fsPre += fmt::format("\n    t_TexCoord += {};", finalCoord);
        } else {
          fsPre += fmt::format("\n    t_TexCoord = {};", finalCoord);
        }
        if (needsTextureSample && hasBaseTexture) {
          u32 texMapId = underlying(stage.texMapId);
          if (useSimpleCoords) {
            fsPre += fmt::format("\n    vec2 ind{0}_uv = t_TexCoord;", i);
          } else {
            fsPre += fmt::format("\n    vec2 ind{0}_uv = t_TexCoord / ubuf.tex{1}_size_bias.xy;", i, texMapId);
          }
          uvIn = fmt::format("ind{0}_uv", i);
        }
      }
    }
    if (!needsTextureSample) {
      continue;
    }
    CHECK(stage.texMapId != GX_TEXMAP_NULL, "unmapped texture for stage {}", i);
    CHECK(stage.texCoordId != GX_TEXCOORD_NULL, "unmapped texcoord for stage {}", i);
    if (uvIn.empty()) {
      uvIn = fmt::format("tex{0}_uv", underlying(stage.texCoordId));
    }
    fsPre += fmt::format("\n    vec4 sampled{0} = texture(tex{1}, {2}, ubuf.tex{1}_size_bias.z);", i,
                         underlying(stage.texMapId), uvIn);
  }

  if (info.usesPTTexMtx.any()) {
    uniformFields += fmt::format("\n    mat3x4 postmtx[{}];", MaxPTTexMtx);
  }
  if (info.usesFog) {
    structDefs +=
        "\nstruct Fog {\n"
        "    vec4 color;\n"
        "    float a;\n"
        "    float b;\n"
        "    float c;\n"
        "    float pad;\n"
        "};\n";
    uniformFields += "\n    Fog fog;";
    fsBody +=
        fmt::format("\n    // Fog\n    float fogF = clamp((ubuf.fog.a / (ubuf.fog.b - {})) - ubuf.fog.c, 0.0, 1.0);",
                    UseReversedZ ? "(1.0 - gl_FragCoord.z)" : "gl_FragCoord.z");
    switch (config.fogType) {
      DEFAULT_FATAL("invalid fog type {}", config.fogType);
    case GX_FOG_PERSP_LIN:
    case GX_FOG_ORTHO_LIN:
      fsBody += "\n    float fogZ = fogF;";
      break;
    case GX_FOG_PERSP_EXP:
    case GX_FOG_ORTHO_EXP:
      fsBody += "\n    float fogZ = 1.0 - exp2(-8.0 * fogF);";
      break;
    case GX_FOG_PERSP_EXP2:
    case GX_FOG_ORTHO_EXP2:
      fsBody += "\n    float fogZ = 1.0 - exp2(-8.0 * fogF * fogF);";
      break;
    case GX_FOG_PERSP_REVEXP:
    case GX_FOG_ORTHO_REVEXP:
      fsBody += "\n    float fogZ = exp2(-8.0 * (1.0 - fogF));";
      break;
    case GX_FOG_PERSP_REVEXP2:
    case GX_FOG_ORTHO_REVEXP2:
      fsBody +=
          "\n    fogF = 1.0 - fogF;"
          "\n    float fogZ = exp2(-8.0 * fogF * fogF);";
      break;
    }
    fsBody += "\n    prev = vec4(mix(prev.rgb, ubuf.fog.color.rgb, clamp(fogZ, 0.0, 1.0)), prev.a);";
  }
  uniformFields += fmt::format("\n    vec4 texcoord_scale[{}];", MaxTexCoord);
  if (info.usedIndTexMtxs.any()) {
    uniformFields += "\n    mat2x4 ind_mtx[3];";
  }
  for (int i = 0; i < info.sampledTextures.size(); ++i) {
    if (!info.sampledTextures.test(i)) {
      continue;
    }
    uniformFields += fmt::format("\n    vec4 tex{}_size_bias;", i);
    samplerDecls += fmt::format("\nuniform sampler2D tex{};", i);
  }
  fsBody += "\n    prev = tev_overflow_vec4f(prev);";
  if (config.alphaCompare) {
    const auto comp0 = alpha_compare(config.alphaCompare.comp0, config.alphaCompare.ref0);
    const auto comp1 = alpha_compare(config.alphaCompare.comp1, config.alphaCompare.ref1);
    AlphaCompareExpr pass;
    switch (config.alphaCompare.op) {
      DEFAULT_FATAL("invalid alpha compare op {}", underlying(config.alphaCompare.op));
    case GX_AOP_AND:
      pass = alpha_compare_and(comp0, comp1);
      break;
    case GX_AOP_OR:
      pass = alpha_compare_or(comp0, comp1);
      break;
    case GX_AOP_XOR:
      pass = alpha_compare_xor(comp0, comp1);
      break;
    case GX_AOP_XNOR:
      pass = alpha_compare_xnor(comp0, comp1);
      break;
    }
    const auto discard = alpha_compare_not(pass);
    if (discard.constant == 1) {
      fsBody += "\n    // Alpha compare\n    discard;";
    } else if (discard.constant != 0) {
      fsBody +=
          "\n    // Alpha compare"
          "\n    uint alphaCompare = uint(round(clamp(prev.a, 0.0, 1.0) * 255.0));";
      fsBody += fmt::format("\n    if ({}) {{ discard; }}", discard.expr);
    }
  }

  // ---- Assemble the two `#version 300 es` sources ----
  // precision highp is mandatory: TEV's integer-compare reconstruction needs fp32
  // (mediump is fp16 on PowerVR and would corrupt it).
  static constexpr std::string_view kHeader = "#version 300 es\nprecision highp float;\nprecision highp int;\n";
  static constexpr std::string_view kOverflow =
      "\nfloat tev_overflow_f32(float v) {\n"
      "  float byte_space = v * 255.0;\n"
      "  return (byte_space - floor(byte_space / 256.0) * 256.0) / 255.0;\n"
      "}\n"
      "vec3 tev_overflow_vec3f(vec3 v) {\n"
      "  vec3 byte_space = v * 255.0;\n"
      "  return (byte_space - floor(byte_space / 256.0) * 256.0) / 255.0;\n"
      "}\n"
      "vec4 tev_overflow_vec4f(vec4 v) {\n"
      "  vec4 byte_space = v * 255.0;\n"
      "  return (byte_space - floor(byte_space / 256.0) * 256.0) / 255.0;\n"
      "}\n";

  std::string uniformBlock = fmt::format(
      "\nlayout(std140) uniform Uniform {{"
      "\n    uint vtx_start;"
      "\n    uint current_pnmtx;"
      "\n    vec2 render_viewport_size;"
      "\n    vec2 logical_viewport_size;"
      "\n    uvec2 pad;"
      "\n    uvec4 array_start[3];{}"
      "\n}} ubuf;\n",
      uniformFields);

  GlslProgram out;
  out.vertex = std::string(kHeader) + structDefs + uniformBlock + vertexInputs + varyingsOut +
               "\n\nvoid main() {" + vsBody + "\n}\n";
  out.fragment = std::string(kHeader) + std::string(kOverflow) + structDefs + uniformBlock + samplerDecls + varyingsIn +
                 "\n\nout vec4 out_color;\n\nvoid main() {" + fsPre + fsBody + "\n    out_color = prev;\n}\n";
  glslify_vocab(out.vertex);
  glslify_vocab(out.fragment);
  return out;
}
} // namespace

std::string build_shader_source(const ShaderConfig& config) noexcept {
  const auto program = emit_glsl(config, build_shader_info(config));
  return "// ---- vertex ----\n" + program.vertex + "\n// ---- fragment ----\n" + program.fragment;
}

uint32_t build_shader(const ShaderConfig& config) noexcept {
  ZoneScoped;
  const auto hash = xxh3_hash(config);
  {
    std::lock_guard lock{g_programCacheMutex};
    const auto it = g_programByShaderHash.find(hash);
    if (it != g_programByShaderHash.end()) {
      return it->second;
    }
  }
  const auto info = build_shader_info(config);
  const auto program = emit_glsl(config, info);
  if (EnableDebugPrints && !g_seenShaders.contains(hash)) {
    g_seenShaders.insert(hash);
    Log.info("Generated GLSL (hash {:x})\n// vertex\n{}\n// fragment\n{}", hash, program.vertex, program.fragment);
  }
  const auto label = fmt::format("GX shader {:x}", hash);
  const auto gl_program = gl::compile_program(program.vertex.c_str(), program.fragment.c_str(), label.c_str());
  if (gl_program == 0) {
    return 0;
  }
  gl::configure_gx_program(gl_program, info.uniformSize);
  {
    std::lock_guard lock{g_programCacheMutex};
    g_programByShaderHash.emplace(hash, gl_program);
  }
  return gl_program;
}

size_t shader_program_count() noexcept {
  std::lock_guard lock{g_programCacheMutex};
  return g_programByShaderHash.size();
}

void clear_shader_program_cache() noexcept {
  std::lock_guard lock{g_programCacheMutex};
  g_programByShaderHash.clear();
}
} // namespace aurora::gx

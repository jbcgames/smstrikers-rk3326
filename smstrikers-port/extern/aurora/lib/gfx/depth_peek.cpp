#include "depth_peek.hpp"

#include "../dolphin/vi/vi_internal.hpp"
#include "../gx/gx.hpp"
#include "../gfx/render_worker.hpp"
#include "../webgpu/gpu.hpp"
#include "../webgpu/gpu_prof.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <mutex>
#include <string>

#include <magic_enum.hpp>
#include <tracy/Tracy.hpp>

namespace aurora::gfx::depth_peek {
namespace {
Module Log("aurora::gfx::depth_peek");

using Clock = std::chrono::steady_clock;

constexpr size_t SlotCount = 3;
constexpr uint32_t WorkgroupSizeX = 8;
constexpr uint32_t WorkgroupSizeY = 8;
constexpr uint32_t DepthPeekSnapshotHz = 30;
constexpr auto SnapshotInterval = std::chrono::nanoseconds{1'000'000'000 / DepthPeekSnapshotHz};

struct Params {
  uint32_t dstWidth = 0;
  uint32_t dstHeight = 0;
  uint32_t srcWidth = 0;
  uint32_t srcHeight = 0;
  float offsetX = 0.f;
  float offsetY = 0.f;
  float scaleX = 1.f;
  float scaleY = 1.f;
};
static_assert(sizeof(Params) == 32);

struct LatestSnapshot {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint32_t> data;
};

enum class SlotState : uint8_t {
  Available,
  CopySubmitted,
  MapPending,
};

struct Slot {
  gl::Buffer storageBuffer;
  gl::Buffer readbackBuffer;
  gl::Buffer paramsBuffer;
  uint32_t width = 0;
  uint32_t height = 0;
  uint64_t byteSize = 0;
  SlotState state = SlotState::Available;
};

bool g_enabled = false;
std::array<Slot, SlotCount> g_slots;
size_t g_nextSlot = 0;
bool g_snapshotRequested = false;
Clock::time_point g_nextSnapshotTime;
LatestSnapshot g_latest;
std::mutex g_mutex;

// WGSL kept as dead source for the Phase 2+ depth-readback compute pass. Phase 1
// has no compute path (device parity: depth peek absent), so it is unused.
constexpr std::string_view ShaderPreamble = R"(
struct Params {
    dstSize: vec2u,
    srcSize: vec2u,
    offset: vec2f,
    scale: vec2f,
};

@group(0) @binding(1) var<storage, read_write> out_z: array<u32>;
@group(0) @binding(2) var<uniform> params: Params;
)"sv;

constexpr std::string_view ReversedZBody = R"(
fn gx_z24(depth: f32) -> u32 {
    return min(u32(clamp(1.0 - depth, 0.0, 1.0) * 16777215.0 + 0.5), 0x00ffffffu);
}
)"sv;

constexpr std::string_view ForwardZBody = R"(
fn gx_z24(depth: f32) -> u32 {
    return min(u32(clamp(depth, 0.0, 1.0) * 16777215.0 + 0.5), 0x00ffffffu);
}
)"sv;

constexpr std::string_view ShaderMain = R"(
@group(0) @binding(0) var src: texture_depth_2d;

fn load_depth(coord: vec2i) -> f32 {
    return textureLoad(src, coord, 0);
}

@compute @workgroup_size(8, 8, 1)
fn cs_main(@builtin(global_invocation_id) id: vec3u) {
    if (id.x >= params.dstSize.x || id.y >= params.dstSize.y) {
        return;
    }

    let dstCenter = vec2f(vec2u(id.xy)) + vec2f(0.5, 0.5);
    let srcPixel = clamp(vec2i(floor(params.offset + dstCenter * params.scale)), vec2i(0, 0),
                         vec2i(params.srcSize) - vec2i(1, 1));
    let depth = load_depth(srcPixel);
    out_z[id.y * params.dstSize.x + id.x] = gx_z24(depth);
}
)"sv;

[[maybe_unused]] std::string build_shader_source() {
  std::string source;
  source.reserve(ShaderPreamble.size() + ReversedZBody.size() + ShaderMain.size());
  source += ShaderPreamble;
  source += gx::UseReversedZ ? ReversedZBody : ForwardZBody;
  source += ShaderMain;
  return source;
}

Params make_params(gl::Extent3D sourceSize, Vec2<uint32_t> dstSize) noexcept {
  Params params{
      .dstWidth = dstSize.x,
      .dstHeight = dstSize.y,
      .srcWidth = sourceSize.width,
      .srcHeight = sourceSize.height,
  };

  if (gx::g_gxState.viewportPolicy == AURORA_VIEWPORT_NATIVE) {
    return params;
  }

  const auto logicalSize = vi::configured_fb_size();
  if (logicalSize.x == 0 || logicalSize.y == 0 || sourceSize.width == 0 || sourceSize.height == 0) {
    return params;
  }

  const bool stretch = gx::g_gxState.viewportPolicy == AURORA_VIEWPORT_STRETCH;
  const float scaleX = static_cast<float>(sourceSize.width) / static_cast<float>(logicalSize.x);
  const float scaleY = static_cast<float>(sourceSize.height) / static_cast<float>(logicalSize.y);
  const float scale = std::min(scaleX, scaleY);
  params.scaleX = stretch ? scaleX : scale;
  params.scaleY = stretch ? scaleY : scale;
  params.offsetX =
      stretch ? 0.f : (static_cast<float>(sourceSize.width) - static_cast<float>(logicalSize.x) * scale) * 0.5f;
  params.offsetY =
      stretch ? 0.f : (static_cast<float>(sourceSize.height) - static_cast<float>(logicalSize.y) * scale) * 0.5f;
  return params;
}

bool ensure_slot(Slot& slot, uint32_t width, uint32_t height) {
  const uint64_t byteSize = static_cast<uint64_t>(width) * static_cast<uint64_t>(height) * sizeof(uint32_t);
  if (slot.storageBuffer && slot.width == width && slot.height == height && slot.byteSize == byteSize) {
    return true;
  }

  slot.storageBuffer = {};
  slot.readbackBuffer = {};
  slot.paramsBuffer = {};
  slot.width = width;
  slot.height = height;
  slot.byteSize = byteSize;

  if (byteSize == 0 || byteSize > UINT32_MAX) {
    return false;
  }

  // Phase 1 stub (device parity: depth peek absent)
  // Phase 2+: allocate the GL storage/readback/uniform buffers here (was three
  // wgpu CreateBuffer calls: Storage|CopySrc, MapRead|CopyDst, Uniform|CopyDst).
  // With no GPU buffers there is no slot to fill, so no snapshot is produced.
  return false;
}

[[maybe_unused]] Slot* find_available_slot(uint32_t width, uint32_t height) {
  for (size_t i = 0; i < g_slots.size(); ++i) {
    const size_t idx = (g_nextSlot + i) % g_slots.size();
    auto& slot = g_slots[idx];
    if (slot.state != SlotState::Available) {
      continue;
    }
    if (!ensure_slot(slot, width, height)) {
      continue;
    }
    g_nextSlot = (idx + 1) % g_slots.size();
    return &slot;
  }
  return nullptr;
}
} // namespace

void initialize() {
  // Phase 1 stub (device parity: depth peek absent)
  // Phase 2+: create the GX PeekZ compute pipeline + bind group layout. Compute is
  // unavailable on the GLES compat path, so depth peek stays disabled and
  // read_latest() reports no snapshot.
  g_enabled = false;
}

void shutdown() {
  testing::reset();
  for (auto& slot : g_slots) {
    slot = {};
  }
}

void request_snapshot() noexcept {
  if (!g_enabled) {
    return;
  }
  std::lock_guard lock{g_mutex};
  g_snapshotRequested = true;
}

bool read_latest(uint16_t x, uint16_t y, uint32_t& z) noexcept {
  std::lock_guard lock{g_mutex};
  if (x >= g_latest.width || y >= g_latest.height || g_latest.data.empty()) {
    return false;
  }
  z = g_latest.data[static_cast<size_t>(y) * g_latest.width + x] & 0x00ffffffu;
  return true;
}

void encode_frame_snapshot(const gl::Texture& depthView, gl::Extent3D sourceSize, uint32_t msaaSamples) noexcept {
  if (!g_enabled) {
    return;
  }

  ZoneScoped;
  const auto now = Clock::now();
  {
    std::lock_guard lock{g_mutex};
    if (!g_snapshotRequested || now < g_nextSnapshotTime) {
      return;
    }
    g_snapshotRequested = false;
    g_nextSnapshotTime = now + SnapshotInterval;
  }

  const auto dstSize = vi::configured_fb_size();
  if (!depthView || dstSize.x == 0 || dstSize.y == 0 || sourceSize.width == 0 || sourceSize.height == 0) {
    return;
  }
  if (msaaSamples > 1) {
    Log.fatal("Depth Peek from multisampled EFB targets is not supported");
  }

  const Params params = make_params(sourceSize, dstSize);
  (void)params;

  // Phase 1 stub (device parity: depth peek absent)
  // Phase 2+: WriteBuffer(params) -> bind the depth texture + storage + params ->
  // dispatch the GX PeekZ compute shader ((dst + 7) / 8 workgroups) ->
  // CopyBufferToBuffer(storage -> readback). Compute is unavailable on the GLES
  // compat path, so nothing is encoded and no snapshot is produced.
  (void)WorkgroupSizeX;
  (void)WorkgroupSizeY;
}

void after_submit() noexcept {
  if (!g_enabled) {
    return;
  }

  // Phase 1 stub (device parity: depth peek absent)
  // Phase 2+: map each CopySubmitted slot's readback buffer and publish the mapped
  // depth values into g_latest (was wgpu MapAsync + GetConstMappedRange). With no
  // GPU copy in flight there is nothing to map; g_latest stays empty so read_latest
  // returns false, which callers tolerate.
  std::lock_guard lock{g_mutex};
  for (auto& slot : g_slots) {
    if (slot.state == SlotState::CopySubmitted || slot.state == SlotState::MapPending) {
      slot.state = SlotState::Available;
    }
  }
}

namespace testing {
void reset() noexcept {
  std::lock_guard lock{g_mutex};
  g_snapshotRequested = false;
  g_nextSlot = 0;
  g_nextSnapshotTime = {};
  g_latest = {};
  for (auto& slot : g_slots) {
    slot.state = SlotState::Available;
  }
}

bool snapshot_requested() noexcept {
  std::lock_guard lock{g_mutex};
  return g_snapshotRequested;
}

void set_latest(uint32_t width, uint32_t height, const std::vector<uint32_t>& data) {
  std::lock_guard lock{g_mutex};
  g_latest.width = width;
  g_latest.height = height;
  g_latest.data = data;
}
} // namespace testing

} // namespace aurora::gfx::depth_peek

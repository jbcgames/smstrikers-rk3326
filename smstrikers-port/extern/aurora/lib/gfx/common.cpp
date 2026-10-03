#include "common.hpp"

#include "clear.hpp"
#include "depth_peek.hpp"
#include "../internal.hpp"
#include "../gl/binary_cache.hpp"
#include "../gl/census.hpp"
#include "../gl/context.hpp"
#include "../gl/fbo_cache.hpp"
#include "../gl/state.hpp"
#include "../gl/textures.hpp"
#include "../webgpu/gpu.hpp"
#include "../webgpu/gpu_prof.hpp"
#include "../webgpu/sdl2shim_present.hpp"
#include "../gx/pipeline.hpp"
#ifdef AURORA_ENABLE_RMLUI
#include "../rmlui/pipeline.hpp"
#endif
#include "pipeline_cache.hpp"
#include "render_worker.hpp"
#include "tex_copy_conv.hpp"
#include "tex_palette_conv.hpp"
#include "texture_replacement.hpp"
#include "texture.hpp"
#include "../window.hpp"
#include "../gx/fifo.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <thread>

#include <absl/container/flat_hash_map.h>
#include <magic_enum.hpp>

#include "tracy/Tracy.hpp"

namespace aurora::gfx {
static Module Log("aurora::gfx");

namespace gl = aurora::gl;

#ifdef AURORA_GFX_DEBUG_GROUPS
std::vector<std::string> g_debugGroupStack;
std::vector<std::string> g_debugMarkers;
#endif

static std::string pass_label(std::string_view kind) {
#ifdef AURORA_GFX_DEBUG_GROUPS
  if (!g_debugGroupStack.empty()) {
    return fmt::format("{} ({})", kind, g_debugGroupStack.back());
  }
#endif
  return std::string{kind};
}

constexpr size_t FrameSlotCount = 2;

struct StagingHighWater {
  uint32_t verts = 0;
  uint32_t uniforms = 0;
  uint32_t indices = 0;
  uint32_t storage = 0;
  uint32_t textureUpload = 0;
  size_t textureUploadCount = 0;
};

struct CustomDrawCommand {
  DrawTypeId type;
  uint32_t payloadSize;
  std::array<std::byte, InlineDrawPayloadSize> payload;
};

struct ShaderDrawCommand {
  ShaderType type;
  union {
    clear::DrawData clear;
    gx::DrawData gx;
#ifdef AURORA_ENABLE_RMLUI
    rmlui::DrawData rml;
#endif
  };
};
enum class CommandType {
  SetViewport,
  SetScissor,
  Draw,
  CustomDraw,
  DebugMarker,
};
struct Command {
  CommandType type;
#ifdef AURORA_GFX_DEBUG_GROUPS
  std::vector<std::string> debugGroupStack;
#endif
  union Data {
    Viewport setViewport;
    ClipRect setScissor;
    ShaderDrawCommand draw;
    CustomDrawCommand customDraw;
    size_t debugMarkerIndex;
  } data;
};
} // namespace aurora::gfx

namespace aurora::gfx {
namespace {
struct CachedBindGroup {
  gl::BindingSet bindGroup;
  uint32_t lastUsedFrame = 0;
};

struct RuntimeDrawType {
  std::string label;
  DrawCallback draw = nullptr;
  void* userdata = nullptr;
  uint32_t generation = 1;
};

struct RuntimeEncoderTaskType {
  std::string label;
  EncoderTaskCallback callback = nullptr;
  void* userdata = nullptr;
  uint32_t generation = 1;
};

constexpr uint32_t draw_type_index(DrawTypeId id) { return static_cast<uint32_t>(id & 0xFFFFFFFFu); }
constexpr uint32_t draw_type_generation(DrawTypeId id) { return static_cast<uint32_t>(id >> 32); }
constexpr DrawTypeId make_draw_type_id(uint32_t index, uint32_t generation) {
  return static_cast<DrawTypeId>(generation) << 32 | index;
}

constexpr uint32_t BindGroupCacheRetainFrames = 32;
constexpr uint32_t BindGroupCacheSweepPeriod = 16;
} // namespace

static absl::flat_hash_map<BindGroupRef, CachedBindGroup> g_cachedBindGroups;
static absl::flat_hash_map<SamplerRef, gl::Sampler> g_cachedSamplers;
static std::vector<RuntimeDrawType> g_runtimeDrawTypes;
static std::vector<uint32_t> g_freeDrawTypeSlots;
static std::vector<RuntimeEncoderTaskType> g_runtimeEncoderTaskTypes;
static std::vector<uint32_t> g_freeEncoderTaskTypeSlots;
static std::mutex g_bindGroupCacheMutex;
static std::mutex g_samplerCacheMutex;
static std::mutex g_runtimeDrawTypeMutex;

// Requires g_runtimeDrawTypeMutex held.
static const RuntimeDrawType* find_runtime_draw_type(DrawTypeId id) {
  const auto idx = draw_type_index(id);
  if (id == InvalidDrawType || idx >= g_runtimeDrawTypes.size()) {
    return nullptr;
  }
  const auto& slot = g_runtimeDrawTypes[idx];
  if (slot.generation != draw_type_generation(id) || slot.draw == nullptr) {
    return nullptr;
  }
  return &slot;
}

// Requires g_runtimeDrawTypeMutex held.
static const RuntimeEncoderTaskType* find_runtime_encoder_task_type(EncoderTaskId id) {
  const auto idx = draw_type_index(id);
  if (id == InvalidEncoderTask || idx >= g_runtimeEncoderTaskTypes.size()) {
    return nullptr;
  }
  const auto& slot = g_runtimeEncoderTaskTypes[idx];
  if (slot.generation != draw_type_generation(id) || slot.callback == nullptr) {
    return nullptr;
  }
  return &slot;
}

// The "active" per-frame ring buffers -- worker-only aliases pointed at the current frame slot's
// FrameBufferSet by select_frame_buffers() each frame. Every draw/upload path reads these, so the
// double-buffering is invisible below this point.
gl::Buffer g_vertexBuffer;
gl::Buffer g_uniformBuffer;
gl::Buffer g_indexBuffer;
gl::Buffer g_storageBuffer;

// One owned set of per-frame ring buffers per frame slot. The per-frame vertex/uniform/index data is
// rewritten from offset 0 every frame; with persistent-mapped (immutable) storage there is no
// glBufferSubData implicit-sync to keep the worker from memcpy'ing over a range the GPU still reads,
// so each slot gets its OWN set (double-buffering) and `fence` gates reuse: the worker waits slot
// s's prior-frame fence before selecting the set for a new frame, guaranteeing the GPU finished
// reading it. On the glBufferSubData fallback (no GL_EXT_buffer_storage) the fence is a harmless
// extra ordering point. Created in initialize(), torn down in shutdown(); worker-owned throughout.
struct FrameBufferSet {
  gl::Buffer vertex;
  gl::Buffer uniform;
  gl::Buffer index;
  gl::GLsync fence = nullptr; // GPU-done marker for this set's last frame; waited before reuse
};
static std::array<FrameBufferSet, FrameSlotCount> g_frameBufferSets;

// Point the active per-frame ring aliases at slot `slot`'s owned buffer set. Worker thread only.
static void select_frame_buffers(size_t slot) {
  auto& set = g_frameBufferSets[slot];
  g_vertexBuffer = set.vertex;
  g_uniformBuffer = set.uniform;
  g_indexBuffer = set.index;
}

// Block the worker until the GPU finished the frame that last used slot `slot`'s buffer set, so its
// persistent mappings are safe to overwrite. Near-zero in steady state (fence already signalled);
// natural backpressure if the GPU falls behind. Deletes the fence. Worker thread only.
static void wait_frame_buffer_fence(size_t slot) {
  auto& set = g_frameBufferSets[slot];
  if (set.fence == nullptr || gl::gl.ClientWaitSync == nullptr) {
    return;
  }
  // Flush on the first wait so the fence is guaranteed to be in the GPU pipe (cannot self-deadlock
  // on unsubmitted commands). Bounded chunks with a diagnostic rather than an unbounded block.
  constexpr uint64_t kChunkNs = 1'000'000'000ull; // 1 s
  for (int attempt = 0; attempt < 5; ++attempt) {
    const gl::GLenum r = gl::gl.ClientWaitSync(set.fence, gl::GL_SYNC_FLUSH_COMMANDS_BIT, kChunkNs);
    if (r == gl::GL_ALREADY_SIGNALED || r == gl::GL_CONDITION_SATISFIED) {
      break;
    }
    if (r == gl::GL_WAIT_FAILED) {
      Log.warn("frame-buffer fence wait failed for slot {}", slot);
      break;
    }
    Log.warn("frame-buffer fence for slot {} still pending after {}s", slot, attempt + 1);
  }
  gl::gl.DeleteSync(set.fence);
  set.fence = nullptr;
}
// Persistent caches for CPU-expanded native-fetch geometry. Separate from the per-frame
// staging rings so cached ranges can never be clobbered by ring uploads. Filled once per
// unique mesh (by content hash) and referenced every subsequent frame.
gl::Buffer g_nativeVertexCacheBuffer;
gl::Buffer g_nativeIndexCacheBuffer;
static constexpr uint64_t NativeGeomVertexCacheSize = 24ull * 1024 * 1024;
static constexpr uint64_t NativeGeomIndexCacheSize = 4ull * 1024 * 1024;
// Bump-allocator cursors; on exhaustion the whole cache is reset (deferred to a frame
// boundary). The generation counter bumps on each reset so CPU-side content maps can
// drop entries that point at now-recycled ranges.
static uint32_t s_nativeVertexCacheOffset = 0;
static uint32_t s_nativeIndexCacheOffset = 0;
static uint32_t s_nativeGeomCacheGeneration = 0;
static bool s_nativeGeomCacheResetPending = false;
// Phase 1: the WebGPU staging-buffer + MapAsync machinery is gone. Frame data is
// collected into plain owned ByteBuffers and (from Phase 2) uploaded with
// glBufferSubData on the worker. No mapping states, no staging slot pool.
static uint32_t g_frameIndex = UINT32_MAX;
static PipelineRef g_currentPipeline;

// for imgui debug
AuroraStats g_stats{};
uint32_t g_drawCallCount = 0;
uint32_t g_mergedDrawCallCount = 0;

using CommandList = std::vector<Command>;
struct RenderPass {
  std::string label;
  gl::Texture colorView;
  gl::Texture resolveView; // MSAA resolve target; null if msaaSamples == 1
  gl::Texture depthStencilView;
  gl::Texture copySourceTexture;
  gl::Texture copySourceView;
  gl::Texture copySourceDepthView;
  gl::Extent3D targetSize;
  uint32_t msaaSamples = 1;

  TextureHandle resolveTarget;
  GXTexFmt resolveFormat = GX_TF_RGBA8;
  ClipRect resolveRect;
  Range resolveUniformRange;
  // Full-target snapshots for the public resolve_pass API
  gl::Texture snapshotColorDst;
  gl::Texture snapshotDepthDst;
  Vec4<float> clearColorValue{0.f, 0.f, 0.f, 0.f};
  float clearDepthValue = gx::UseReversedZ ? 0.f : 1.f;
  gl::LoadOp colorLoadOp = gl::LoadOp::Undefined;
  gl::StoreOp colorStoreOp = gl::StoreOp::Store;
  gl::LoadOp depthLoadOp = gl::LoadOp::Undefined;
  gl::StoreOp depthStoreOp = gl::StoreOp::Store;
  gl::LoadOp stencilLoadOp = gl::LoadOp::Undefined;
  gl::StoreOp stencilStoreOp = gl::StoreOp::Undefined;
  uint32_t stencilClearValue = 0;
  CommandList commands;
  bool clearColor = true;
  bool clearDepth = true;
  bool hasDepth = true;
  bool hasStencil = false;
  bool depthReadOnly = false;
  bool hasDraws = false;
  bool discardable = false;
  bool captureDepthSnapshot = false;
  bool sealed = false;
  std::vector<tex_palette_conv::ConvRequest> paletteConvs;

  // Something copies this pass's output after it ends: a GX resolve or resolve_pass snapshots.
  bool has_consumer() const { return resolveTarget || snapshotColorDst || snapshotDepthDst; }
  // The pass mutates its attachments: draws or pending clears.
  bool has_content() const { return hasDraws || clearColor || clearDepth; }
};

struct TextureCopy {
  TextureCopyView src;
  TextureCopyView dst;
  gl::Extent3D size;
  // The RmlUi SaveLayerAsTexture path passes a top-left source origin into a GL (bottom-left)
  // layer target and wants the result stored upright for later top-left geometry sampling; this
  // converts the source band and vertically flips the blit. Full-target snapshots leave it off.
  bool flipY = false;
};

struct EncoderTask {
  EncoderTaskId type = InvalidEncoderTask;
  std::array<uint8_t, InlineDrawPayloadSize> payload{};
  uint32_t payloadSize = 0;
};

enum class FrameOpType : uint8_t {
  RenderPass,
  TextureCopy,
  EncoderTask,
};

struct FrameOp {
  FrameOpType type = FrameOpType::RenderPass;
  uint32_t index = 0;
  RenderPass* renderPass = nullptr;
  TextureCopy* textureCopy = nullptr;
  EncoderTask* encoderTask = nullptr;
  StagingHighWater highWater;
  std::vector<const TextureUpload*> textureUploads;
};

using RenderPassList = std::deque<RenderPass>;
struct FramePacket {
  RenderPassList renderPasses;
  std::deque<TextureCopy> textureCopies;
  std::deque<EncoderTask> encoderTasks;
  std::deque<FrameOp> ops;
  std::deque<TextureUpload> textureUploads;
  ByteBuffer verts;
  ByteBuffer uniforms;
  ByteBuffer indices;
  ByteBuffer storage;
  ByteBuffer textureUpload;
  uint64_t frameId = 0;
  uint32_t frameIndex = 0;
  StagingHighWater copied;
  AuroraStats stats{};

  // Reset for a fresh recording WITHOUT freeing the ByteBuffer storage. The render worker
  // reads these buffers (per-op glBufferSubData/glTexSubImage uploads) concurrently with the
  // recording thread appending to them -- EFB-copy / offscreen pass boundaries (shadows,
  // minimap, effects) enqueue passes mid-frame, so the worker starts consuming this packet
  // while recording is still in progress. A `frame = {}` reset frees the storage, forcing the
  // buffers to regrow via realloc() every frame; a realloc that lands while the worker is
  // mid-read frees the pointer out from under it -> use-after-free -> a torn upload. That
  // manifests as intermittent geometry/texture corruption (weird shadows, black minimap) that
  // *vanishes under RenderDoc*, which replays the settled final buffer contents single-threaded.
  // Retaining capacity and reserving the fixed ring ceilings means recording never reallocs, so
  // the worker's in-flight reads always see stable storage. Reallocating here is safe: begin_frame
  // only reuses a slot the worker has already released (FrameSlotPool), so nothing reads it now.
  void reset() {
    renderPasses.clear();
    textureCopies.clear();
    encoderTasks.clear();
    ops.clear();
    textureUploads.clear();
    verts.clear();
    uniforms.clear();
    indices.clear();
    storage.clear();
    textureUpload.clear();
    verts.reserve_extra(VertexBufferSize);
    uniforms.reserve_extra(UniformBufferSize);
    indices.reserve_extra(IndexBufferSize);
    copied = {};
    stats = {};
  }
};

static std::array<FramePacket, FrameSlotCount> g_framePackets;
static FramePacket* g_recordingFrame = nullptr;
static size_t g_recordingFrameSlot = 0;
static uint64_t g_nextFrameId = 1;
static render_worker::FrameSlotPool g_frameSlots{FrameSlotCount};
static u32 g_currentRenderPass = UINT32_MAX;
static bool g_inOffscreen = false;
static std::optional<RenderPass> g_suspendedEfbPass;
static Viewport g_suspendedEfbViewport;
static ClipRect g_suspendedEfbScissor;
static webgpu::TextureWithSampler g_offscreenColor;
static webgpu::TextureWithSampler g_offscreenDepth;
static Viewport g_cachedViewport;
static ClipRect g_cachedScissor;

using PresentClock = std::chrono::steady_clock;
static constexpr auto PresentFpsWindow = std::chrono::seconds{1};
static std::mutex g_presentStatsMutex;
static std::deque<PresentClock::time_point> g_presentTimes;
static std::atomic_bool g_processEventsQueued = false;
static std::atomic_int64_t g_lastPresentNs = 0;
static std::atomic_int64_t g_presentPeriodNs = 0;
static std::atomic_int64_t g_cpuFrameTimeNs = 0;
static PresentClock::time_point g_cpuFrameStart;
static constexpr auto FrameStartSafetyMargin = std::chrono::milliseconds{2};
static constexpr auto MaxPacingSample = std::chrono::milliseconds{250};
static constexpr uint32_t PacingEmaWeight = 8;

static int64_t timestamp_ns(PresentClock::time_point time) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count();
}

static int64_t duration_ns(PresentClock::duration duration) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
}

static void update_ema(std::atomic_int64_t& value, int64_t sample) {
  if (sample <= 0 || sample > duration_ns(MaxPacingSample)) {
    return;
  }

  int64_t current = value.load(std::memory_order_acquire);
  while (true) {
    const int64_t next = current == 0 ? sample : current + (sample - current) / static_cast<int64_t>(PacingEmaWeight);
    if (value.compare_exchange_weak(current, next, std::memory_order_acq_rel, std::memory_order_acquire)) {
      return;
    }
  }
}

static void prune_present_times(PresentClock::time_point now) {
  while (!g_presentTimes.empty() && g_presentTimes.front() + PresentFpsWindow < now) {
    g_presentTimes.pop_front();
  }
}

static void process_events() {
  // No Dawn instance to pump on GL: async GPU work is driven by the render worker
  // and GL fences. Kept as a hook so the callers (idle-wait paths) keep their shape.
}

static void enqueue_process_events() {
  if (render_worker::is_worker_thread()) {
    process_events();
    return;
  }

  bool expected = false;
  if (!g_processEventsQueued.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                     std::memory_order_acquire)) {
    return;
  }

  render_worker::enqueue_work([] {
    process_events();
    g_processEventsQueued.store(false, std::memory_order_release);
  });
}

static void wait_for_gpu_progress(std::chrono::nanoseconds sleepDuration) {
  if (render_worker::is_idle()) {
    enqueue_process_events();
  }
  std::this_thread::sleep_for(sleepDuration);
}

static void pace_frame_start() {
  ZoneScopedN("Frame start pacing");
  if (g_frameSlots.free_count() == FrameSlotCount) {
    return;
  }

  const int64_t lastPresentNs = g_lastPresentNs.load(std::memory_order_acquire);
  const int64_t presentPeriodNs = g_presentPeriodNs.load(std::memory_order_acquire);
  const int64_t cpuFrameTimeNs = g_cpuFrameTimeNs.load(std::memory_order_acquire);
  if (lastPresentNs == 0 || presentPeriodNs == 0 || cpuFrameTimeNs == 0) {
    return;
  }

  const int64_t safetyMarginNs = duration_ns(FrameStartSafetyMargin);
  const int64_t targetStartNs = lastPresentNs + presentPeriodNs - cpuFrameTimeNs - safetyMarginNs;
  int64_t nowNs = timestamp_ns(PresentClock::now());
  if (targetStartNs <= nowNs) {
    return;
  }

  const double initialWaitMs = static_cast<double>(targetStartNs - nowNs) / 1'000'000.0;
  TracyPlot("aurora: frameStartPaceWaitMs", initialWaitMs);
  while (nowNs < targetStartNs) {
    const int64_t remainingNs = targetStartNs - nowNs;
    const auto sleepDuration =
        remainingNs > 1'000'000 ? std::chrono::milliseconds{1} : std::chrono::nanoseconds{remainingNs};
    wait_for_gpu_progress(sleepDuration);
    nowNs = timestamp_ns(PresentClock::now());
  }
}

static void set_efb_targets(RenderPass& pass) {
  pass.colorView = webgpu::g_frameBuffer.view;
  pass.resolveView = webgpu::g_graphicsConfig.msaaSamples > 1 ? webgpu::g_frameBufferResolved.view : gl::Texture{};
  pass.depthStencilView = webgpu::g_depthBuffer.view;
  pass.copySourceTexture =
      webgpu::g_graphicsConfig.msaaSamples > 1 ? webgpu::g_frameBufferResolved.texture : webgpu::g_frameBuffer.texture;
  pass.copySourceView =
      webgpu::g_graphicsConfig.msaaSamples > 1 ? webgpu::g_frameBufferResolved.view : webgpu::g_frameBuffer.view;
  pass.copySourceDepthView = webgpu::g_depthBuffer.view;
  pass.targetSize = webgpu::g_frameBuffer.size;
  pass.msaaSamples = webgpu::g_graphicsConfig.msaaSamples;
  pass.hasDepth = true;
  pass.hasStencil = false;
}

struct OffscreenCacheKey {
  uint32_t width;
  uint32_t height;

  bool operator==(const OffscreenCacheKey& rhs) const { return width == rhs.width && height == rhs.height; }
  template <typename H>
  friend H AbslHashValue(H h, const OffscreenCacheKey& key) {
    return H::combine(std::move(h), key.width, key.height);
  }
};
struct OffscreenCacheEntry {
  webgpu::TextureWithSampler color;
  webgpu::TextureWithSampler depth;
};
static absl::flat_hash_map<OffscreenCacheKey, OffscreenCacheEntry> g_offscreenCache;

// Pooled destinations for the public resolve_pass API. Entries are recycled
// per frame slot: a slot is only re-acquired after the render worker has
// submitted its previous frame, and queue serialization orders the new frame's
// copies after the old frame's reads.
struct PassSnapshotEntry {
  webgpu::TextureWithSampler color;
  webgpu::TextureWithSampler depth; // R32Float raw depth
};
struct PassSnapshotPool {
  std::vector<PassSnapshotEntry> entries;
  size_t used = 0;
};
static std::array<PassSnapshotPool, FrameSlotCount> g_passSnapshotPools;

static PassSnapshotEntry& acquire_pass_snapshot(uint32_t width, uint32_t height, bool wantColor, bool wantDepth) {
  // Full-target color snapshot for the public resolve_pass API (unused on the live TP
  // path). Create/resize the color texture on demand; the R32Float depth snapshot stays
  // gated off (tex_copy_conv::snapshot_depth is a stub), so wantDepth is ignored.
  (void)wantDepth;
  auto& pool = g_passSnapshotPools[g_recordingFrameSlot];
  if (pool.used == pool.entries.size()) {
    pool.entries.emplace_back();
  }
  auto& entry = pool.entries[pool.used++];
  if (wantColor &&
      (entry.color.texture.id == 0 || entry.color.size.width != width || entry.color.size.height != height)) {
    const auto colorFormat = webgpu::g_graphicsConfig.surfaceConfiguration.format;
    const gl::Extent3D size{width, height, 1};
    const gl::Texture color = create_gl_texture(colorFormat, size, 1, /*renderable=*/true);
    entry.color = webgpu::TextureWithSampler{.texture = color, .view = color, .size = size, .format = colorFormat};
  }
  return entry;
}

static FramePacket& current_frame_packet() {
  CHECK(g_recordingFrame != nullptr, "No active frame packet");
  return *g_recordingFrame;
}

static RenderPassList& current_render_passes() { return current_frame_packet().renderPasses; }

static StagingHighWater current_high_water(const FramePacket& frame) noexcept {
  return {
      .verts = static_cast<uint32_t>(frame.verts.size()),
      .uniforms = static_cast<uint32_t>(frame.uniforms.size()),
      .indices = static_cast<uint32_t>(frame.indices.size()),
      .storage = static_cast<uint32_t>(frame.storage.size()),
      .textureUpload = static_cast<uint32_t>(frame.textureUpload.size()),
      .textureUploadCount = frame.textureUploads.size(),
  };
}

static FrameOp capture_frame_op(FramePacket& frame, FrameOpType type, uint32_t index) {
  FrameOp op{
      .type = type,
      .index = index,
      .renderPass =
          type == FrameOpType::RenderPass && index < frame.renderPasses.size() ? &frame.renderPasses[index] : nullptr,
      .textureCopy = type == FrameOpType::TextureCopy && index < frame.textureCopies.size()
                         ? &frame.textureCopies[index]
                         : nullptr,
      .encoderTask =
          type == FrameOpType::EncoderTask && index < frame.encoderTasks.size() ? &frame.encoderTasks[index] : nullptr,
      .highWater = current_high_water(frame),
  };
  op.textureUploads.reserve(op.highWater.textureUploadCount);
  for (size_t i = 0; i < op.highWater.textureUploadCount; ++i) {
    op.textureUploads.push_back(&frame.textureUploads[i]);
  }
  return op;
}

static void seal_pass(FramePacket& frame, uint32_t passIndex) {
  if (passIndex >= frame.renderPasses.size()) {
    return;
  }
  auto& pass = frame.renderPasses[passIndex];
  if (pass.sealed) {
    return;
  }
  pass.sealed = true;
}

static void encode_op(FramePacket& frame, const FrameOp& op);
static void render(FramePacket& frame, RenderPass& passInfo, uint32_t passIndex);
static void render_pass(gl::PassEncoder& pass, FramePacket& frame, const RenderPass& passInfo);
static void render_custom_draw(const CustomDrawCommand& draw, gl::PassEncoder& pass, const RenderPass& passInfo);
static void execute_encoder_task(const EncoderTask& task);
static void resume_efb_pass_loading(const RenderPass& prevPass);
static void expire_cached_bind_groups();
static void push_command(CommandType type, const Command::Data& data);

static void enqueue_op(FramePacket& frame, size_t frameSlot, uint32_t opIndex) {
  if (opIndex >= frame.ops.size()) {
    return;
  }
  auto op = frame.ops[opIndex];
  render_worker::enqueue_encode_pass(frame.frameId, opIndex, [frameSlot, op = std::move(op)] {
    if (op.renderPass == nullptr && op.textureCopy == nullptr && op.encoderTask == nullptr) {
      return;
    }
    auto& packet = g_framePackets[frameSlot];
    encode_op(packet, op);
  });
}

static void enqueue_pass(FramePacket& frame, size_t frameSlot, uint32_t passIndex) {
  seal_pass(frame, passIndex);
  const auto opIndex = static_cast<uint32_t>(frame.ops.size());
  frame.ops.emplace_back(capture_frame_op(frame, FrameOpType::RenderPass, passIndex));
  enqueue_op(frame, frameSlot, opIndex);
}

void queue_texture_upload(TextureUpload upload) {
  if (g_currentRenderPass != UINT32_MAX) {
    ASSERT(!current_render_passes()[g_currentRenderPass].sealed,
           "Attempted to append texture upload to sealed render pass {}", g_currentRenderPass);
  }
  current_frame_packet().textureUploads.emplace_back(std::move(upload));
}

void queue_texture_upload_data(const uint8_t* data, uint32_t bytesPerRow, uint32_t rowsPerImage, gl::Texture tex,
                               gl::Origin3D origin, gl::Extent3D size, uint32_t level) {
  // GL: stage the tightly-packed source rows into the frame's textureUpload ByteBuffer;
  // the worker does glTexSubImage2D from that range at the op slot (Phase 2). No 256-byte
  // row alignment (that was a Dawn CopyBufferToTexture requirement) and no staging buffer.
  auto& frame = current_frame_packet();
  const uint32_t offset = static_cast<uint32_t>(frame.textureUpload.size());
  const size_t byteCount = static_cast<size_t>(bytesPerRow) * rowsPerImage;
  frame.textureUpload.append(data, byteCount);
  queue_texture_upload(TextureUpload{tex, origin, size, level, bytesPerRow, Range{offset, static_cast<uint32_t>(byteCount)}});
}

void queue_texture_copy(TextureCopyView src, TextureCopyView dst, gl::Extent3D size, bool flipY) {
  ZoneScoped;
  auto& frame = current_frame_packet();
  if (g_currentRenderPass != UINT32_MAX) {
    enqueue_pass(frame, g_recordingFrameSlot, g_currentRenderPass);
    g_currentRenderPass = UINT32_MAX;
  }

  const auto copyIndex = static_cast<uint32_t>(frame.textureCopies.size());
  frame.textureCopies.emplace_back(TextureCopy{
      .src = std::move(src),
      .dst = std::move(dst),
      .size = size,
      .flipY = flipY,
  });
  const auto opIndex = static_cast<uint32_t>(frame.ops.size());
  frame.ops.emplace_back(capture_frame_op(frame, FrameOpType::TextureCopy, copyIndex));
  enqueue_op(frame, g_recordingFrameSlot, opIndex);
}

void begin_color_pass(const ColorPassDescriptor& desc) {
  ZoneScoped;
  auto& frame = current_frame_packet();
  if (g_currentRenderPass != UINT32_MAX) {
    enqueue_pass(frame, g_recordingFrameSlot, g_currentRenderPass);
  }

  RenderPass pass{
      .label = desc.label != nullptr ? desc.label : "",
      .colorView = desc.colorView,
      .resolveView = desc.resolveView,
      .depthStencilView = desc.depthStencilView,
      .targetSize = desc.targetSize,
      .msaaSamples = desc.sampleCount,
      .clearColorValue =
          {
              static_cast<float>(desc.clearColor.r),
              static_cast<float>(desc.clearColor.g),
              static_cast<float>(desc.clearColor.b),
              static_cast<float>(desc.clearColor.a),
          },
      .clearDepthValue = desc.depthClearValue,
      .colorLoadOp = desc.colorLoadOp,
      .colorStoreOp = desc.colorStoreOp,
      .depthLoadOp = desc.depthLoadOp,
      .depthStoreOp = desc.depthStoreOp,
      .stencilLoadOp = desc.stencilLoadOp,
      .stencilStoreOp = desc.stencilStoreOp,
      .stencilClearValue = desc.stencilClearValue,
      .clearColor = desc.colorLoadOp == gl::LoadOp::Clear,
      .clearDepth = desc.depthLoadOp == gl::LoadOp::Clear,
      .hasDepth = desc.hasDepth,
      .hasStencil = desc.hasStencil,
      .depthReadOnly = desc.depthReadOnly,
  };
  pass.commands.reserve(128);
  frame.renderPasses.emplace_back(std::move(pass));
  g_currentRenderPass = static_cast<uint32_t>(frame.renderPasses.size() - 1);

  g_cachedViewport = {0.f, 0.f, static_cast<float>(desc.targetSize.width), static_cast<float>(desc.targetSize.height),
                      0.f, 1.f};
  g_cachedScissor = {0, 0, static_cast<int32_t>(desc.targetSize.width), static_cast<int32_t>(desc.targetSize.height)};
  push_command(CommandType::SetViewport, Command::Data{.setViewport = g_cachedViewport});
  push_command(CommandType::SetScissor, Command::Data{.setScissor = g_cachedScissor});
}

void end_color_pass() {
  ZoneScoped;
  if (g_currentRenderPass == UINT32_MAX) {
    return;
  }
  enqueue_pass(current_frame_packet(), g_recordingFrameSlot, g_currentRenderPass);
  g_currentRenderPass = UINT32_MAX;
}

static void push_command(CommandType type, const Command::Data& data) {
  if (g_currentRenderPass == UINT32_MAX)
    UNLIKELY {
      Log.warn("Dropping command {}", magic_enum::enum_name(type));
      return;
    }
  auto& renderPass = current_render_passes()[g_currentRenderPass];
  ASSERT(!renderPass.sealed, "Attempted to append command {} to sealed render pass {}", magic_enum::enum_name(type),
         g_currentRenderPass);
  if (type == CommandType::Draw || type == CommandType::CustomDraw) {
    renderPass.hasDraws = true;
  }
  renderPass.commands.push_back({
      .type = type,
#ifdef AURORA_GFX_DEBUG_GROUPS
      .debugGroupStack = g_debugGroupStack,
#endif
      .data = data,
  });
}

template <>
gx::DrawData* get_last_draw_command() {
  if (g_currentRenderPass >= current_render_passes().size()) {
    return nullptr;
  }
  auto& last = current_render_passes()[g_currentRenderPass].commands.back();
  if (last.type != CommandType::Draw || last.data.draw.type != ShaderType::GX) {
    return nullptr;
  }
  return &last.data.draw.gx;
}

static void push_draw_command(ShaderDrawCommand data) {
  push_command(CommandType::Draw, Command::Data{.draw = data});
  ++g_drawCallCount;
}

Vec2<uint32_t> get_render_target_size() noexcept {
  if (g_currentRenderPass < current_render_passes().size()) {
    const auto& size = current_render_passes()[g_currentRenderPass].targetSize;
    return {size.width, size.height};
  }
  const auto windowSize = window::get_window_size();
  return {windowSize.fb_width, windowSize.fb_height};
}

void set_viewport(const Viewport& cmd) noexcept {
  if (cmd != g_cachedViewport) {
    push_command(CommandType::SetViewport, Command::Data{.setViewport = cmd});
    g_cachedViewport = cmd;
  }
}

void set_scissor(const ClipRect& cmd) noexcept {
  if (cmd != g_cachedScissor) {
    push_command(CommandType::SetScissor, Command::Data{.setScissor = cmd});
    g_cachedScissor = cmd;
  }
}

template <>
void push_draw_command(clear::DrawData data) {
  push_draw_command(ShaderDrawCommand{.type = ShaderType::Clear, .clear = data});
}

template <>
PipelineRef pipeline_ref(const clear::PipelineConfig& config) {
  return find_pipeline(ShaderType::Clear, config, [=] { return create_pipeline(config); });
}

void resolve_pass_into(TextureHandle texture, ClipRect rect, bool clearColor, bool clearAlpha, bool clearDepth,
                       Vec4<float> clearColorValue, float clearDepthValue, GXTexFmt resolveFormat) {
  // Resolve current render pass
  auto& prevPass = current_render_passes()[g_currentRenderPass];
  prevPass.resolveTarget = std::move(texture);
  prevPass.resolveRect = rect;
  prevPass.resolveFormat = resolveFormat;
  // Push UV transform uniform for tex_copy_conv (crop region in UV space)
  const auto srcW = static_cast<float>(prevPass.targetSize.width);
  const auto srcH = static_cast<float>(prevPass.targetSize.height);
  const std::array uvTransform{
      static_cast<float>(rect.x) / srcW,
      static_cast<float>(rect.y) / srcH,
      static_cast<float>(rect.width) / srcW,
      static_cast<float>(rect.height) / srcH,
  };
  prevPass.resolveUniformRange = push_uniform(uvTransform);
  enqueue_pass(current_frame_packet(), g_recordingFrameSlot, g_currentRenderPass);

  // Populate new render pass from previous
  const auto msaaSamples = prevPass.msaaSamples;
  RenderPass newPass{
      .label = pass_label("EFB"),
      .colorView = prevPass.colorView,
      .resolveView = prevPass.resolveView,
      .depthStencilView = prevPass.depthStencilView,
      .copySourceTexture = prevPass.copySourceTexture,
      .copySourceView = prevPass.copySourceView,
      .copySourceDepthView = prevPass.copySourceDepthView,
      .targetSize = prevPass.targetSize,
      .msaaSamples = msaaSamples,
      .clearColorValue = clearColorValue,
      .clearDepthValue = clearDepthValue,
      .clearColor = clearColor && clearAlpha,
      .clearDepth = clearDepth,
      .hasDepth = prevPass.hasDepth,
      .hasStencil = prevPass.hasStencil,
  };
  newPass.commands.reserve(2048);
  current_render_passes().emplace_back(std::move(newPass));
  ++g_currentRenderPass;

  if (!newPass.clearColor && (clearColor || clearAlpha)) {
    // If we're only clearing color _or_ alpha, perform a clear draw
    push_draw_command(clear::DrawData{
        .pipeline = pipeline_ref(clear::PipelineConfig{
            .msaaSamples = msaaSamples,
            .clearColor = clearColor,
            .clearAlpha = clearAlpha,
            .clearDepth = false, // Depth cleared via render attachment
        }),
        .color =
            gl::Color{
                .r = clearColorValue.x(),
                .g = clearColorValue.y(),
                .b = clearColorValue.z(),
                .a = clearColorValue.w(),
            },
    });
  }
  push_command(CommandType::SetViewport, Command::Data{.setViewport = g_cachedViewport});
  push_command(CommandType::SetScissor, Command::Data{.setScissor = g_cachedScissor});
}

void queue_palette_conv(tex_palette_conv::ConvRequest req) {
  auto& renderPass = current_render_passes()[g_currentRenderPass];
  ASSERT(!renderPass.sealed, "Attempted to append palette conversion to sealed render pass {}", g_currentRenderPass);
  renderPass.paletteConvs.push_back(std::move(req));
}

bool is_offscreen() noexcept { return g_inOffscreen; }

uint32_t get_sample_count() noexcept {
  CHECK(g_currentRenderPass != UINT32_MAX, "get_sample_count called outside of a frame");
  return current_render_passes()[g_currentRenderPass].msaaSamples;
}

void clear_caches() noexcept {
  g_offscreenCache.clear();
  std::lock_guard lock{g_bindGroupCacheMutex};
  g_cachedBindGroups.clear();
}

uint32_t color_format() noexcept {
  return static_cast<uint32_t>(webgpu::g_graphicsConfig.surfaceConfiguration.format);
}

uint32_t depth_format() noexcept { return static_cast<uint32_t>(webgpu::g_graphicsConfig.depthFormat); }

uint32_t sample_count() noexcept { return webgpu::g_graphicsConfig.msaaSamples; }

bool uses_reversed_z() noexcept { return gx::UseReversedZ; }

DrawTypeId register_draw_type(const DrawTypeDescriptor& desc) {
  if (desc.draw == nullptr) {
    Log.warn("register_draw_type: draw callback is null");
    return InvalidDrawType;
  }

  std::lock_guard lock{g_runtimeDrawTypeMutex};
  uint32_t idx;
  if (!g_freeDrawTypeSlots.empty()) {
    idx = g_freeDrawTypeSlots.back();
    g_freeDrawTypeSlots.pop_back();
  } else {
    idx = static_cast<uint32_t>(g_runtimeDrawTypes.size());
    g_runtimeDrawTypes.emplace_back();
  }
  auto& slot = g_runtimeDrawTypes[idx];
  slot.label = desc.label != nullptr ? desc.label : "";
  slot.draw = desc.draw;
  slot.userdata = desc.userdata;
  return make_draw_type_id(idx, slot.generation);
}

void unregister_draw_type(DrawTypeId type) noexcept {
  std::lock_guard lock{g_runtimeDrawTypeMutex};
  if (find_runtime_draw_type(type) == nullptr) {
    return;
  }
  const auto idx = draw_type_index(type);
  auto& slot = g_runtimeDrawTypes[idx];
  slot.label.clear();
  slot.draw = nullptr;
  slot.userdata = nullptr;
  ++slot.generation;
  g_freeDrawTypeSlots.push_back(idx);
}

bool push_custom_draw(DrawTypeId type, const void* payload, size_t payloadSize) {
  if (type == InvalidDrawType) {
    Log.warn("push_custom_draw: invalid draw type");
    return false;
  }
  if (payloadSize > InlineDrawPayloadSize) {
    Log.warn("push_custom_draw: payload size {} exceeds inline payload size {}", payloadSize, InlineDrawPayloadSize);
    return false;
  }
  if (payloadSize > 0 && payload == nullptr) {
    Log.warn("push_custom_draw: non-zero payload size with null payload");
    return false;
  }
  {
    std::lock_guard lock{g_runtimeDrawTypeMutex};
    if (find_runtime_draw_type(type) == nullptr) {
      Log.warn("push_custom_draw: unregistered draw type {:#x}", type);
      return false;
    }
  }

  gx::fifo::drain();

  if (g_recordingFrame == nullptr || g_currentRenderPass == UINT32_MAX) {
    Log.warn("push_custom_draw: called outside an active render pass");
    return false;
  }

  CustomDrawCommand draw{};
  draw.type = type;
  draw.payloadSize = static_cast<uint32_t>(payloadSize);
  if (payloadSize > 0) {
    std::memcpy(draw.payload.data(), payload, payloadSize);
  }
  push_command(CommandType::CustomDraw, Command::Data{.customDraw = draw});
  ++g_drawCallCount;
  return true;
}

static OffscreenCacheEntry get_offscreen_textures(uint32_t width, uint32_t height) {
  // A zero-sized offscreen (e.g. the shadow system under shadowResolutionMultiplier=0)
  // makes glTexStorage2D fail with GL_INVALID_VALUE and lose the device on Mali. Clamp
  // to at least 1x1: the render becomes an effective no-op (disabled shadow) but the
  // texture is valid and the begin/end offscreen pass stays balanced.
  width = width < 1u ? 1u : width;
  height = height < 1u ? 1u : height;
  OffscreenCacheKey key{width, height};
  if (const auto it = g_offscreenCache.find(key); it != g_offscreenCache.end()) {
    return it->second;
  }
  // Real GL color + depth textures, created on the worker (create_gl_texture marshals and
  // blocks -- begin_offscreen runs on the recording thread). Cached by size, so this fires
  // only on the first offscreen pass of each size. Color is both sampleable and an FBO
  // color attachment; depth is the FBO depth attachment (linear color sampling for the
  // scaled reads effects take).
  const auto colorFormat = webgpu::g_graphicsConfig.surfaceConfiguration.format;
  const auto depthFormat = webgpu::g_graphicsConfig.depthFormat;
  const gl::Extent3D size{width, height, 1};
  gl::SamplerDescriptor colorSamplerDesc{};
  colorSamplerDesc.magFilter = gl::FilterMode::Linear;
  colorSamplerDesc.minFilter = gl::FilterMode::Linear;
  const gl::Texture color = create_gl_texture(colorFormat, size, 1, /*renderable=*/true);
  const gl::Texture depth = create_gl_texture(depthFormat, size, 1, /*renderable=*/true);
  OffscreenCacheEntry entry{
      .color = webgpu::TextureWithSampler{.texture = color,
                                          .view = color,
                                          .size = size,
                                          .format = colorFormat,
                                          .sampler = create_gl_sampler(colorSamplerDesc)},
      .depth = webgpu::TextureWithSampler{.texture = depth, .view = depth, .size = size, .format = depthFormat},
  };
  auto [insertIt, _] = g_offscreenCache.emplace(key, std::move(entry));
  return insertIt->second;
}

void begin_offscreen(uint32_t width, uint32_t height) {
  ZoneScoped;
  CHECK(g_currentRenderPass != UINT32_MAX, "begin_offscreen called outside of a frame");

  // If the current EFB pass has no resolve target, its output is unobservable.
  // Suspend it so that we can resume it after the offscreen pass.
  if (!g_inOffscreen) {
    auto& currentPass = current_render_passes()[g_currentRenderPass];
    if (!currentPass.resolveTarget) {
      g_suspendedEfbPass = std::move(currentPass);
      current_render_passes().pop_back();
      --g_currentRenderPass;
    } else {
      enqueue_pass(current_frame_packet(), g_recordingFrameSlot, g_currentRenderPass);
    }
    g_suspendedEfbViewport = g_cachedViewport;
    g_suspendedEfbScissor = g_cachedScissor;
  }

  // Create offscreen textures
  auto offscreenEntry = get_offscreen_textures(width, height);
  g_offscreenColor = std::move(offscreenEntry.color);
  g_offscreenDepth = std::move(offscreenEntry.depth);

  // Start a new pass with offscreen targets
  RenderPass newPass{
      .label = pass_label("Offscreen"),
      .colorView = g_offscreenColor.view,
      .depthStencilView = g_offscreenDepth.view,
      .copySourceTexture = g_offscreenColor.texture,
      .copySourceView = g_offscreenColor.view,
      .copySourceDepthView = g_offscreenDepth.view,
      .targetSize = {width, height, 1},
      .msaaSamples = 1,
      .clearColorValue = {0.f, 0.f, 0.f, 0.f},
      .clearDepthValue = gx::UseReversedZ ? 0.f : 1.f,
      .clearColor = true,
      .clearDepth = true,
      .hasDepth = true,
      .hasStencil = false,
  };
  current_render_passes().emplace_back(std::move(newPass));
  ++g_currentRenderPass;

  g_inOffscreen = true;

  g_cachedViewport = {0.f, 0.f, static_cast<float>(width), static_cast<float>(height), 0.f, 1.f};
  g_cachedScissor = {0, 0, static_cast<int32_t>(width), static_cast<int32_t>(height)};
  push_command(CommandType::SetViewport, Command::Data{.setViewport = g_cachedViewport});
  push_command(CommandType::SetScissor, Command::Data{.setScissor = g_cachedScissor});
}

void end_offscreen() {
  ZoneScoped;
  CHECK(g_inOffscreen, "end_offscreen called without begin_offscreen");

  // Mark current render pass as discardable if there is no consumer
  auto& offscreenPass = current_render_passes()[g_currentRenderPass];
  offscreenPass.discardable = !offscreenPass.has_consumer();

  enqueue_pass(current_frame_packet(), g_recordingFrameSlot, g_currentRenderPass);

  g_inOffscreen = false;
  g_offscreenColor = {};
  g_offscreenDepth = {};

  // Resume suspended EFB pass, or start a new one (load existing content)
  if (g_suspendedEfbPass) {
    current_render_passes().emplace_back(std::move(*g_suspendedEfbPass));
    g_suspendedEfbPass.reset();
  } else {
    auto& pass = current_render_passes().emplace_back();
    pass.label = pass_label("EFB");
    pass.clearColor = false;
    pass.clearDepth = false;
  }
  ++g_currentRenderPass;
  set_efb_targets(current_render_passes()[g_currentRenderPass]);

  g_cachedViewport = g_suspendedEfbViewport;
  g_cachedScissor = g_suspendedEfbScissor;
  push_command(CommandType::SetViewport, Command::Data{.setViewport = g_cachedViewport});
  push_command(CommandType::SetScissor, Command::Data{.setScissor = g_cachedScissor});
}

bool create_pass(uint32_t width, uint32_t height) {
  if (width == 0 || height == 0) {
    Log.warn("create_pass: invalid size {}x{}", width, height);
    return false;
  }

  gx::fifo::drain();

  if (g_recordingFrame == nullptr || g_currentRenderPass == UINT32_MAX) {
    Log.warn("create_pass: called outside an active render pass");
    return false;
  }
  if (g_inOffscreen) {
    Log.warn("create_pass: an offscreen pass is already active (nesting is unsupported)");
    return false;
  }

  begin_offscreen(width, height);
  return true;
}

bool resolve_pass(const ResolveDesc& desc, ResolvedTargets& out) {
  out = {};
  gx::fifo::drain();

  if (g_recordingFrame == nullptr || g_currentRenderPass == UINT32_MAX) {
    Log.warn("resolve_pass: called outside an active render pass");
    return false;
  }

  bool wantDepth = desc.depth;
  if (wantDepth && !tex_copy_conv::snapshot_depth_supported()) {
    Log.warn("resolve_pass: depth snapshots are unsupported on this device");
    wantDepth = false;
  }

  auto& prevPass = current_render_passes()[g_currentRenderPass];
  const uint32_t width = prevPass.targetSize.width;
  const uint32_t height = prevPass.targetSize.height;
  // Requesting no snapshots is a plain pass break (or offscreen close, discarding its output).
  if (desc.color || wantDepth) {
    auto& entry = acquire_pass_snapshot(width, height, desc.color, wantDepth);
    if (desc.color) {
      prevPass.snapshotColorDst = entry.color.texture;
      out.color = entry.color.view.id;
      out.colorFormat = static_cast<uint32_t>(entry.color.format);
    }
    if (wantDepth) {
      prevPass.snapshotDepthDst = entry.depth.view;
      out.depth = entry.depth.view.id;
    }
  }
  out.width = width;
  out.height = height;

  if (g_inOffscreen) {
    // Seal the offscreen pass and resume the EFB.
    end_offscreen();
    return true;
  }

  // Seal the current EFB pass and continue on a new one that loads the existing contents.
  // EFB writes persist into the loading continuation, so content keeps the pass alive even
  // without a consumer.
  prevPass.discardable = !prevPass.has_consumer() && !prevPass.has_content();
  enqueue_pass(current_frame_packet(), g_recordingFrameSlot, g_currentRenderPass);
  resume_efb_pass_loading(prevPass);
  return true;
}

static void resume_efb_pass_loading(const RenderPass& prevPass) {
  RenderPass newPass{
      .label = pass_label("EFB"),
      .colorView = prevPass.colorView,
      .resolveView = prevPass.resolveView,
      .depthStencilView = prevPass.depthStencilView,
      .copySourceTexture = prevPass.copySourceTexture,
      .copySourceView = prevPass.copySourceView,
      .copySourceDepthView = prevPass.copySourceDepthView,
      .targetSize = prevPass.targetSize,
      .msaaSamples = prevPass.msaaSamples,
      .clearColor = false,
      .clearDepth = false,
      .hasDepth = prevPass.hasDepth,
      .hasStencil = prevPass.hasStencil,
  };
  newPass.commands.reserve(2048);
  current_render_passes().emplace_back(std::move(newPass));
  ++g_currentRenderPass;
  push_command(CommandType::SetViewport, Command::Data{.setViewport = g_cachedViewport});
  push_command(CommandType::SetScissor, Command::Data{.setScissor = g_cachedScissor});
}

EncoderTaskId register_encoder_task_type(const EncoderTaskDescriptor& desc) {
  if (desc.callback == nullptr) {
    Log.warn("register_encoder_task_type: callback is null");
    return InvalidEncoderTask;
  }

  std::lock_guard lock{g_runtimeDrawTypeMutex};
  uint32_t idx;
  if (!g_freeEncoderTaskTypeSlots.empty()) {
    idx = g_freeEncoderTaskTypeSlots.back();
    g_freeEncoderTaskTypeSlots.pop_back();
  } else {
    idx = static_cast<uint32_t>(g_runtimeEncoderTaskTypes.size());
    g_runtimeEncoderTaskTypes.emplace_back();
  }
  auto& slot = g_runtimeEncoderTaskTypes[idx];
  slot.label = desc.label != nullptr ? desc.label : "";
  slot.callback = desc.callback;
  slot.userdata = desc.userdata;
  return make_draw_type_id(idx, slot.generation);
}

void unregister_encoder_task_type(EncoderTaskId type) noexcept {
  std::lock_guard lock{g_runtimeDrawTypeMutex};
  if (find_runtime_encoder_task_type(type) == nullptr) {
    return;
  }
  const auto idx = draw_type_index(type);
  auto& slot = g_runtimeEncoderTaskTypes[idx];
  slot.label.clear();
  slot.callback = nullptr;
  slot.userdata = nullptr;
  ++slot.generation;
  g_freeEncoderTaskTypeSlots.push_back(idx);
}

bool push_encoder_task(EncoderTaskId type, const void* payload, size_t payloadSize) {
  if (type == InvalidEncoderTask) {
    Log.warn("push_encoder_task: invalid encoder task type");
    return false;
  }
  if (payloadSize > InlineDrawPayloadSize) {
    Log.warn("push_encoder_task: payload size {} exceeds inline payload size {}", payloadSize, InlineDrawPayloadSize);
    return false;
  }
  if (payloadSize > 0 && payload == nullptr) {
    Log.warn("push_encoder_task: non-zero payload size with null payload");
    return false;
  }
  {
    std::lock_guard lock{g_runtimeDrawTypeMutex};
    if (find_runtime_encoder_task_type(type) == nullptr) {
      Log.warn("push_encoder_task: unregistered encoder task type {:#x}", type);
      return false;
    }
  }

  gx::fifo::drain();

  if (g_recordingFrame == nullptr || g_currentRenderPass == UINT32_MAX) {
    Log.warn("push_encoder_task: called outside an active render pass");
    return false;
  }
  if (g_inOffscreen) {
    Log.warn("push_encoder_task: unsupported while an offscreen pass is active");
    return false;
  }

  // Seal the current EFB pass, record the task between it and a continuation
  // pass that loads the existing contents. EFB writes persist into the continuation, so
  // content keeps the sealed pass alive even without a consumer.
  auto& frame = current_frame_packet();
  auto& prevPass = current_render_passes()[g_currentRenderPass];
  prevPass.discardable = !prevPass.has_consumer() && !prevPass.has_content();
  enqueue_pass(frame, g_recordingFrameSlot, g_currentRenderPass);

  const auto taskIndex = static_cast<uint32_t>(frame.encoderTasks.size());
  auto& task = frame.encoderTasks.emplace_back(EncoderTask{.type = type});
  task.payloadSize = static_cast<uint32_t>(payloadSize);
  if (payloadSize > 0) {
    std::memcpy(task.payload.data(), payload, payloadSize);
  }
  const auto opIndex = static_cast<uint32_t>(frame.ops.size());
  frame.ops.emplace_back(capture_frame_op(frame, FrameOpType::EncoderTask, taskIndex));
  enqueue_op(frame, g_recordingFrameSlot, opIndex);

  resume_efb_pass_loading(prevPass);
  return true;
}

template <>
void push_draw_command(gx::DrawData data) {
  push_draw_command(ShaderDrawCommand{.type = ShaderType::GX, .gx = data});
}

#ifdef AURORA_ENABLE_RMLUI
template <>
void push_draw_command(rmlui::DrawData data) {
  push_draw_command(ShaderDrawCommand{.type = ShaderType::Rml, .rml = data});
}
#endif

template <>
PipelineRef pipeline_ref(const gx::PipelineConfig& config) {
  return find_pipeline(ShaderType::GX, config, [=] { return create_pipeline(config); });
}

#ifdef AURORA_ENABLE_RMLUI
template <>
PipelineRef pipeline_ref(const rmlui::PipelineConfig& config) {
  return find_pipeline(ShaderType::Rml, config, [=] { return rmlui::create_pipeline(config); });
}
#endif

static bool ensure_native_geom_cache_buffers();

void initialize() {
  g_frameIndex = 0;
  g_processEventsQueued.store(false, std::memory_order_release);
  g_lastPresentNs.store(0, std::memory_order_release);
  g_presentPeriodNs.store(0, std::memory_order_release);
  g_cpuFrameTimeNs.store(0, std::memory_order_release);
  g_cpuFrameStart = {};
  {
    std::lock_guard lock{g_presentStatsMutex};
    g_presentTimes.clear();
  }
  render_worker::initialize();
  // Normalcy Doctrine rule 3: the render worker owns the GL context for its whole life.
  // device.cpp created it and released it on the main thread; bind it on the worker now
  // (runs inline on this thread in the single-threaded fallback -- either way the thread
  // that will execute GL work is the one that holds the context). Every GL call after this
  // -- buffer/texture creation, draws, present -- happens on the worker. Create the shared
  // per-frame ring buffers, the persistent native-geometry cache, the clear program, and
  // the EFB render targets here, all with the context freshly current.
  render_worker::enqueue_work([] {
    gl::make_render_current();
    // Per-frame ring: one persistent-mapped set per frame slot (double-buffered, fenced on reuse --
    // see FrameBufferSet). select_frame_buffers points the active aliases at slot 0 for the first frame.
    for (auto& set : g_frameBufferSets) {
      set.vertex = gl::create_buffer(gl::GL_ARRAY_BUFFER, VertexBufferSize, true, /*persistent=*/true);
      set.uniform = gl::create_buffer(gl::GL_UNIFORM_BUFFER, UniformBufferSize, true, /*persistent=*/true);
      set.index = gl::create_buffer(gl::GL_ELEMENT_ARRAY_BUFFER, IndexBufferSize, true, /*persistent=*/true);
    }
    select_frame_buffers(0);
    // The storage-fetch path is dead (native vertex fetch only); no g_storageBuffer.
    // Native geom cache is a single shared buffer (all frames read the same hashed offsets, so it
    // cannot be double-buffered) and is only ever appended at fresh offsets, so it stays on the
    // glBufferSubData path -- no persistent-map reuse hazard to fence.
    g_nativeVertexCacheBuffer = gl::create_buffer(gl::GL_ARRAY_BUFFER, NativeGeomVertexCacheSize, true);
    g_nativeIndexCacheBuffer = gl::create_buffer(gl::GL_ELEMENT_ARRAY_BUFFER, NativeGeomIndexCacheSize, true);
    clear::init_program();
    webgpu::create_frame_targets();
  });
  render_worker::synchronize();

  depth_peek::initialize();
  tex_copy_conv::initialize();
  tex_palette_conv::initialize();
  texture_replacement::initialize();

  gx::initialize();
#ifdef AURORA_ENABLE_RMLUI
  rmlui::initialize_pipeline();
#endif
  initialize_pipeline_cache();
}

void shutdown() {
  webgpu::sdl2shim_present::recycle_pending();
  render_worker::synchronize();
  render_worker::shutdown();
  g_processEventsQueued.store(false, std::memory_order_release);
  g_lastPresentNs.store(0, std::memory_order_release);
  g_presentPeriodNs.store(0, std::memory_order_release);
  g_cpuFrameTimeNs.store(0, std::memory_order_release);
  g_cpuFrameStart = {};
  {
    std::lock_guard lock{g_presentStatsMutex};
    g_presentTimes.clear();
  }
  shutdown_pipeline_cache();
  // After the pipeline cache -- its compiler thread is the last caller of gl::compile_program, so no
  // more binary-cache stores can be enqueued once it has joined.
  gl::binary_cache_shutdown();
  depth_peek::shutdown();
  tex_copy_conv::shutdown();
  tex_palette_conv::shutdown();
  texture_replacement::shutdown();
  gx::shutdown();
#ifdef AURORA_ENABLE_RMLUI
  rmlui::shutdown_pipeline();
#endif
  // Teardown drops (gx caches above and anything since the last end_frame) sit in the
  // deferred-destroy list with no frame left to drain them; the context died with the
  // worker, so discard without GL calls.
  (void)take_deferred_texture_destroys();

  {
    std::lock_guard lock{g_bindGroupCacheMutex};
    g_cachedBindGroups.clear();
  }
  {
    std::lock_guard lock{g_samplerCacheMutex};
    g_cachedSamplers.clear();
  }
  {
    std::lock_guard lock{g_runtimeDrawTypeMutex};
    g_runtimeDrawTypes.clear();
    g_freeDrawTypeSlots.clear();
  }
  for (auto& pool : g_passSnapshotPools) {
    pool = {};
  }
  // The GL context is torn down with the worker (above), which reclaims the buffer objects, their
  // persistent mappings, and the fence sync objects; matching the existing single-buffer teardown we
  // only drop the handles here rather than issuing GL after the context is gone.
  for (auto& set : g_frameBufferSets) {
    set = {};
  }
  g_vertexBuffer = {};
  g_uniformBuffer = {};
  g_indexBuffer = {};
  g_storageBuffer = {};
  g_nativeVertexCacheBuffer = {};
  g_nativeIndexCacheBuffer = {};
  s_nativeVertexCacheOffset = 0;
  s_nativeIndexCacheOffset = 0;
  s_nativeGeomCacheResetPending = false;
  ++s_nativeGeomCacheGeneration;
  for (auto& packet : g_framePackets) {
    packet = {};
  }
  g_recordingFrame = nullptr;
  g_currentRenderPass = UINT32_MAX;
  g_offscreenCache.clear();
  g_offscreenColor = {};
  g_offscreenDepth = {};
  g_inOffscreen = false;
  g_frameIndex = UINT32_MAX;
  g_frameSlots.reset();
}

static size_t acquire_frame_slot() {
  ZoneScopedN("Acquire frame slot");
  const auto waitStart = PresentClock::now();
  while (true) {
    if (const auto slot = g_frameSlots.try_acquire()) {
      const auto waitDuration = PresentClock::now() - waitStart;
      const double waitMs = std::chrono::duration<double, std::milli>{waitDuration}.count();
      TracyPlot("aurora: frameSlotWaitMs", waitMs);
      return *slot;
    }
    wait_for_gpu_progress(std::chrono::microseconds{100});
  }
}

bool begin_frame() {
  ZoneScoped;
  // pace_frame_start();
  const size_t frameSlot = acquire_frame_slot();

  auto& frame = g_framePackets[frameSlot];
  frame.reset();
  frame.frameId = g_nextFrameId++;
  frame.frameIndex = g_frameIndex;
  g_recordingFrame = &frame;
  g_recordingFrameSlot = frameSlot;
  g_passSnapshotPools[frameSlot].used = 0;

  if (s_nativeGeomCacheResetPending) {
    // Deferred cache reset: every frame that referenced the old cached ranges has already
    // been submitted; recycling the cache regions from offset 0 cannot race the GPU's reads
    // of the old contents. Bump the generation so CPU-side content maps drop stale entries.
    s_nativeGeomCacheResetPending = false;
    s_nativeVertexCacheOffset = 0;
    s_nativeIndexCacheOffset = 0;
    ++s_nativeGeomCacheGeneration;
  }

  // Per-frame data goes into the packet's ByteBuffers, uploaded to the GL ring buffers with
  // glBufferSubData on the worker. FramePacket::reset() (above) clears them while retaining
  // their fixed-ceiling storage so recording never reallocs under the worker's concurrent reads.

  g_drawCallCount = 0;
  g_mergedDrawCallCount = 0;
  g_suspendedEfbPass.reset();

  current_render_passes().emplace_back();
  auto& pass = current_render_passes()[0];
  pass.label = pass_label("EFB");
  set_efb_targets(pass);
  pass.clearColorValue = gx::g_gxState.clearColor;
  pass.clearDepthValue = gx::clear_depth_value();
  g_currentRenderPass = 0;
  // Refresh render viewport/scissor from logical in case FB size changed
  g_cachedViewport = gx::map_logical_viewport(gx::g_gxState.logicalViewport);
  g_cachedScissor = gx::map_logical_scissor(gx::g_gxState.logicalScissor);
  push_command(CommandType::SetViewport, Command::Data{.setViewport = g_cachedViewport});
  push_command(CommandType::SetScissor, Command::Data{.setScissor = g_cachedScissor});
  begin_pipeline_frame();
  render_worker::enqueue_begin_frame(frame.frameId, [frameSlot] {
    // Reusing this slot's persistent ring buffers: wait until the GPU finished the frame that last
    // used them before the worker starts memcpy'ing new data into the coherent mapping (immutable
    // storage has no implicit orphaning). Then make this slot's set the active ring for the frame.
    wait_frame_buffer_fence(frameSlot);
    select_frame_buffers(frameSlot);
    webgpu::gpu_prof::frame_begin();
  });
  g_cpuFrameStart = PresentClock::now();
  return true;
}

void finish() {
  ZoneScoped;
  if (g_recordingFrame == nullptr) {
    return;
  }
  ASSERT(!g_inOffscreen, "finish called while offscreen rendering is active");
  if (g_currentRenderPass != UINT32_MAX) {
    auto& frame = current_frame_packet();
    frame.uniforms.append_zeroes(gx::MaxUniformSize);
    auto& pass = frame.renderPasses[g_currentRenderPass];
    pass.captureDepthSnapshot = true;
    enqueue_pass(frame, g_recordingFrameSlot, g_currentRenderPass);
    g_currentRenderPass = UINT32_MAX;
  }
}

void end_frame(EndFrameCallback callback) {
  ZoneScoped;
  ASSERT(!g_inOffscreen, "end_frame called while offscreen rendering is active");
  ASSERT(g_currentRenderPass == UINT32_MAX, "end_frame called before finish finalized the current render pass");
  // Dusklight (P4): present the frame the worker finished while we were decoding this one. Doing it
  // here rather than at the top of the frame is what preserves the overlap: by now the worker has
  // had our whole decode to run its Submit, so the wait below is usually near-zero.
  webgpu::sdl2shim_present::flush_present();
  if (g_cpuFrameStart.time_since_epoch().count() != 0) {
    const auto cpuFrameTime = PresentClock::now() - g_cpuFrameStart;
    update_ema(g_cpuFrameTimeNs, duration_ns(cpuFrameTime));
    const double cpuFrameTimeMs = std::chrono::duration<double, std::milli>{cpuFrameTime}.count();
    TracyPlot("aurora: cpuFrameTimeMs", cpuFrameTimeMs);
  }
  auto& frame = current_frame_packet();
  frame.stats.drawCallCount = g_drawCallCount;
  frame.stats.mergedDrawCallCount = g_mergedDrawCallCount;
  frame.stats.lastVertSize = frame.verts.size();
  frame.stats.lastUniformSize = frame.uniforms.size();
  frame.stats.lastIndexSize = frame.indices.size();
  frame.stats.lastStorageSize = frame.storage.size();
  frame.stats.lastTextureUploadSize = frame.textureUpload.size();

  const size_t frameSlot = g_recordingFrameSlot;
  const uint64_t frameId = frame.frameId;
  g_currentRenderPass = UINT32_MAX;
  for (auto& array : gx::g_gxState.arrays) {
    array.cachedRange = {};
  }
  end_pipeline_frame();
  ++g_frameIndex;
  g_recordingFrame = nullptr;

#if defined(AURORA_GFX_DEBUG_GROUPS)
  if (!g_debugGroupStack.empty()) {
    for (auto& it : std::ranges::reverse_view(g_debugGroupStack)) {
      Log.warn("Debug group was not popped at end of frame: {}", it);
    }
    g_debugGroupStack.clear();
  }

  if (g_debugMarkers.size() > 0) {
    g_debugMarkers.clear();
  }
#endif

  // Dusklight (P4): tell the present module a frame is on its way, so the next flush_present()
  // knows to wait for it. Must precede the enqueue, which is what produces that frame.
  webgpu::sdl2shim_present::note_frame_enqueued();
  // Snapshot the GL textures whose last handle dropped during this frame's record.
  // They are destroyed inside the end-of-frame item below -- FIFO-after every pass
  // that could still reference their raw ids (see ~TextureRef).
  render_worker::enqueue_end_frame(frameId, [frameSlot, callback = std::move(callback),
                                             deferredDestroys = take_deferred_texture_destroys()]() mutable {
    for (auto& tex : deferredDestroys) {
      gl::destroy_texture(tex);
    }
    auto& packet = g_framePackets[frameSlot];
    const auto stats = packet.stats;
    // Do NOT `packet = {}` here. Move-assigning a default frees this slot's per-frame ByteBuffers
    // (verts/uniforms/indices, whose reset() pre-reserves 5+24+1 MB), so the next begin_frame's
    // reset() would calloc + page-zero ~30 MB from scratch every frame -- pure memory-bandwidth
    // churn (and heat) on the bandwidth-starved Mali handhelds, defeating the pool's whole point
    // ("recording never reallocs"). The slot is released below and nothing reads the packet until
    // the next begin_frame reset()s it for reuse, which clears the deques + rewinds the ByteBuffers
    // while retaining their capacity. Leaving the stale data resident until then is harmless.
    g_stats.drawCallCount = stats.drawCallCount;
    g_stats.mergedDrawCallCount = stats.mergedDrawCallCount;
    g_stats.lastVertSize = stats.lastVertSize;
    g_stats.lastUniformSize = stats.lastUniformSize;
    g_stats.lastIndexSize = stats.lastIndexSize;
    g_stats.lastStorageSize = stats.lastStorageSize;
    g_stats.lastTextureUploadSize = stats.lastTextureUploadSize;
    // GL present: aurora's command list already sequenced the frame; there is no command
    // buffer to submit. The callback (aurora.cpp) issues the present GL and swaps.
    if (callback) {
      callback();
    }
    // Fence this slot's persistent buffer set: it stays "busy" until the GPU finishes everything
    // issued this frame (its GX draws read the vertex/uniform/index mappings). The next frame that
    // reuses this slot waits this fence before overwriting the mappings (see wait_frame_buffer_fence).
    // Created after the callback so it orders after every GL command this frame emitted.
    if (gl::gl.FenceSync != nullptr) {
      auto& set = g_frameBufferSets[frameSlot];
      if (set.fence != nullptr) {
        gl::gl.DeleteSync(set.fence); // paranoia: normally cleared by the matching wait
      }
      set.fence = gl::gl.FenceSync(gl::GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    }
    g_frameSlots.release(frameSlot);
    expire_cached_bind_groups();
    process_events();
  });
}

uint32_t current_frame() noexcept { return g_frameIndex; }

static void expire_cached_bind_groups() {
  std::lock_guard lock{g_bindGroupCacheMutex};
  if (g_cachedBindGroups.empty() || g_frameIndex == UINT32_MAX || g_frameIndex % BindGroupCacheSweepPeriod != 0) {
    return;
  }

  ZoneScoped;
  for (auto it = g_cachedBindGroups.begin(); it != g_cachedBindGroups.end();) {
    if (g_frameIndex - it->second.lastUsedFrame > BindGroupCacheRetainFrames) {
      g_cachedBindGroups.erase(it++);
    } else {
      ++it;
    }
  }
}

// Upload the dirty per-frame buffer ranges and pending texture uploads this op needs,
// with glBufferSubData / glTexSubImage2D on the render worker (the WebGPU staging-buffer +
// CopyBuffer path is gone). Incremental: only the bytes/uploads added since the previous
// op's high-water are pushed, tracked in frame.copied.
static void upload_op_data(FramePacket& frame, const FrameOp& op) {
  const auto& hw = op.highWater;
  auto& done = frame.copied;
  const auto upload_range = [](const gl::Buffer& buffer, const ByteBuffer& src, uint32_t from, uint32_t to) {
    if (buffer.id == 0 || to <= from) {
      return;
    }
    if (to > buffer.size) {
      Log.warn("upload range end {} exceeds buffer size {}; clamping", to, buffer.size);
      to = static_cast<uint32_t>(buffer.size);
      if (to <= from) {
        return;
      }
    }
    gl::upload_buffer(buffer, from, src.data() + from, to - from);
  };
  upload_range(g_vertexBuffer, frame.verts, done.verts, hw.verts);
  upload_range(g_uniformBuffer, frame.uniforms, done.uniforms, hw.uniforms);
  upload_range(g_indexBuffer, frame.indices, done.indices, hw.indices);
  done.verts = std::max(done.verts, hw.verts);
  done.uniforms = std::max(done.uniforms, hw.uniforms);
  done.indices = std::max(done.indices, hw.indices);

  const size_t uploadStart = done.textureUploadCount;
  for (size_t i = done.textureUploadCount; i < hw.textureUploadCount && i < frame.textureUploads.size(); ++i) {
    const auto& up = frame.textureUploads[i];
    if (up.tex.id == 0) {
      continue;
    }
    const uint8_t* srcData = up.data != nullptr ? up.data : frame.textureUpload.data() + up.range.offset;
    gl::upload_texture(up.tex, up.level, up.origin, up.size, srcData, up.bytesPerRow);
  }
  if (hw.textureUploadCount > uploadStart) {
    // upload_texture binds GL_TEXTURE_2D out-of-band; forget the shadowed texture units.
    gl::invalidate_texture_bindings();
  }
  done.textureUploadCount = std::max(done.textureUploadCount, hw.textureUploadCount);
}

// FBO-to-FBO region copy via glBlitFramebuffer (worker, context current). Both textures
// are attached to cache FBOs as COLOR_ATTACHMENT0. NEAREST + equal src/dst extents == an
// exact texel copy. Full-target snapshots (origin 0, whole texture, flipY=false) are
// orientation-neutral. When flipY is set (the RmlUi SaveLayerAsTexture path), srcOrigin is a
// top-left origin into a GL (bottom-left) source: convert the source band to GL rows and blit
// with a vertical flip so the destination stores the region upright (dst GL row 0 == the
// region's visual-top row), which is what RmlUi's later top-left geometry sampling expects.
static void blit_texture_region(const gl::Texture& src, gl::Origin3D srcOrigin, const gl::Texture& dst,
                                gl::Origin3D dstOrigin, const gl::Extent3D& size, bool flipY) {
  if (src.id == 0 || dst.id == 0 || size.width == 0 || size.height == 0) {
    return;
  }
  const gl::GLuint readFbo = gl::get_framebuffer(src);
  const gl::GLuint drawFbo = gl::get_framebuffer(dst);
  gl::gl.BindFramebuffer(gl::GL_READ_FRAMEBUFFER, readFbo);
  gl::gl.BindFramebuffer(gl::GL_DRAW_FRAMEBUFFER, drawFbo);
  gl::gl.Disable(gl::GL_SCISSOR_TEST);
  const auto srcX0 = static_cast<gl::GLint>(srcOrigin.x);
  const auto srcX1 = static_cast<gl::GLint>(srcOrigin.x + size.width);
  const auto dstX0 = static_cast<gl::GLint>(dstOrigin.x);
  const auto dstX1 = static_cast<gl::GLint>(dstOrigin.x + size.width);
  gl::GLint srcY0, srcY1, dstY0, dstY1;
  if (flipY) {
    const auto srcHeight = static_cast<gl::GLint>(src.size.height);
    srcY0 = srcHeight - static_cast<gl::GLint>(srcOrigin.y) - static_cast<gl::GLint>(size.height);
    srcY1 = srcHeight - static_cast<gl::GLint>(srcOrigin.y);
    dstY0 = static_cast<gl::GLint>(dstOrigin.y + size.height);
    dstY1 = static_cast<gl::GLint>(dstOrigin.y);
  } else {
    srcY0 = static_cast<gl::GLint>(srcOrigin.y);
    srcY1 = static_cast<gl::GLint>(srcOrigin.y + size.height);
    dstY0 = static_cast<gl::GLint>(dstOrigin.y);
    dstY1 = static_cast<gl::GLint>(dstOrigin.y + size.height);
  }
  gl::gl.BlitFramebuffer(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, gl::GL_COLOR_BUFFER_BIT,
                         gl::GL_NEAREST);
  gl::gl.Enable(gl::GL_SCISSOR_TEST);
  gl::gl.BindFramebuffer(gl::GL_FRAMEBUFFER, drawFbo);
}

static void encode_op(FramePacket& frame, const FrameOp& op) {
  upload_op_data(frame, op);
  switch (op.type) {
  case FrameOpType::RenderPass:
    if (op.renderPass != nullptr) {
      render(frame, *op.renderPass, op.index);
    }
    break;
  case FrameOpType::TextureCopy:
    if (op.textureCopy != nullptr) {
      blit_texture_region(op.textureCopy->src.texture, op.textureCopy->src.origin, op.textureCopy->dst.texture,
                          op.textureCopy->dst.origin, op.textureCopy->size, op.textureCopy->flipY);
    }
    break;
  case FrameOpType::EncoderTask:
    if (op.encoderTask != nullptr) {
      execute_encoder_task(*op.encoderTask);
    }
    break;
  }
}

static void render(FramePacket& frame, RenderPass& passInfo, uint32_t passIndex) {
  ZoneScoped;
  (void)passIndex;
  if (!passInfo.sealed) {
    return;
  }
  // Palette conversions render into their own destination textures (a later draw in THIS
  // pass may sample them), so they run before -- and independently of -- this pass's own
  // attachment, even for a discardable pass.
  for (const auto& conv : passInfo.paletteConvs) {
    tex_palette_conv::run(conv);
  }
  if (passInfo.discardable) {
    return;
  }
  // A pass with no color attachment is a genuinely empty EFB segment (offscreen / EFB-copy
  // targets now carry real attachments); nothing to render into.
  if (passInfo.colorView.id == 0) {
    return;
  }

  const bool hasDepth = passInfo.hasDepth && passInfo.depthStencilView.id != 0;
  const gl::GLuint fbo = gl::get_framebuffer(passInfo.colorView, hasDepth ? &passInfo.depthStencilView : nullptr);
  gl::gl.BindFramebuffer(gl::GL_FRAMEBUFFER, fbo);

  // Load ops (S7). A WebGPU LoadOp::Clear clears the whole render area regardless of the
  // scissor, so disable the scissor test and force full write masks around the clears, then
  // re-enable scissor for the draws.
  gl::gl.Disable(gl::GL_SCISSOR_TEST);
  if (passInfo.clearColor) {
    gl::gl.ColorMask(gl::GL_TRUE, gl::GL_TRUE, gl::GL_TRUE, gl::GL_TRUE);
    const gl::GLfloat clearColor[4]{passInfo.clearColorValue.x(), passInfo.clearColorValue.y(),
                                    passInfo.clearColorValue.z(), passInfo.clearColorValue.w()};
    gl::gl.ClearBufferfv(gl::GL_COLOR, 0, clearColor);
  }
  if (hasDepth && passInfo.clearDepth) {
    gl::gl.DepthMask(gl::GL_TRUE);
    if (passInfo.hasStencil) {
      gl::gl.ClearBufferfi(gl::GL_DEPTH_STENCIL, 0, passInfo.clearDepthValue,
                           static_cast<gl::GLint>(passInfo.stencilClearValue));
    } else {
      const gl::GLfloat clearDepth = passInfo.clearDepthValue;
      gl::gl.ClearBufferfv(gl::GL_DEPTH, 0, &clearDepth);
    }
  }
  gl::gl.Enable(gl::GL_SCISSOR_TEST);

  // The raw clears desynced color/depth mask + scissor from the state-cache shadow; reset it
  // so the first replayed draw re-issues its full baked pipeline state.
  gl::reset_state_cache();

  gl::PassEncoder pass{gl::PassTarget{
      .fbo = fbo,
      .width = passInfo.targetSize.width,
      .height = passInfo.targetSize.height,
  }};
  render_pass(pass, frame, passInfo);
  pass.End();

  // Store ops (S7): discard attachments explicitly marked StoreOp::Discard. On a tiler this
  // is a large depth/color bandwidth win, but only sound when the attachment is genuinely not
  // loaded again -- so it fires solely on an explicit Discard (never inferred). The EFB copy
  // machinery (Phase 4) sets Discard on the frame's terminal pass.
  std::array<gl::GLenum, 2> invalidate{};
  gl::GLsizei invalidateCount = 0;
  if (passInfo.colorStoreOp == gl::StoreOp::Discard) {
    invalidate[invalidateCount++] = gl::GL_COLOR_ATTACHMENT0;
  }
  if (hasDepth && passInfo.depthStoreOp == gl::StoreOp::Discard) {
    invalidate[invalidateCount++] = passInfo.hasStencil ? gl::GL_DEPTH_STENCIL_ATTACHMENT : gl::GL_DEPTH_ATTACHMENT;
  }
  if (invalidateCount > 0 && gl::gl.InvalidateFramebuffer != nullptr) {
    gl::gl.InvalidateFramebuffer(gl::GL_FRAMEBUFFER, invalidateCount, invalidate.data());
  }

  // EFB resolve: copy (and format-convert / scale) this pass's color into the GX texture
  // the game asked for. The crop UV transform lives in resolveUniformRange (S1c); the
  // conversion / passthrough shaders render into resolveTarget's FBO.
  if (passInfo.resolveTarget) {
    const auto& dstSize = passInfo.resolveTarget->size;
    const bool needsConversion = tex_copy_conv::needs_conversion(passInfo.resolveFormat);
    const bool needsScaling = dstSize.width != static_cast<uint32_t>(passInfo.resolveRect.width) ||
                              dstSize.height != static_cast<uint32_t>(passInfo.resolveRect.height);
    const bool isDepth = gx::is_depth_format(passInfo.resolveFormat);
    if (isDepth && passInfo.msaaSamples > 1) {
      Log.fatal("Depth tex copies from multisampled EFB targets are not supported");
    }
    const tex_copy_conv::ConvRequest convReq{
        .fmt = passInfo.resolveFormat,
        .srcView = isDepth ? passInfo.copySourceDepthView : passInfo.copySourceView,
        .uniformRange = passInfo.resolveUniformRange,
        .dst = passInfo.resolveTarget,
        .sampleFilter = needsScaling ? tex_copy_conv::SampleFilter::Linear : tex_copy_conv::SampleFilter::Nearest,
    };
    if (needsConversion) {
      tex_copy_conv::run(convReq);
    } else {
      // No format conversion: the passthrough blit handles both the scaled and the exact
      // 1:1 case (Dawn special-cased 1:1 with CopyTextureToTexture; the shader blit is
      // equivalent and keeps the crop/orientation handling in one place).
      tex_copy_conv::blit(convReq);
    }
  }

  // Full-target color snapshot for the public resolve_pass API (off the live TP path;
  // depth snapshot stays gated in tex_copy_conv::snapshot_depth).
  if (passInfo.snapshotColorDst.id != 0) {
    blit_texture_region(passInfo.copySourceTexture, gl::Origin3D{}, passInfo.snapshotColorDst, gl::Origin3D{},
                        passInfo.targetSize, /*flipY=*/false);
  }
}

void after_submit() noexcept { depth_peek::after_submit(); }

void gpu_synchronize() {
  // The worker can be parked waiting for the main thread to release an EFB slot. Nothing else will
  // release it while we block here, so drop whatever it is holding out to us first.
  webgpu::sdl2shim_present::recycle_pending();
  render_worker::synchronize();
}

void synchronize() { render_worker::synchronize(); }

void after_present() noexcept {
  const auto now = PresentClock::now();
  const int64_t nowNs = timestamp_ns(now);
  const int64_t previousPresentNs = g_lastPresentNs.exchange(nowNs, std::memory_order_acq_rel);
  if (previousPresentNs != 0) {
    update_ema(g_presentPeriodNs, nowNs - previousPresentNs);
    const double presentPeriodMs = static_cast<double>(g_presentPeriodNs.load(std::memory_order_acquire)) / 1'000'000.0;
    TracyPlot("aurora: presentPeriodMs", presentPeriodMs);
  }
  std::lock_guard lock{g_presentStatsMutex};
  g_presentTimes.push_back(now);
  prune_present_times(now);
}

float calculate_fps() noexcept {
  const auto now = PresentClock::now();
  std::lock_guard lock{g_presentStatsMutex};
  prune_present_times(now);
  if (g_presentTimes.size() < 2) {
    return 0.f;
  }

  const auto elapsed = std::chrono::duration<float>(g_presentTimes.back() - g_presentTimes.front()).count();
  if (elapsed <= 0.f) {
    return 0.f;
  }
  return static_cast<float>(g_presentTimes.size() - 1) / elapsed;
}

static void apply_viewport(gl::PassEncoder& pass, const Viewport& vp) {
  const float minDepth = gx::UseReversedZ ? 1.f - vp.zfar : vp.znear;
  const float maxDepth = gx::UseReversedZ ? 1.f - vp.znear : vp.zfar;
  pass.SetViewport(vp.left, vp.top, vp.width, vp.height, minDepth, maxDepth);
}

static void apply_scissor(gl::PassEncoder& pass, const ClipRect& sc, const gl::Extent3D& size) {
  const auto x = std::clamp(static_cast<uint32_t>(sc.x), 0u, size.width);
  const auto y = std::clamp(static_cast<uint32_t>(sc.y), 0u, size.height);
  const auto w = std::clamp(static_cast<uint32_t>(sc.width), 0u, size.width - x);
  const auto h = std::clamp(static_cast<uint32_t>(sc.height), 0u, size.height - y);
  pass.SetScissorRect(x, y, w, h);
}

static DrawContext make_draw_context(const RenderPass& passInfo) {
  return {
      .vertexBuffer = g_vertexBuffer.id,
      .indexBuffer = g_indexBuffer.id,
      .uniformBuffer = g_uniformBuffer.id,
      .colorFormat = static_cast<uint32_t>(webgpu::g_graphicsConfig.surfaceConfiguration.format),
      .depthFormat = static_cast<uint32_t>(webgpu::g_graphicsConfig.depthFormat),
      .sampleCount = passInfo.msaaSamples,
      .targetWidth = passInfo.targetSize.width,
      .targetHeight = passInfo.targetSize.height,
  };
}

static void render_custom_draw(const CustomDrawCommand& draw, gl::PassEncoder& pass, const RenderPass& passInfo) {
  RuntimeDrawType drawType;
  {
    std::lock_guard lock{g_runtimeDrawTypeMutex};
    const auto* slot = find_runtime_draw_type(draw.type);
    if (slot == nullptr) {
      // Unregistered between record and replay; the command is a no-op.
      return;
    }
    drawType = *slot;
  }

  const auto context = make_draw_context(passInfo);
  drawType.draw(context, pass, draw.payload.data(), draw.payloadSize, drawType.userdata);
}

static void execute_encoder_task(const EncoderTask& task) {
  RuntimeEncoderTaskType taskType;
  {
    std::lock_guard lock{g_runtimeDrawTypeMutex};
    const auto* slot = find_runtime_encoder_task_type(task.type);
    if (slot == nullptr) {
      // Unregistered between record and encode; the task is a no-op.
      return;
    }
    taskType = *slot;
  }

  const EncoderTaskContext context{
      .vertexBuffer = g_vertexBuffer.id,
      .indexBuffer = g_indexBuffer.id,
      .uniformBuffer = g_uniformBuffer.id,
  };
  taskType.callback(context, task.payload.data(), task.payloadSize, taskType.userdata);
}

static void render_pass(gl::PassEncoder& pass, FramePacket& frame, const RenderPass& passInfo) {
  ZoneScoped;
  g_currentPipeline = UINTPTR_MAX;
#ifdef AURORA_GFX_DEBUG_GROUPS
  std::vector<std::string> lastDebugGroupStack;
#endif
  Viewport currentViewport{};
  ClipRect currentScissor{};
  bool hasViewport = false;
  bool hasScissor = false;

  // Native shaders don't use group 0 (the dead storage-fetch path is gone); bind only the
  // empty texture set (group 2) so unused GX texture units sample a 1x1 fill.
  pass.SetBindGroup(2, gx::g_emptyTextureBindGroup);

  for (const auto& cmd : passInfo.commands) {
#ifdef AURORA_GFX_DEBUG_GROUPS
    {
      size_t firstDiff = lastDebugGroupStack.size();
      for (size_t i = 0; i < lastDebugGroupStack.size(); ++i) {
        if (i >= cmd.debugGroupStack.size() || cmd.debugGroupStack[i] != lastDebugGroupStack[i]) {
          firstDiff = i;
          break;
        }
      }
      for (size_t i = firstDiff; i < lastDebugGroupStack.size(); ++i) {
        pass.PopDebugGroup();
      }
      for (size_t i = firstDiff; i < cmd.debugGroupStack.size(); ++i) {
        pass.PushDebugGroup(cmd.debugGroupStack[i].c_str());
      }
      lastDebugGroupStack = cmd.debugGroupStack;
    }
#endif
    switch (cmd.type) {
    case CommandType::SetViewport: {
      const auto& vp = cmd.data.setViewport;
      apply_viewport(pass, vp);
      currentViewport = vp;
      hasViewport = true;
    } break;
    case CommandType::SetScissor: {
      const auto& sc = cmd.data.setScissor;
      apply_scissor(pass, sc, passInfo.targetSize);
      currentScissor = sc;
      hasScissor = true;
    } break;
    case CommandType::Draw: {
      const auto& draw = cmd.data.draw;
      switch (draw.type) {
      case ShaderType::Clear:
        clear::render(draw.clear, pass, passInfo.targetSize);
        break;
      case ShaderType::GX:
        gx::render(draw.gx, pass);
        break;
#ifdef AURORA_ENABLE_RMLUI
      case ShaderType::Rml:
        rmlui::render(draw.rml, pass);
        break;
#endif
      }
    } break;
    case CommandType::CustomDraw: {
      render_custom_draw(cmd.data.customDraw, pass, passInfo);
      g_currentPipeline = UINTPTR_MAX;
      pass.SetBindGroup(2, gx::g_emptyTextureBindGroup);
      if (hasViewport) {
        apply_viewport(pass, currentViewport);
      }
      if (hasScissor) {
        apply_scissor(pass, currentScissor, passInfo.targetSize);
      }
    } break;
    case CommandType::DebugMarker: {
#if defined(AURORA_GFX_DEBUG_GROUPS)
      pass.InsertDebugMarker(wgpu::StringView(g_debugMarkers[cmd.data.debugMarkerIndex]));
#endif
    } break;
    }
  }

#ifdef AURORA_GFX_DEBUG_GROUPS
  for (size_t i = 0; i < lastDebugGroupStack.size(); ++i) {
    pass.PopDebugGroup();
  }
#endif
}

void render_pass(gl::PassEncoder& pass, u32 idx) {
  auto& frame = current_frame_packet();
  render_pass(pass, frame, frame.renderPasses[idx]);
}

bool bind_pipeline(PipelineRef ref, gl::PassEncoder& pass) {
  if (ref == g_currentPipeline) {
    return true;
  }
  gl::Pipeline pipeline;
  if (!get_pipeline(ref, pipeline)) {
    return false;
  }
  pass.SetPipeline(pipeline);
  g_currentPipeline = ref;
  return true;
}

static Range push(ByteBuffer& target, const uint8_t* data, size_t length, size_t alignment) {
  if (alignment != 0) {
    const size_t begin = target.size();
    const size_t alignedBegin = AURORA_ALIGN(begin, alignment);
    if (alignedBegin > begin) {
      target.append_zeroes(alignedBegin - begin);
    }
  }
  const auto begin = target.size();
  if (length > 0) {
    target.append(data, length);
  }
  return {static_cast<uint32_t>(begin), static_cast<uint32_t>(length)};
}

static Range map(ByteBuffer& target, size_t length, size_t alignment) {
  if (alignment != 0) {
    const size_t begin = target.size();
    const size_t alignedBegin = AURORA_ALIGN(begin, alignment);
    if (alignedBegin > begin) {
      target.append_zeroes(alignedBegin - begin);
    }
  }
  auto begin = target.size();
  if (length > 0) {
    target.append_zeroes(length);
  }
  return {static_cast<uint32_t>(begin), static_cast<uint32_t>(length)};
}

// For our public API, warn instead of fatal-ing when called outside an active recording frame.
static bool check_recording(const char* name) {
  if (g_recordingFrame == nullptr)
    UNLIKELY {
      Log.warn("{}: called outside an active frame", name);
      return false;
    }
  return true;
}

Range push_verts(const uint8_t* data, size_t length, size_t alignment) {
  ZoneScoped;
  if (!check_recording("push_verts")) {
    return {};
  }
  return push(current_frame_packet().verts, data, length, alignment);
}

Range push_indices(const uint8_t* data, size_t length, size_t alignment) {
  ZoneScoped;
  if (!check_recording("push_indices")) {
    return {};
  }
  return push(current_frame_packet().indices, data, length, alignment);
}

static bool ensure_native_geom_cache_buffers() {
  // The persistent native-geometry GL cache buffers are created on the worker in
  // initialize(). If they exist, push_native_cached_* uploads into them; otherwise the
  // caller falls back to the per-frame ring (e.g. the single-threaded pre-init window).
  return g_nativeVertexCacheBuffer && g_nativeIndexCacheBuffer;
}

// Bump-allocate `length` bytes from a persistent cache buffer and upload the data on the
// render worker (FIFO-ordered after prior submits, so it never races a prior frame's reads).
static std::pair<Range, bool> push_native_cached(const gl::Buffer& buffer, uint32_t& cursor, uint64_t limit,
                                                 const uint8_t* data, size_t length) {
  const uint32_t offset = static_cast<uint32_t>(AURORA_ALIGN(cursor, 4));
  const size_t alignedSize = AURORA_ALIGN(length, 4);
  if (static_cast<uint64_t>(offset) + alignedSize > limit) {
    return {{}, false};
  }
  // The GL upload must run on the render worker (it owns the context). The copy keeps the
  // data alive until the worker executes the glBufferSubData.
  std::vector<uint8_t> copy(alignedSize);
  memcpy(copy.data(), data, length);
  render_worker::enqueue_work(
      [buffer, offset, copy = std::move(copy)] { gl::upload_buffer(buffer, offset, copy.data(), copy.size()); });
  cursor = offset + static_cast<uint32_t>(alignedSize);
  return {Range{offset, static_cast<uint32_t>(length)}, true};
}

std::pair<Range, bool> push_native_cached_verts(const uint8_t* data, size_t length) {
  if (!ensure_native_geom_cache_buffers()) {
    return {{}, false};
  }
  return push_native_cached(g_nativeVertexCacheBuffer, s_nativeVertexCacheOffset, NativeGeomVertexCacheSize, data,
                            length);
}

std::pair<Range, bool> push_native_cached_indices(const uint8_t* data, size_t length) {
  if (!ensure_native_geom_cache_buffers()) {
    return {{}, false};
  }
  return push_native_cached(g_nativeIndexCacheBuffer, s_nativeIndexCacheOffset, NativeGeomIndexCacheSize, data, length);
}

void request_native_geometry_cache_reset() noexcept { s_nativeGeomCacheResetPending = true; }
uint32_t native_geom_cache_generation() noexcept { return s_nativeGeomCacheGeneration; }

Range push_uniform(const uint8_t* data, size_t length) {
  ZoneScoped;
  if (!check_recording("push_uniform")) {
    return {};
  }
  return push(current_frame_packet().uniforms, data, length, webgpu::g_uniformBufferOffsetAlignment);
}

Range push_storage(const uint8_t* data, size_t length) {
  ZoneScoped;
  if (!check_recording("push_storage")) {
    return {};
  }
  // The storage-fetch path is dead (native vertex fetch only); this remains for API
  // completeness. Reuse the uniform alignment.
  return push(current_frame_packet().storage, data, length, webgpu::g_uniformBufferOffsetAlignment);
}

Range push_texture_data(const uint8_t* data, u32 bytesPerRow, u32 rowsPerImage) {
  // For CopyBufferToTexture, we need an alignment of 256 per row (see Dawn kTextureBytesPerRowAlignment)
  const auto copyBytesPerRow = AURORA_ALIGN(bytesPerRow, 256);
  const auto range = map(current_frame_packet().textureUpload, copyBytesPerRow * rowsPerImage, 0);
  u8* dst = current_frame_packet().textureUpload.data() + range.offset;
  for (u32 i = 0; i < rowsPerImage; ++i) {
    memcpy(dst, data, bytesPerRow);
    data += bytesPerRow;
    dst += copyBytesPerRow;
  }
  return range;
}

BindGroupRef bind_group_ref(const gl::BindingSet& bindingSet) {
  // Hash the POD binding set (callers zero-init it, so trailing padding is deterministic).
  const auto id = xxh3_hash_s(&bindingSet, sizeof(bindingSet));
  std::lock_guard lock{g_bindGroupCacheMutex};
  const auto it = g_cachedBindGroups.find(id);
  if (it == g_cachedBindGroups.end()) {
    g_cachedBindGroups.emplace(id, CachedBindGroup{.bindGroup = bindingSet, .lastUsedFrame = g_frameIndex});
  } else {
    it->second.lastUsedFrame = g_frameIndex;
  }
  return id;
}

const gl::BindingSet& find_bind_group(BindGroupRef id) {
  std::lock_guard lock{g_bindGroupCacheMutex};
  const auto it = g_cachedBindGroups.find(id);
  CHECK(it != g_cachedBindGroups.end(), "get_bind_group: failed to locate {:x}", id);
  return it->second.bindGroup;
}

gl::Texture create_gl_texture(gl::TextureFormat format, gl::Extent3D size, uint32_t mips, bool renderable) {
  gl::Texture result;
  render_worker::enqueue_work([&] {
    result = gl::create_texture(format, size, mips, renderable);
    // create_texture binds GL_TEXTURE_2D directly; drop the state cache's texture-unit
    // shadow so the next draw re-binds.
    gl::invalidate_texture_bindings();
  });
  render_worker::synchronize();
  return result;
}

gl::Sampler create_gl_sampler(const gl::SamplerDescriptor& descriptor) {
  gl::Sampler result;
  const bool anisotropySupported = webgpu::g_graphicsConfig.textureAnisotropy > 0;
  render_worker::enqueue_work([&] { result = gl::create_sampler(descriptor, anisotropySupported); });
  render_worker::synchronize();
  return result;
}

gl::Sampler sampler_ref(const gl::SamplerDescriptor& descriptor) {
  // Hash explicit fields, never the raw struct: SamplerDescriptor mixes uint8_t enums
  // with floats, so it has padding bytes. Hashing stack garbage in the padding made
  // every lookup unique -- the cache never hit and each textured draw minted a fresh
  // GL sampler (~50k live samplers in minutes; the G31 GPU-memory OOM).
  const uint32_t key[4] = {
      static_cast<uint32_t>(descriptor.addressU) | static_cast<uint32_t>(descriptor.addressV) << 8 |
          static_cast<uint32_t>(descriptor.addressW) << 16 | static_cast<uint32_t>(descriptor.magFilter) << 24,
      static_cast<uint32_t>(descriptor.minFilter) | static_cast<uint32_t>(descriptor.mipmapFilter) << 8 |
          static_cast<uint32_t>(descriptor.maxAnisotropy) << 16,
      std::bit_cast<uint32_t>(descriptor.lodMinClamp),
      std::bit_cast<uint32_t>(descriptor.lodMaxClamp),
  };
  const auto id = xxh3_hash_s(key, sizeof(key));
  {
    std::lock_guard lock{g_samplerCacheMutex};
    if (const auto it = g_cachedSamplers.find(id); it != g_cachedSamplers.end()) {
      return it->second;
    }
  }
  // Create outside the lock (the worker marshal blocks); a rare create-create race just
  // leaks one GL sampler, which is benign.
  const gl::Sampler sampler = create_gl_sampler(descriptor);
  std::lock_guard lock{g_samplerCacheMutex};
  return g_cachedSamplers.try_emplace(id, sampler).first->second;
}

uint32_t align_uniform(uint32_t value) { return AURORA_ALIGN(value, webgpu::g_uniformBufferOffsetAlignment); }

void insert_debug_marker(std::string label) {
#if defined(AURORA_GFX_DEBUG_GROUPS)
  auto idx = g_debugMarkers.size();
  g_debugMarkers.emplace_back(std::move(label));
  push_command(CommandType::DebugMarker, {.debugMarkerIndex = idx});
#endif
}

} // namespace aurora::gfx

void aurora::gfx::push_debug_group(std::string label) {
#if defined(AURORA_GFX_DEBUG_GROUPS)
  g_debugGroupStack.push_back(std::move(label));
#endif
}
void push_debug_group(const char* label) {
#ifdef AURORA_GFX_DEBUG_GROUPS
  aurora::gfx::g_debugGroupStack.emplace_back(label);
#endif
}
void pop_debug_group() {
#ifdef AURORA_GFX_DEBUG_GROUPS
  if (aurora::gfx::g_debugGroupStack.empty()) {
    aurora::gfx::Log.error("Debug group stack underflowed!");
    return;
  }

  aurora::gfx::g_debugGroupStack.pop_back();
#endif
}

const AuroraStats* aurora_get_stats() { return &aurora::gfx::g_stats; }
void aurora_get_pipeline_counts(uint32_t* queued, uint32_t* created) {
  if (queued != nullptr) {
    *queued = aurora::gfx::g_stats.queuedPipelines;
  }
  if (created != nullptr) {
    *created = aurora::gfx::g_stats.createdPipelines;
  }
}
float aurora_get_fps() { return aurora::gfx::calculate_fps(); }

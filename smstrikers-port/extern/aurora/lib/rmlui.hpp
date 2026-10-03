#pragma once

#include "aurora/aurora.h"
#include "gfx/common.hpp"
#include "webgpu/gpu.hpp"

#include <SDL3/SDL_events.h>
#include <aurora/rmlui.hpp>

namespace aurora::rmlui {

struct RecordedFrame {
  gl::Texture texture;   // the composited UI target sampled by the present overlay (id 0 = nothing recorded)
  gl::Sampler sampler;   // its filtering sampler
  bool overlay = false;  // true = blend the UI over the scene; false = UI already seeded the scene (opaque)
};

void initialize(const AuroraWindowSize& size) noexcept;
void handle_event(SDL_Event& event) noexcept;
RecordedFrame record_frame(const webgpu::Viewport& presentViewport) noexcept;
void shutdown() noexcept;

} // namespace aurora::rmlui

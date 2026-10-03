#pragma once

#include <dolphin/gx/GXStruct.h>

#include <cstdint>

#include <aurora/math.hpp>

namespace aurora::vi {
void configure(const GXRenderModeObj* rm) noexcept;
Vec2<uint32_t> configured_fb_size() noexcept;
float locked_aspect() noexcept;
} // namespace aurora::vi

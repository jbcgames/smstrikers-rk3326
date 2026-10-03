#include "census.hpp"

#include "../internal.hpp"

namespace aurora::gl::census {
namespace {
Module Log("aurora::gl");

double mb(const Counter& c) { return static_cast<double>(c.bytes.load(std::memory_order_relaxed)) / (1024.0 * 1024.0); }
} // namespace

Counter textures;
Counter buffers;
Counter programs;
Counter fbos;
Counter samplers;

void log_final() noexcept {
  Log.info("[gl-census] tex {} ({:.1f} MB) buf {} ({:.1f} MB) prog {} fbo {} smp {}",
           textures.count.load(std::memory_order_relaxed), mb(textures),
           buffers.count.load(std::memory_order_relaxed), mb(buffers),
           programs.count.load(std::memory_order_relaxed), fbos.count.load(std::memory_order_relaxed),
           samplers.count.load(std::memory_order_relaxed));
}

} // namespace aurora::gl::census

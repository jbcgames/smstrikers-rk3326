#include "backend.hpp"

#include "context.hpp"
#include "gl_core.hpp"

#include "../internal.hpp"

namespace aurora::gl {
namespace {
Module Log("aurora::gl");
} // namespace

bool smoke(void* sdlWindow) {
  Log.info("[gl] smoke: bringing up a desktop GL context beside the live backend");
  ContextConfig cfg{};
  cfg.mode = ContextMode::Desktop;
  cfg.sdlWindow = sdlWindow;
  if (!create_contexts(cfg)) {
    Log.error("[gl] smoke: context/loader bring-up FAILED (window needs SDL_WINDOW_OPENGL)");
    return false;
  }
  Log.info("[gl] smoke: proc table {}", loaded() ? "complete" : "INCOMPLETE");
  destroy_contexts();
  return true;
}

} // namespace aurora::gl

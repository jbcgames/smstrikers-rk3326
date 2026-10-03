#include "imgui.hpp"

#include <cstddef>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_render.h>

#include "internal.hpp"
#include "gfx/render_worker.hpp"
#include "gl/gl_core.hpp"
#include "gl/pass.hpp"
#include "gl/state.hpp"
#include "gl/textures.hpp"
#include "webgpu/gpu.hpp"
#include "window.hpp"

// The GLES imgui backend is imgui_impl_opengl3 driven with a "#version 300 es" init
// string. Its GL objects live on the render worker's context, so init/shutdown/render
// and user-texture uploads are all marshaled there. The SDL_Renderer path stays for the
// headless/NULL backend (no GL context).
#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlrenderer3.h"
#include "tracy/Tracy.hpp"

namespace aurora::imgui {
static float g_scale;
static std::string g_imguiSettings{};
static std::string g_imguiLog{};
static bool g_useSdlRenderer = false;

static std::vector<SDL_Texture*> g_sdlTextures;
static std::vector<gl::Texture> g_glTextures;

struct DrawData::Impl {
  ImDrawData drawData;
  std::vector<std::unique_ptr<ImDrawList>> drawLists;
};

void create_context() noexcept {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  g_imguiSettings = std::string{g_config.userPath} + "/imgui.ini";
  g_imguiLog = std::string{g_config.cachePath} + "/imgui.log";
  io.IniFilename = g_imguiSettings.c_str();
  io.LogFilename = g_imguiLog.c_str();
}

void initialize() noexcept {
  ZoneScoped;
  SDL_Renderer* renderer = window::get_sdl_renderer();
  ImGui_ImplSDL3_InitForSDLRenderer(window::get_sdl_window(), renderer);
  g_useSdlRenderer = renderer != nullptr;
  if (g_useSdlRenderer) {
    ImGui_ImplSDLRenderer3_Init(renderer);
  } else {
    // The imgui GL objects (shaders + font atlas texture) must be created on the render
    // worker's context, so init there and force device-object creation (NewFrame lazily
    // builds them) before the first real frame renders on the worker.
    gfx::render_worker::enqueue_work([] {
      ImGui_ImplOpenGL3_Init("#version 300 es");
      ImGui_ImplOpenGL3_NewFrame();
      gl::invalidate_texture_bindings();
    });
    gfx::render_worker::synchronize();
  }
}

void shutdown() noexcept {
  ZoneScoped;
  if (g_useSdlRenderer) {
    ImGui_ImplSDLRenderer3_Shutdown();
  } else {
    gfx::render_worker::enqueue_work([] { ImGui_ImplOpenGL3_Shutdown(); });
    gfx::render_worker::synchronize();
  }
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
  for (const auto& texture : g_sdlTextures) {
    SDL_DestroyTexture(texture);
  }
  g_sdlTextures.clear();
  g_glTextures.clear();
}

void process_event(const SDL_Event& event) noexcept {
  auto renderEvent = event;
  if (g_useSdlRenderer) {
    if (SDL_Renderer* renderer = window::get_sdl_renderer()) {
      SDL_ConvertEventToRenderCoordinates(renderer, &renderEvent);
    }
  }
  ImGui_ImplSDL3_ProcessEvent(&renderEvent);
}

bool wants_capture_event(const SDL_Event& event) noexcept {
  if (ImGui::GetCurrentContext() == nullptr) {
    return false;
  }

  const ImGuiIO& io = ImGui::GetIO();
  switch (event.type) {
  case SDL_EVENT_MOUSE_MOTION:
  case SDL_EVENT_MOUSE_BUTTON_DOWN:
  case SDL_EVENT_MOUSE_BUTTON_UP:
  case SDL_EVENT_MOUSE_WHEEL:
  case SDL_EVENT_FINGER_DOWN:
  case SDL_EVENT_FINGER_MOTION:
  case SDL_EVENT_FINGER_UP:
  case SDL_EVENT_FINGER_CANCELED:
    return io.WantCaptureMouse;
  case SDL_EVENT_KEY_DOWN:
  case SDL_EVENT_KEY_UP:
  case SDL_EVENT_TEXT_INPUT:
    return io.WantCaptureKeyboard || io.WantTextInput;
  default:
    return false;
  }
}

void new_frame(const AuroraWindowSize& size) noexcept {
  ZoneScoped;
  ImVec2 framebufferScale{
      size.width > 0 ? static_cast<float>(size.native_fb_width) / static_cast<float>(size.width) : 1.0f,
      size.height > 0 ? static_cast<float>(size.native_fb_height) / static_cast<float>(size.height) : 1.0f,
  };
  ImVec2 displaySize{static_cast<float>(size.width), static_cast<float>(size.height)};

  if (g_useSdlRenderer) {
    if (SDL_Renderer* renderer = window::get_sdl_renderer()) {
      float renderScaleX = 1.0f;
      float renderScaleY = 1.0f;
      SDL_GetRenderScale(renderer, &renderScaleX, &renderScaleY);
      if (renderScaleX > 0.0f && renderScaleY > 0.0f &&
          (std::fabs(renderScaleX - 1.0f) > 0.0001f || std::fabs(renderScaleY - 1.0f) > 0.0001f)) {
        int outputWidth = static_cast<int>(size.native_fb_width);
        int outputHeight = static_cast<int>(size.native_fb_height);
        SDL_GetRenderOutputSize(renderer, &outputWidth, &outputHeight);
        displaySize = {
            static_cast<float>(outputWidth) / renderScaleX,
            static_cast<float>(outputHeight) / renderScaleY,
        };
        framebufferScale = {renderScaleX, renderScaleY};
      }
    }
    ImGui_ImplSDLRenderer3_NewFrame();
    g_scale = size.scale;
  } else {
    // The game rebuilds the imgui font atlas whenever the UI scale changes
    // (dusk::ImGuiEngine_Initialize -> io.Fonts->Clear() + re-add fonts, fired on window resize),
    // which invalidates the atlas: Fonts->IsBuilt() goes false and the ImFont objects are recreated.
    // The backend font texture must then be rebuilt, or the next ImGui::NewFrame dereferences a font
    // whose ContainerAtlas is gone (ContainerAtlas == nullptr -> crash; hit on device when the window
    // resizes 320x320 -> 304x224). The old Dawn path rebuilt here too. The rebuild issues GL, so it
    // must run where the render context is current -- the render worker (the main thread has no GL
    // context on desktop, and only the shim's present context on device), same as init above.
    if ((g_scale > 0.f && g_scale != size.scale) || !ImGui::GetIO().Fonts->IsBuilt()) {
      gfx::render_worker::enqueue_work([] {
        ImGui_ImplOpenGL3_DestroyFontsTexture();
        ImGui_ImplOpenGL3_CreateFontsTexture();
        gl::invalidate_texture_bindings();
      });
      gfx::render_worker::synchronize();
    }
    g_scale = size.scale;
    ImGui_ImplOpenGL3_NewFrame();
  }
  ImGui_ImplSDL3_NewFrame();

  ImGuiIO& io = ImGui::GetIO();
  io.DisplayFramebufferScale = framebufferScale;
  ImGui::GetIO().DisplaySize = displaySize;
  ImGui::NewFrame();
}

DrawData freeze() noexcept {
  ZoneScoped;
  ImGui::Render();

  auto* data = ImGui::GetDrawData();
  data->FramebufferScale = ImGui::GetIO().DisplayFramebufferScale;
  auto frozen = std::make_shared<DrawData::Impl>();
  frozen->drawData = *data;
  frozen->drawLists.reserve(data->CmdListsCount);
  frozen->drawData.CmdLists.resize(data->CmdListsCount);
  for (int i = 0; i < data->CmdListsCount; ++i) {
    frozen->drawLists.emplace_back(data->CmdLists[i]->CloneOutput());
    frozen->drawData.CmdLists[i] = frozen->drawLists.back().get();
  }
  return DrawData{std::move(frozen)};
}

void render(gl::PassEncoder& pass, const DrawData& drawData) noexcept {
  ZoneScoped;

  if (!drawData.m_impl) {
    return;
  }
  auto* data = &drawData.m_impl->drawData;
  if (data->CmdListsCount == 0) {
    return;
  }
  if (g_useSdlRenderer) {
    SDL_Renderer* renderer = window::get_sdl_renderer();
    SDL_RenderClear(renderer);
    ImGui_ImplSDLRenderer3_RenderDrawData(data, renderer);
    SDL_RenderPresent(renderer);
  } else {
    // Runs on the render worker (end-frame callback). Draw over the bound framebuffer (the
    // window's default framebuffer, on top of the presented scene + RmlUi overlay).
    // ImGui_ImplOpenGL3_RenderDrawData sets its own viewport/program/blend from the draw data.
    gl::gl.BindFramebuffer(gl::GL_FRAMEBUFFER, pass.target().fbo);
    ImGui_ImplOpenGL3_RenderDrawData(data);
    // imgui restored raw GL it does not track; drop the state-cache shadow.
    gl::reset_state_cache();
  }
}

ImTextureID add_texture(uint32_t width, uint32_t height, const uint8_t* data) noexcept {
  if (SDL_Renderer* renderer = window::get_sdl_renderer()) {
    SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, width, height);
    SDL_UpdateTexture(texture, nullptr, data, width * 4);
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
    g_sdlTextures.push_back(texture);
    return reinterpret_cast<ImTextureID>(texture);
  }
  // Create + upload a GL texture on the render worker; imgui uses the GL texture name as the
  // ImTextureID directly (ImGui_ImplOpenGL3 binds it with glBindTexture and sampler 0, so set
  // linear filtering on the texture itself).
  gl::Texture texture;
  const gl::Extent3D size{width, height, 1};
  gfx::render_worker::enqueue_work([&] {
    texture = gl::create_texture(gl::TextureFormat::RGBA8Unorm, size, 1, /*renderable=*/false);
    gl::gl.BindTexture(gl::GL_TEXTURE_2D, texture.id);
    gl::gl.TexParameteri(gl::GL_TEXTURE_2D, gl::GL_TEXTURE_MIN_FILTER, gl::GL_LINEAR);
    gl::gl.TexParameteri(gl::GL_TEXTURE_2D, gl::GL_TEXTURE_MAG_FILTER, gl::GL_LINEAR);
    gl::gl.PixelStorei(gl::GL_UNPACK_ALIGNMENT, 4);
    gl::gl.TexSubImage2D(gl::GL_TEXTURE_2D, 0, 0, 0, static_cast<gl::GLsizei>(width),
                         static_cast<gl::GLsizei>(height), gl::GL_RGBA, gl::GL_UNSIGNED_BYTE, data);
    gl::invalidate_texture_bindings();
  });
  gfx::render_worker::synchronize();
  g_glTextures.push_back(texture);
  return static_cast<ImTextureID>(texture.id);
}
} // namespace aurora::imgui

// C bindings
extern "C" {
ImTextureID aurora_imgui_add_texture(uint32_t width, uint32_t height, const void* rgba8) {
  return aurora::imgui::add_texture(width, height, static_cast<const uint8_t*>(rgba8));
}
}

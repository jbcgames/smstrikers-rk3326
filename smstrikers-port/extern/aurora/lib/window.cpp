#include "window.hpp"

#ifdef AURORA_ENABLE_GX
#include "imgui.hpp"
#include "webgpu/gpu.hpp"
#endif
#include "input.hpp"
#include "internal.hpp"

#include <aurora/aurora.h>
#include <aurora/event.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_render.h>
#include <SDL3/SDL_surface.h>
#include <SDL3/SDL_video.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_pixels.h>
#include <tracy/Tracy.hpp>

#if defined(SDL_PLATFORM_ANDROID)
#include <jni.h>
extern "C" void Android_LockActivityMutex(void);
extern "C" void Android_UnlockActivityMutex(void);
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <vector>

#include "rmlui.hpp"
#include "time_internal.hpp"
#include "dolphin/vi/vi_internal.hpp"

namespace aurora::window {
namespace {
constexpr Module Log{"aurora::window"};

SDL_Window* g_window;
SDL_Renderer* g_renderer;
float g_frameBufferScale = 0.f;
bool g_frameBufferAspectFit = false;
AuroraWindowSize g_windowSize;
std::vector<AuroraEvent> g_events;
std::atomic_bool g_backgrounded = false;
#if defined(SDL_PLATFORM_ANDROID)
std::atomic_bool g_surfaceReady = false;
#else
std::atomic_bool g_surfaceReady = true;
#endif
bool g_lastPaused = false;
bool g_gotFocus = false;

bool operator==(const AuroraWindowSize& lhs, const AuroraWindowSize& rhs) {
  return lhs.width == rhs.width && lhs.height == rhs.height && lhs.fb_width == rhs.fb_width &&
         lhs.fb_height == rhs.fb_height && lhs.native_fb_height == rhs.native_fb_height &&
         lhs.native_fb_width == rhs.native_fb_width && lhs.scale == rhs.scale;
}

Vec2<int> scale_frame_buffer_to_aspect(int w, int h, float scale, float aspect) {
  if (w <= 0 || h <= 0 || scale <= 0.f || aspect <= 0.f) {
    return {std::max(w, 1), std::max(h, 1)};
  }
  const int baseW = std::max(1, static_cast<int>(std::lround(static_cast<float>(w) * scale)));
  const int baseH = std::max(1, static_cast<int>(std::lround(static_cast<float>(h) * scale)));
  if (aspect >= static_cast<float>(w) / static_cast<float>(h)) {
    return {std::max(1, static_cast<int>(std::lround(static_cast<float>(baseH) * aspect))), baseH};
  }
  return {baseW, std::max(1, static_cast<int>(std::lround(static_cast<float>(baseW) / aspect)))};
}

Vec2<int> fit_frame_buffer_to_aspect(int width, int height, float aspect) {
  if (width <= 0 || height <= 0 || aspect <= 0.f) {
    return {std::max(width, 1), std::max(height, 1)};
  }
  if (static_cast<float>(width) / static_cast<float>(height) > aspect) {
    return {std::max(1, static_cast<int>(std::lround(static_cast<float>(height) * aspect))), height};
  }
  return {width, std::max(1, static_cast<int>(std::lround(static_cast<float>(width) / aspect)))};
}

void resize_swapchain() noexcept {
  const auto size = get_window_size();
  if (size == g_windowSize) {
    return;
  }
  if (size.scale != g_windowSize.scale) {
    if (g_windowSize.scale > 0.f) {
      Log.info("Display scale changed to {}", size.scale);
    }
  }
  g_windowSize = size;
  if (g_renderer != nullptr) {
    SDL_SetRenderLogicalPresentation(g_renderer, static_cast<int>(size.native_fb_width),
                                     static_cast<int>(size.native_fb_height), SDL_LOGICAL_PRESENTATION_DISABLED);
    SDL_SetRenderScale(g_renderer, size.scale, size.scale);
  }
#ifdef AURORA_ENABLE_GX
  webgpu::resize_swapchain(size.fb_width, size.fb_height, size.native_fb_width, size.native_fb_height);
#endif
}

void set_window_icon() noexcept {
  if (g_config.iconRGBA8 == nullptr) {
    return;
  }
  auto* iconSurface =
      SDL_CreateSurfaceFrom(static_cast<int>(g_config.iconWidth), static_cast<int>(g_config.iconHeight),
                            SDL_GetPixelFormatForMasks(32, 0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000),
                            g_config.iconRGBA8, static_cast<int>(4 * g_config.iconWidth));
  AURORA_ASSERT(iconSurface != nullptr, "Failed to create icon surface: {}", SDL_GetError());
  TRY_WARN(SDL_SetWindowIcon(g_window, iconSurface), "Failed to set window icon: {}", SDL_GetError());
  SDL_DestroySurface(iconSurface);
}

bool targets_primary_window(const SDL_Event* event) noexcept {
  const SDL_Window* eventWindow = SDL_GetWindowFromEvent(event);
  return eventWindow == nullptr || eventWindow == g_window;
}

bool SDLCALL lifecycle_event_watch(void*, SDL_Event* event) {
  if (targets_primary_window(event)) {
    switch (event->type) {
#if defined(SDL_PLATFORM_ANDROID) || defined(SDL_PLATFORM_APPLE)
    case SDL_EVENT_WINDOW_MINIMIZED:
      time::internal::set_pause_reason(time::internal::PauseReason::Background, true);
      g_backgrounded.store(true, std::memory_order_relaxed);
      break;
    case SDL_EVENT_WINDOW_RESTORED:
      g_backgrounded.store(false, std::memory_order_relaxed);
      time::internal::set_pause_reason(time::internal::PauseReason::Background, false);
      break;
#endif
    default:
      break;
    }
  }
  return true;
}

void sync_paused() {
  const bool paused = is_paused();
  if (g_lastPaused == paused) {
    return;
  }
  g_lastPaused = paused;
  time::internal::set_pause_reason(time::internal::PauseReason::Window, paused);
  g_events.push_back(AuroraEvent{
      .type = paused ? AURORA_PAUSED : AURORA_UNPAUSED,
  });
}

void process_event(SDL_Event& event) {
  const bool primaryWindow = targets_primary_window(&event);
  if (primaryWindow) {
#ifdef AURORA_ENABLE_GX
    imgui::process_event(event);
#endif
#ifdef AURORA_ENABLE_RMLUI
    rmlui::handle_event(event);
#endif
  }

  switch (event.type) {
  case SDL_EVENT_WINDOW_MOVED: {
    if (!primaryWindow) {
      break;
    }
    g_events.push_back(AuroraEvent{
        .type = AURORA_WINDOW_MOVED,
        .windowPos = {.x = event.window.data1, .y = event.window.data2},
    });
    break;
  }
  case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED: {
    if (!primaryWindow) {
      break;
    }
    resize_swapchain();
    g_events.push_back(AuroraEvent{
        .type = AURORA_DISPLAY_SCALE_CHANGED,
        .windowSize = get_window_size(),
    });
    break;
  }
  case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: {
    if (!primaryWindow) {
      break;
    }
    resize_swapchain();
    g_events.push_back(AuroraEvent{
        .type = AURORA_WINDOW_RESIZED,
        .windowSize = get_window_size(),
    });
    break;
  }
  case SDL_EVENT_GAMEPAD_ADDED: {
    auto instance = input::add_controller(event.gdevice.which);
    g_events.push_back(AuroraEvent{
        .type = AURORA_CONTROLLER_ADDED,
        .controller = instance,
    });
    break;
  }
  case SDL_EVENT_GAMEPAD_REMOVED: {
    input::remove_controller(event.gdevice.which);
    g_events.push_back(AuroraEvent{
        .type = AURORA_CONTROLLER_REMOVED,
        .controller = event.gdevice.which,
    });
    break;
  }
  case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
    auto* ctrl = SDL_GetGamepadFromID(event.gbutton.which);
    if (ctrl != nullptr) {
      const bool backPressed = SDL_GetGamepadButton(ctrl, SDL_GAMEPAD_BUTTON_BACK);
      const bool startPressed = SDL_GetGamepadButton(ctrl, SDL_GAMEPAD_BUTTON_START);
      const bool guidePressed = SDL_GetGamepadButton(ctrl, SDL_GAMEPAD_BUTTON_GUIDE);
      if (guidePressed || (backPressed && startPressed)) {
        Log.info("Exit combo (Select + Start) triggered from gamepad");
        g_events.push_back(AuroraEvent{
            .type = AURORA_EXIT,
        });
      }
    }
    break;
  }
  case SDL_EVENT_KEY_DOWN: {
    if (event.key.scancode == SDL_SCANCODE_ESCAPE) {
      Log.info("Exit hotkey (Escape) triggered from keyboard");
      g_events.push_back(AuroraEvent{
          .type = AURORA_EXIT,
      });
    }
    break;
  }
  case SDL_EVENT_MOUSE_WHEEL:
    if (primaryWindow) {
      input::set_mouse_scroll(event.wheel.x, event.wheel.y);
    }
    break;
  case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
    if (primaryWindow) {
      g_events.push_back(AuroraEvent{
          .type = AURORA_EXIT,
      });
    }
    break;
  case SDL_EVENT_QUIT:
    g_events.push_back(AuroraEvent{
        .type = AURORA_EXIT,
    });
    break;
  default:
    if (event.type == g_sdlCustomEventsStart + CustomEvent::FutureResize) {
      // Future resize event
      resize_swapchain();
      g_events.push_back(AuroraEvent{
          .type = AURORA_WINDOW_RESIZED,
          .windowSize = get_window_size(),
      });
    } else if (event.type == g_sdlCustomEventsStart + CustomEvent::RefreshSurface) {
      // Refresh surface (vsync changed)
#ifdef AURORA_ENABLE_GX
      webgpu::refresh_surface(false);
#endif
    }
    break;
  }

  if (primaryWindow) {
    sync_paused();
  }
  g_events.push_back(AuroraEvent{
      .type = AURORA_SDL_EVENT,
      .sdl = event,
  });
}
} // namespace

const AuroraEvent* poll_events() {
  ZoneScoped;
  g_events.clear();

  SDL_Event event;
  // Clear out the previous scroll values to prevent ghost input
  input::set_mouse_scroll(0, 0);
  if (is_paused()) {
    ZoneScopedN("SDL_WaitEvent (paused)");
    if (SDL_WaitEvent(&event)) {
      process_event(event);
    } else {
      Log.warn("SDL_WaitEvent failed: {}", SDL_GetError());
    }
  }
  while (true) {
    bool hasEvent = false;
    {
      ZoneScopedN("SDL_PollEvent");
      hasEvent = SDL_PollEvent(&event);
    }
    if (hasEvent) {
      process_event(event);
    } else {
      break;
    }
  }
  g_events.push_back(AuroraEvent{
      .type = AURORA_NONE,
  });
  return g_events.data();
}

bool create_window(AuroraBackend backend) {
  // The SDL2 shim is our custom SDL3 video driver wrapping the device's firmware SDL2 (kmsdrm/gbm via
  // libMali). On that driver we let SDL create/own the GL context so it exposes the firmware's borrowed
  // EGL display/surface/context, which Dawn then binds to via the adapter proc loader. On desktop
  // (wayland/x11) Dawn owns EGL itself, so an SDL-owned context would make Dawn's worker-thread
  // eglMakeCurrent fail with EGL_BAD_ACCESS — keep upstream's no-GL-flag behaviour there.
  const char* videoDriver = SDL_GetCurrentVideoDriver();
  const bool sdl2ShimDriver = videoDriver != nullptr && SDL_strcmp(videoDriver, "sdl2") == 0;

  // The shim exists to lend Dawn the firmware's EGL/GLES display; no other backend can present
  // through it. On a fresh config (backend "auto") the backend loop would otherwise attempt
  // Vulkan first, and that attempt's window create/destroy cycle leaves the firmware SDL2/EGL
  // stack unable to keep a context current for the later GLES attempt's EFB present init
  // (fresh-install crash in the field). Refuse non-GLES backends before touching the display so
  // the loop falls straight through to OpenGLES. BACKEND_NULL stays allowed: it is the loop's
  // final fallback and must reach webgpu::initialize to produce the loud EFB-or-bust fatal.
  if (sdl2ShimDriver && backend != BACKEND_OPENGLES && backend != BACKEND_NULL) {
    constexpr std::array kBackendNames{"Auto",     "D3D11",  "D3D12",  "Metal", "Vulkan",
                                       "OpenGL", "OpenGLES", "WebGPU", "Null"};
    const auto idx = static_cast<size_t>(backend);
    Log.info("Skipping backend {} on the sdl2 shim driver (GLES-only display path)",
             idx < kBackendNames.size() ? kBackendNames[idx] : "?");
    return false;
  }

  SDL_WindowFlags flags = 0;
  // The borrowed-EGL path presents at the shim's own drawable size; high-DPI scaling here would desync
  // the WebGPU swapchain extent from the borrowed surface. Skip it on the shim driver only.
  if (!sdl2ShimDriver) {
    flags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;
  }
#if TARGET_OS_IOS || TARGET_OS_TV
  flags |= SDL_WINDOW_FULLSCREEN;
#else
  flags |= SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE;
  if (g_config.startFullscreen) {
    flags |= SDL_WINDOW_FULLSCREEN;
  }
#endif
#ifdef AURORA_ENABLE_GX
  // The GL backend renders through a real SDL GL context (desktop: SDL_GL_CreateContext;
  // device: the shim's borrowed EGL context), so the window must be an OpenGL window.
  if (backend == BACKEND_OPENGL || backend == BACKEND_OPENGLES) {
    flags |= SDL_WINDOW_OPENGL;
  }
#endif
  auto width = static_cast<Sint32>(g_config.windowWidth);
  auto height = static_cast<Sint32>(g_config.windowHeight);
  if (width == 0 || height == 0) {
    width = 1280;
    height = 960;
  }
  if (width < 640) {
    width = 640;
  }
  if (height < 480) {
    height = 480;
  }

  // Device (sdl2 shim) path: the firmware SDL2 creates its fbdev surface at exactly the size we
  // request and there is no compositor to place or scale it, so a window that is not the panel size
  // shows up clipped and offset -- and SDL_WINDOW_FULLSCREEN is a no-op through the shim, so it
  // cannot fix that. Size the window to the panel's native mode (the shim republishes the firmware
  // SDL2 desktop mode as this SDL3 display), which makes every handheld fill its own panel with no
  // per-device window-size config. Desktop keeps the configured/default size. The min-size clamp
  // above stays as the floor for the pathological "mode query failed" case.
  if (sdl2ShimDriver) {
    const SDL_DisplayID display = SDL_GetPrimaryDisplay();
    const SDL_DisplayMode* mode = display != 0 ? SDL_GetDesktopDisplayMode(display) : nullptr;
    if (mode != nullptr && mode->w > 0 && mode->h > 0) {
      width = mode->w;
      height = mode->h;
      Log.info("Device panel mode is {}x{}; sizing the window to fill it", width, height);
    } else {
      Log.warn("Could not query the device panel mode ({}); leaving window at {}x{}",
               SDL_GetError(), width, height);
    }
  }

  Sint32 posX = g_config.windowPosX;
  Sint32 posY = g_config.windowPosY;
  if (posX < 0 || posY < 0) {
    posX = SDL_WINDOWPOS_UNDEFINED;
    posY = SDL_WINDOWPOS_UNDEFINED;
  }

  const auto props = SDL_CreateProperties();
  TRY(SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, g_config.appName), "Failed to set {}: {}",
      SDL_PROP_WINDOW_CREATE_TITLE_STRING, SDL_GetError());
  TRY(SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, posX), "Failed to set {}: {}",
      SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_GetError());
  TRY(SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, posY), "Failed to set {}: {}",
      SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_GetError());
  TRY(SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, width), "Failed to set {}: {}",
      SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, SDL_GetError());
  TRY(SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, height), "Failed to set {}: {}",
      SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, SDL_GetError());
  TRY(SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER, flags), "Failed to set {}: {}",
      SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER, SDL_GetError());
  if (flags & SDL_WINDOW_OPENGL) {
    TRY(SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_OPENGL_BOOLEAN, true),
        "Failed to set {}: {}", SDL_PROP_WINDOW_CREATE_OPENGL_BOOLEAN, SDL_GetError());
  }
  // Only the shim (device) path supplies the GL context externally (the firmware
  // EGL context aurora borrows). On desktop, SDL owns the GL context via
  // SDL_GL_CreateContext, so the window must NOT be flagged external -- otherwise
  // SDL skips GL setup and SDL_GL_CreateContext rejects the window.
  const bool externalGraphicsContext = sdl2ShimDriver && backend != BACKEND_NULL;
  TRY(SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_EXTERNAL_GRAPHICS_CONTEXT_BOOLEAN, externalGraphicsContext),
      "Failed to set {}: {}", SDL_PROP_WINDOW_CREATE_EXTERNAL_GRAPHICS_CONTEXT_BOOLEAN, SDL_GetError());
  g_window = SDL_CreateWindowWithProperties(props);
  if (g_window == nullptr) {
    Log.error("Failed to create window: {}", SDL_GetError());
    return false;
  }
  SDL_SetWindowMinimumSize(g_window, 640, 480);
  set_window_icon();
  return true;
}

bool create_renderer() {
  if (g_window == nullptr) {
    return false;
  }
  const auto props = SDL_CreateProperties();
  TRY(SDL_SetPointerProperty(props, SDL_PROP_RENDERER_CREATE_WINDOW_POINTER, g_window), "Failed to set {}: {}",
      SDL_PROP_RENDERER_CREATE_WINDOW_POINTER, SDL_GetError());
  TRY(SDL_SetNumberProperty(props, SDL_PROP_RENDERER_CREATE_PRESENT_VSYNC_NUMBER, SDL_RENDERER_VSYNC_ADAPTIVE),
      "Failed to set {}: {}", SDL_PROP_RENDERER_CREATE_PRESENT_VSYNC_NUMBER, SDL_GetError());
  g_renderer = SDL_CreateRendererWithProperties(props);
  if (g_renderer == nullptr) {
    Log.error("Failed to create renderer: {}", SDL_GetError());
    return false;
  }
  return true;
}

void destroy_window() {
  if (g_renderer != nullptr) {
    SDL_DestroyRenderer(g_renderer);
    g_renderer = nullptr;
  }
  if (g_window != nullptr) {
    SDL_DestroyWindow(g_window);
    g_window = nullptr;
  }
}

void show_window() {
  if (g_window != nullptr) {
    TRY_WARN(SDL_ShowWindow(g_window), "Failed to show window: {}", SDL_GetError());
  }
}

bool initialize() {
  /* We don't want to initialize anything input related here, otherwise the add events will get lost to the void */
  TRY(SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight"), "Error setting {}: {}", SDL_HINT_ORIENTATIONS,
      SDL_GetError());
  TRY(SDL_InitSubSystem(SDL_INIT_EVENTS | SDL_INIT_VIDEO), "Error initializing SDL: {}", SDL_GetError());
  time::internal::set_pause_reason(time::internal::PauseReason::Surface,
                                   !g_surfaceReady.load(std::memory_order_acquire));

#if !defined(_WIN32) && !defined(__APPLE__)
  TRY(SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0"), "Error setting {}: {}",
      SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, SDL_GetError());
#endif
  TRY(SDL_SetHint(SDL_HINT_SCREENSAVER_INHIBIT_ACTIVITY_NAME, g_config.appName), "Error setting {}: {}",
      SDL_HINT_SCREENSAVER_INHIBIT_ACTIVITY_NAME, SDL_GetError());
  TRY(SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_GAMECUBE_RUMBLE_BRAKE, "1"), "Error setting {}: {}",
      SDL_HINT_JOYSTICK_HIDAPI_GAMECUBE_RUMBLE_BRAKE, SDL_GetError());

  TRY(SDL_DisableScreenSaver(), "Error disabling screensaver: {}", SDL_GetError());
  if (g_config.allowJoystickBackgroundEvents) {
    TRY(SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1"), "Error setting {}: {}",
        SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, SDL_GetError());
  }

  return true;
}

bool initialize_event_watch() {
  TRY(SDL_AddEventWatch(lifecycle_event_watch, nullptr), "Error adding SDL event watch: {}", SDL_GetError());
  return true;
}

void shutdown() {
  SDL_RemoveEventWatch(lifecycle_event_watch, nullptr);
  destroy_window();
  TRY_WARN(SDL_EnableScreenSaver(), "Error enabling screensaver: {}", SDL_GetError());
  SDL_Quit();
}

AuroraWindowSize get_window_size() {
  int width = 0;
  int height = 0;
  int native_fb_w = 0;
  int native_fb_h = 0;
  AURORA_ASSERT(SDL_GetWindowSize(g_window, &width, &height), "Failed to get window size: {}", SDL_GetError());
  AURORA_ASSERT(SDL_GetWindowSizeInPixels(g_window, &native_fb_w, &native_fb_h), "Failed to get window size in pixels: {}",
         SDL_GetError());

  int fb_w = native_fb_w;
  int fb_h = native_fb_h;
  if (g_frameBufferScale > 0.f) {
    // Scale the physical drawable, not the GameCube render mode, so the internal resolution is a
    // fraction of the actual screen. An integer 1/N (e.g. 0.5) then maps each rendered pixel onto
    // an exact NxN block of panel pixels -- pixel-perfect, no upscale shimmer -- on any display.
    // (Passing the native size and its own aspect makes scale_frame_buffer_to_aspect a plain
    // scale-by-g_frameBufferScale; the aspect-fit-to-game block below crops it to 4:3.)
    const auto [scaledW, scaledH] =
        scale_frame_buffer_to_aspect(fb_w, fb_h, g_frameBufferScale,
                                     static_cast<float>(fb_w) / static_cast<float>(fb_h));
    fb_w = scaledW;
    fb_h = scaledH;
  }
  if (g_frameBufferAspectFit) {
    const auto [baseW, baseH] = vi::configured_fb_size();
    if (baseW > 0 && baseH > 0) {
      const auto [fitW, fitH] =
          fit_frame_buffer_to_aspect(fb_w, fb_h, static_cast<float>(baseW) / static_cast<float>(baseH));
      fb_w = fitW;
      fb_h = fitH;
    }
  }

  const float scale = SDL_GetWindowDisplayScale(g_window);
  return {
      .width = static_cast<uint32_t>(width),
      .height = static_cast<uint32_t>(height),
      .fb_width = static_cast<uint32_t>(fb_w),
      .fb_height = static_cast<uint32_t>(fb_h),
      .native_fb_width = static_cast<uint32_t>(native_fb_w),
      .native_fb_height = static_cast<uint32_t>(native_fb_h),
      .scale = scale,
  };
}

SDL_Window* get_sdl_window() { return g_window; }

SDL_Renderer* get_sdl_renderer() { return g_renderer; }

bool is_paused() noexcept {
  if (!is_presentable()) {
    return true;
  }
  const auto flags = SDL_GetWindowFlags(g_window);
  if ((flags & SDL_WINDOW_HIDDEN) != 0u) {
    return true;
  }
  // Wait until the window has received focus before respecting pauseOnFocusLost
  if (!g_gotFocus) {
    g_gotFocus = (flags & SDL_WINDOW_INPUT_FOCUS) != 0u;
    return false;
  }
  return g_config.pauseOnFocusLost && ((flags & SDL_WINDOW_INPUT_FOCUS) == 0u || (flags & SDL_WINDOW_MINIMIZED) != 0u);
}

bool is_presentable() noexcept {
  return g_window != nullptr && !g_backgrounded.load(std::memory_order_acquire) &&
         g_surfaceReady.load(std::memory_order_acquire);
}

void set_surface_ready(bool ready) noexcept {
  g_surfaceReady.store(ready, std::memory_order_release);
  time::internal::set_pause_reason(time::internal::PauseReason::Surface, !ready);
}

SurfaceLock::SurfaceLock() noexcept {
#if defined(SDL_PLATFORM_ANDROID)
  Android_LockActivityMutex();
#endif
}

SurfaceLock::~SurfaceLock() {
#if defined(SDL_PLATFORM_ANDROID)
  Android_UnlockActivityMutex();
#endif
}

bool push_custom_event(CustomEvent eventType) {
  SDL_Event event{.type = g_sdlCustomEventsStart + eventType};
  return SDL_PushEvent(&event);
}

void set_title(const char* title) {
  TRY_WARN(SDL_SetWindowTitle(g_window, title), "Failed to set window title: {}", SDL_GetError());
}

void set_fullscreen(bool fullscreen) {
  TRY_WARN(SDL_SetWindowFullscreen(g_window, fullscreen), "Failed to set window fullscreen: {}", SDL_GetError());
}

bool get_fullscreen() { return (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN) != 0u; }

void set_window_size(uint32_t width, uint32_t height) {
  TRY_WARN(SDL_RestoreWindow(g_window), "Failed to un-maximize window: {}", SDL_GetError());
  TRY_WARN(SDL_SetWindowSize(g_window, width, height), "Failed to set window size: {}", SDL_GetError());
}

void set_window_position(uint32_t x, uint32_t y) {
  TRY_WARN(SDL_SetWindowPosition(g_window, x, y), "Failed to set window position: {}", SDL_GetError());
}

void center_window() {
  TRY_WARN(SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED),
           "Failed to center window: {}", SDL_GetError());
}

static void push_future_resize_event() {
  TRY_WARN(push_custom_event(CustomEvent::FutureResize), "Failed to push SDL event for future resize: {}",
           SDL_GetError());
}

void request_frame_buffer_resize() {
  if (g_window != nullptr) {
    // Defer so that we don't try to reconfigure the surface in the middle of game logic.
    push_future_resize_event();
  }
}

void resize_frame_buffer_now() {
  if (g_window != nullptr) {
    resize_swapchain();
  }
}

float get_frame_buffer_scale() { return g_frameBufferScale; }

void set_frame_buffer_scale(float scale) {
  if (scale < 0.f) {
    scale = 0.f;
  }
  if (g_frameBufferScale == scale) {
    return;
  }
  g_frameBufferScale = scale;
  request_frame_buffer_resize();
}

void set_frame_buffer_aspect_fit(bool fit) {
  if (g_frameBufferAspectFit == fit) {
    return;
  }

  g_frameBufferAspectFit = fit;
  request_frame_buffer_resize();
}

void set_background_input(bool value) { SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, value ? "1" : "0"); }

} // namespace aurora::window

#if defined(SDL_PLATFORM_ANDROID)
extern "C" JNIEXPORT void JNICALL Java_dev_encounter_aurora_AuroraSurface_nativeSetSurfaceReady(JNIEnv*, jclass,
                                                                                                jboolean ready) {
  aurora::window::set_surface_ready(ready == JNI_TRUE);
}
#endif

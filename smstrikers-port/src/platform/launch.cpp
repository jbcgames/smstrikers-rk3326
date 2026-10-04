// See include/port/launch.h for why these three settings live here rather than in main.cpp.

#include "port/host.h"
#include "port/launch.h"

#if defined(PORT_USE_AURORA)

#include "port/fatal.h"
#include "port/steamdeck.h"
#include "port/texfilter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_video.h>

namespace
{

const char* LevelName(AuroraLogLevel level)
{
    switch (level)
    {
    case LOG_DEBUG: return "debug";
    case LOG_INFO: return "info";
    case LOG_WARNING: return "warning";
    case LOG_ERROR: return "error";
    case LOG_FATAL: return "fatal";
    default: return "unknown";
    }
}

void AuroraLog(AuroraLogLevel level, const char* module, const char* message, unsigned int len)
{
    fprintf(stderr, "[%s] [%s] %.*s\n", LevelName(level), module != NULL ? module : "", (int)len, message);
    if (level != LOG_FATAL)
        return;
    fflush(stderr);
    // A box off the main thread would wait on a main thread that may never pump it.
    if (!SDL_IsMainThread())
        return;
    char text[2048];
    snprintf(text, sizeof text,
             "The game stopped with an error it cannot recover from:\n\n  [%s] %.*s\n\n"
             "If it names the graphics adapter, device or window, updating the graphics driver "
             "is the first thing to try.",
             module != NULL ? module : "", (int)len, message);
    port_fatal_notice("Super Mario Strikers: fatal error", text);
}

// A boolean the way a person writes one. `fullscreen = yes` and `fullscreen = 1` are the same
// statement, and refusing the first is the kind of pedantry that makes a config file worse than a
// wrapper script.
int EnvBool(const char* name, int fallback)
{
    const char* v = getenv(name);
    if (v == NULL || *v == '\0')
        return fallback;
    if (strcmp(v, "0") == 0 || strcmp(v, "false") == 0 || strcmp(v, "no") == 0 ||
        strcmp(v, "off") == 0 || strcmp(v, "FALSE") == 0)
        return 0;
    return 1;
}

bool ParseWindowSize(const char* v, unsigned* w, unsigned* h)
{
    unsigned a = 0, b = 0;
    char sep = 0;
    if (sscanf(v, " %u %c %u", &a, &sep, &b) != 3 || (sep != 'x' && sep != 'X' && sep != '*'))
        return false;
    if (a == 0 || b == 0 || a > 16384 || b > 16384)
        return false;
    *w = a;
    *h = b;
    return true;
}

constexpr int kFrameW = 16;
constexpr int kFrameH = 64;

// Video is started and stopped again so aurora_initialize finds SDL as it expects.
bool UsableScreen(SDL_Rect* out)
{
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
        return false;
    *out = SDL_Rect{ 0, 0, 0, 0 };
    const SDL_DisplayID display = SDL_GetPrimaryDisplay();
    const bool ok = display != 0 && SDL_GetDisplayUsableBounds(display, out) && out->w > 0 && out->h > 0;
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    return ok;
}

void FitWindowToScreen(const SDL_Rect& r, unsigned* w, unsigned* h)
{
    static const unsigned kSizes[][2] = {
        { 1920, 1080 }, { 1600, 900 }, { 1280, 720 }, { 1024, 576 }, { 960, 540 },
    };

    const size_t n = sizeof kSizes / sizeof kSizes[0];
    size_t pick = n - 1;
    for (size_t i = 0; i < n; i++)
    {
        if ((int)kSizes[i][0] + kFrameW <= r.w && (int)kSizes[i][1] + kFrameH <= r.h)
        {
            pick = i;
            break;
        }
    }
    *w = kSizes[pick][0];
    *h = kSizes[pick][1];
}

// SDL on Windows places the client area at (x, y), so the title bar needs kFrameH clear above it.
void PlaceWindow(const SDL_Rect& r, unsigned w, unsigned h, int* x, int* y)
{
    *x = r.x + (r.w - (int)w) / 2;
    if (*x < r.x)
        *x = r.x;
    *y = r.y + (r.h - (int)h + kFrameH) / 2;
    if (*y < r.y + kFrameH)
        *y = r.y + kFrameH;
}

bool g_renderScalePinned;
bool g_renderScaleChecked;
float g_renderScale;

// The window icon: the game's own memory card icon assets/icon/MC_Icon.tpl is the 32x32 image the
// game writes into its save file so the GameCube's memory card screen has something to show: Mario
// with the ball.

#include "mc_icon.h"

constexpr int kIconScale = 2;
constexpr int kIconSize = MC_ICON_WIDTH * kIconScale;

unsigned char g_icon[kIconSize * kIconSize * 4];

void BuildIcon()
{
    for (int sy = 0; sy < MC_ICON_HEIGHT; sy++)
        for (int sx = 0; sx < MC_ICON_WIDTH; sx++)
        {
            const unsigned char* px = &kMcIconRGBA8[(sy * MC_ICON_WIDTH + sx) * 4];
            for (int dy = 0; dy < kIconScale; dy++)
                for (int dx = 0; dx < kIconScale; dx++)
                    memcpy(&g_icon[((sy * kIconScale + dy) * kIconSize + sx * kIconScale + dx) * 4], px, 4);
        }
}

// Aurora copies the AuroraConfig struct but not what its pointers point at (aurora.cpp keeps
// `g_config = *config`), and the icon is read later, when the window is created.
char g_cacheDir[1024];
char g_userDir[1024];

} // namespace

extern "C" void PortAuroraConfigure(AuroraConfig* cfg)
{
    if (cfg == NULL)
        return;

    // What the operating system calls this program.
    SDL_SetAppMetadata(cfg->appName != NULL ? cfg->appName : "Super Mario Strikers",
                       NULL, "org.smstrikers.port");
    SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_TYPE_STRING, "game");

    cfg->logCallback = AuroraLog;

#if defined(__linux__)
    // Dawn's EGL swap chain cannot present to a Wayland surface, so the GL backends go through XWayland.
    if (cfg->desiredBackend == BACKEND_OPENGL || cfg->desiredBackend == BACKEND_OPENGLES)
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "x11,wayland");
#endif

    // Fullscreen at startup. F11 and the debug menu's System tab already toggle it at runtime
    // through SDL, but a player who wants fullscreen wants it before the game has drawn anything,
    // and AuroraConfig is the only place that can be asked for.
    {
        // gamescope (Steam's Game Mode) sizes a window to the screen only when it asks for fullscreen, and scales any other window in with bars.
        const bool gamescope = PortUnderGamescope() != 0;
        const char* v = getenv("STRIKERS_FULLSCREEN");
        cfg->startFullscreen = EnvBool("STRIKERS_FULLSCREEN", gamescope ? 1 : 0) != 0;
        if (gamescope && (v == NULL || *v == '\0'))
            fprintf(stderr, "[port] gamescope: starting fullscreen; fullscreen = 0 opens a window\n");
    }

    {
        const char* v = getenv("STRIKERS_WINDOW_SIZE");
        unsigned w = 0, h = 0;
        SDL_Rect screen;
        const bool measured = UsableScreen(&screen);
        const bool asked = v != NULL && *v != '\0' && strcmp(v, "auto") != 0;
        if (asked && !ParseWindowSize(v, &w, &h))
            fprintf(stderr,
                    "[port] window size: ignoring window_size = %s; expected WIDTHxHEIGHT, "
                    "such as 1600x900\n", v);
        if (asked && w != 0)
        {
            cfg->windowWidth = w;
            cfg->windowHeight = h;
            fprintf(stderr, "[port] window size: %ux%u (window_size)\n", w, h);
        }
        else if (measured)
        {
            FitWindowToScreen(screen, &w, &h);
            cfg->windowWidth = w;
            cfg->windowHeight = h;
            cfg->startMaximized = true;
            fprintf(stderr, "[port] window size: maximised, restoring to %ux%u, the largest that fits "
                    "%dx%d of usable screen\n", w, h, screen.w, screen.h);
        }
        else
            fprintf(stderr, "[port] window size: %ux%u (no display to measure)\n",
                    (unsigned)cfg->windowWidth, (unsigned)cfg->windowHeight);

        cfg->windowPosX = -1;
        cfg->windowPosY = -1;
        if (measured)
        {
            int x = 0, y = 0;
            PlaceWindow(screen, cfg->windowWidth, cfg->windowHeight, &x, &y);
            if (x >= 0 && y >= 0)
            {
                cfg->windowPosX = x;
                cfg->windowPosY = y;
            }
            fprintf(stderr, "[port] window position: %d,%d in usable screen at %d,%d\n",
                    (int)cfg->windowPosX, (int)cfg->windowPosY, screen.x, screen.y);
        }
    }

    // Let Aurora pick a software rasteriser when that is all there is.
    cfg->allowCpuAdapter = EnvBool("STRIKERS_ALLOW_CPU_ADAPTER", 0) != 0;

    // Default 0, and that is a decision rather than an oversight.
    cfg->pauseOnFocusLost = EnvBool("STRIKERS_PAUSE_ON_FOCUS_LOST", 0) != 0;

    // How many shaders compile at once; 0 or unset leaves it to the core count.
    {
        constexpr long kMaxShaderJobs = 16;
        const char* v = getenv("STRIKERS_SHADER_JOBS");
        cfg->pipelineJobs = 0;
        if (v != NULL && *v != '\0')
        {
            char* end = NULL;
            const long n = strtol(v, &end, 10);
            while (end != NULL && (*end == ' ' || *end == '\t'))
                end++;
            if (end == v || end == NULL || *end != '\0' || n < 0 || n > kMaxShaderJobs)
                fprintf(stderr, "[port] shader_jobs = %s is not a number from 0 to %ld; using the core count\n",
                        v, kMaxShaderJobs);
            else
                cfg->pipelineJobs = (uint32_t)n;
        }
    }

    // Anisotropic filtering. This is the *ceiling*, and it is the half of the control the GX enum
    // cannot express: glxSend picks GX_ANISO_4, which Aurora's wgpu_aniso() resolves to exactly
    // this number (GX_ANISO_2 would resolve to half of it).
    cfg->maxTextureAnisotropy = (uint16_t)PortTextureAniso();

    // STRIKERS_USER_DIR: the memory card somewhere other than the player's. Aurora derives the card
    // directory from userPath alone and formats an empty card where it finds none.
    {
        const char* dir = getenv("STRIKERS_USER_DIR");
        if (dir != NULL && *dir != '\0')
        {
            // The trailing separator is what SDL_GetPrefPath returns; imgui.cpp concatenates
            // "/imgui.ini" onto it by hand.
            const size_t n = strlen(dir);
            const int hasSep = n != 0 && (dir[n - 1] == '/' || dir[n - 1] == '\\');
            snprintf(g_userDir, sizeof g_userDir, "%s%s", dir, hasSep ? "" : "/");
            // SDL_GetPrefPath creates what it returns and a path handed in here does not exist yet;
            // without this the card is formatted into a directory that is not there.
            SDL_CreateDirectory(g_userDir);
            cfg->userPath = g_userDir;
            fprintf(stderr, "[port] user dir: %s\n", g_userDir);
        }
    }

    // Aurora otherwise puts its pipeline and shader caches under SDL_GetPrefPath (on macOS,
    // ~/Library/Application Support/Super Mario Strikers).
    {
        const char* dir = getenv("STRIKERS_CACHE_DIR");
        if (dir != NULL && *dir != '\0')
        {
            snprintf(g_cacheDir, sizeof g_cacheDir, "%s", dir);
            // SDL_GetPrefPath creates what it returns; a path handed in here does not exist yet,
            // and sqlite3_open into a missing directory leaves Aurora running with no cache at all.
            SDL_CreateDirectory(g_cacheDir);
            cfg->cachePath = g_cacheDir;
        }
    }

    // Shader pipeline seeds and initial cache path (Dusklight methodology).
    {
        static char s_resourcesDir[512] = {0};
        const char* dir = getenv("STRIKERS_RESOURCES_DIR");
        if (dir == NULL || *dir == '\0')
            dir = getenv("GAMEDIR");
        if (dir == NULL || *dir == '\0')
            dir = getenv("STRIKERS_DATA");
        if (dir != NULL && *dir != '\0')
        {
            snprintf(s_resourcesDir, sizeof(s_resourcesDir), "%s", dir);
            cfg->resourcesPath = s_resourcesDir;
            fprintf(stderr, "[port] resources dir for pipeline cache: %s\n", s_resourcesDir);
        }
    }

    // If local pipeline_cache.db doesn't exist yet, seed it from initial_pipeline_cache.db
    if (cfg->cachePath != NULL && *cfg->cachePath != '\0')
    {
        char targetFile[512];
        snprintf(targetFile, sizeof(targetFile), "%s/pipeline_cache.db", cfg->cachePath);
        FILE* testTarget = fopen(targetFile, "rb");
        if (testTarget != NULL)
        {
            fclose(testTarget);
        }
        else
        {
            char sourceFile[512] = {0};
            if (cfg->resourcesPath != NULL && *cfg->resourcesPath != '\0')
                snprintf(sourceFile, sizeof(sourceFile), "%s/initial_pipeline_cache.db", cfg->resourcesPath);
            else
                snprintf(sourceFile, sizeof(sourceFile), "initial_pipeline_cache.db");

            FILE* src = fopen(sourceFile, "rb");
            if (src == NULL && cfg->resourcesPath != NULL)
            {
                snprintf(sourceFile, sizeof(sourceFile), "initial_pipeline_cache.db");
                src = fopen(sourceFile, "rb");
            }
            if (src != NULL)
            {
                FILE* dst = fopen(targetFile, "wb");
                if (dst != NULL)
                {
                    char buf[64 * 1024];
                    size_t n;
                    while ((n = fread(buf, 1, sizeof(buf), src)) > 0)
                    {
                        if (fwrite(buf, 1, n, dst) != n)
                            break;
                    }
                    fclose(dst);
                    fprintf(stderr, "[port] Seeded pipeline cache from '%s' to '%s'\n", sourceFile, targetFile);
                }
                fclose(src);
            }
        }
    }

    BuildIcon();
    cfg->iconRGBA8 = g_icon;
    cfg->iconWidth = kIconSize;
    cfg->iconHeight = kIconSize;

    // STRIKERS_DUMP_ICON=<path.ppm>: what the icon actually looks like.
    {
        const char* dump = getenv("STRIKERS_DUMP_ICON");
        if (dump != NULL && *dump != '\0')
        {
            FILE* f = fopen(dump, "wb");
            if (f != NULL)
            {
                fprintf(f, "P6\n%d %d\n255\n", kIconSize, kIconSize);
                for (int i = 0; i < kIconSize * kIconSize; i++)
                {
                    const unsigned char* p = &g_icon[i * 4];
                    for (int c = 0; c < 3; c++)
                    {
                        const int v = (p[c] * p[3] + 48 * (255 - p[3])) / 255;
                        fputc(v, f);
                    }
                }
                fclose(f);
                fprintf(stderr, "[port] icon -> %s\n", dump);
            }
        }
    }

    fprintf(stderr,
            "[port] window: fullscreen=%d pause_on_focus_lost=%d aniso=%ux "
            "icon=MC_Icon %dx%d",
            cfg->startFullscreen ? 1 : 0, cfg->pauseOnFocusLost ? 1 : 0,
            (unsigned)cfg->maxTextureAnisotropy, kIconSize, kIconSize);
    if (cfg->cachePath != NULL)
        fprintf(stderr, " cache=%s", cfg->cachePath);
    fprintf(stderr, "\n");
}

namespace
{

// Aurora's scale covers the 854x448 logical frame on both axes, which is about 480 x scale rows,
// so the port finds the scale that makes 448 x res_scale rows against Aurora's own arithmetic.
// Bisected: at 16:9 the band that makes an exact row count is narrower than a ratio step.
float ScaleForRows(unsigned rows, float start)
{
    float best = start;
    unsigned bestMiss = ~0u;
    float lo = 0.0f, hi = 0.0f;
    auto tryScale = [&](float scale) -> unsigned {
        aurora_set_frame_buffer_scale(scale);
        const unsigned got = aurora_window_size().fb_height;
        const unsigned miss = got > rows ? got - rows : rows - got;
        if (got != 0 && miss < bestMiss)
        {
            bestMiss = miss;
            best = scale;
        }
        if (got < rows)
            lo = scale;
        else if (got > rows)
            hi = scale;
        return got;
    };

    float scale = start;
    for (int i = 0; i < 4 && bestMiss != 0 && (lo == 0.0f || hi == 0.0f); i++)
    {
        const unsigned got = tryScale(scale);
        if (got == 0)
            return start;
        scale *= (float)rows / (float)got;
        if (got < rows && hi == 0.0f)
            scale += 1.0f / 1024.0f;
        else if (got > rows && lo == 0.0f)
            scale -= 1.0f / 1024.0f;
    }
    for (int i = 0; i < 24 && bestMiss != 0 && lo > 0.0f && hi > 0.0f; i++)
        tryScale(0.5f * (lo + hi));
    aurora_set_frame_buffer_scale(best);
    return best;
}

float g_auroraScale;
unsigned g_rowsWanted;
unsigned g_rowsGot;

void ApplyRenderRows()
{
    if (g_renderScale <= 0.0f)
        return;
    const unsigned want = (unsigned)(g_renderScale * 448.0f + 0.5f);
    // A window, aspect or VI mode change all show up as the framebuffer height moving.
    if (want == g_rowsWanted && aurora_window_size().fb_height == g_rowsGot)
        return;

    g_auroraScale = ScaleForRows(want, g_auroraScale > 0.0f ? g_auroraScale : g_renderScale);
    const AuroraWindowSize ws = aurora_window_size();
    g_rowsWanted = want;
    g_rowsGot = ws.fb_height;
    fprintf(stderr, "[port] render target %ux%u for %.3fx (%u rows)%s, Aurora scale %.4f\n",
            ws.fb_width, ws.fb_height, (double)g_renderScale, want,
            g_renderScalePinned ? ", chosen" : " following the window", (double)g_auroraScale);
}

#if defined(__SWITCH__)
// Scale from STRIKERS_RES_SCALE or the window, used when no per-mode override applies.
bool g_baseCaptured = false;
bool g_basePinned = false;
float g_baseScale = 1.0f;
#endif

} // namespace

void PortFollowRenderScale(unsigned int windowHeight)
{
    if (!g_renderScaleChecked)
    {
        g_renderScaleChecked = true;
        const char* e = getenv("STRIKERS_RES_SCALE");
        if (e != NULL && *e != '\0')
        {
            g_renderScalePinned = true;
            g_renderScale = (float)atof(e);
        }
        else if (PortIsSteamDeck())
        {
            // The panel's own rows even when docked, where following the output would outrun the Deck's GPU.
            g_renderScalePinned = true;
            g_renderScale = (float)PORT_STEAM_DECK_ROWS / 448.0f;
            fprintf(stderr, "[port] Steam Deck: rendering the panel's %u rows; res_scale overrides\n",
                    (unsigned)PORT_STEAM_DECK_ROWS);
        }
    }
#if defined(__SWITCH__)
    // Per-mode scales override STRIKERS_RES_SCALE, checked each frame to follow docking.
    {
        static float s_handheld = -1.0f;
        static float s_docked = -1.0f;
        if (s_handheld < 0.0f)
        {
            const char* h = getenv("STRIKERS_RES_SCALE_HANDHELD");
            const char* d = getenv("STRIKERS_RES_SCALE_DOCKED");
            s_handheld = (h != NULL && *h != '\0') ? (float)atof(h) : 0.0f;
            s_docked = (d != NULL && *d != '\0') ? (float)atof(d) : 0.0f;
        }
        if (!g_baseCaptured)
        {
            g_baseCaptured = true;
            g_basePinned = g_renderScalePinned;
            g_baseScale = g_renderScale;
        }
        const float perMode = port_docked() == 1 ? s_docked : s_handheld;
        g_renderScalePinned = perMode > 0.0f || g_basePinned;
        g_renderScale = perMode > 0.0f ? perMode : g_baseScale;
    }
#endif
    if (!g_renderScalePinned)
    {
        if (windowHeight == 0)
            return;
        float scale = (float)windowHeight / 448.0f;
        if (scale > 3.0f)
            scale = 3.0f;
        if (scale < 1.0f)
            scale = 1.0f;
        g_renderScale = scale;
    }
    ApplyRenderRows();
}

void PortSetRenderScale(float scale)
{
    g_renderScaleChecked = true;
    g_renderScalePinned = true;
    g_renderScale = scale;
#if defined(__SWITCH__)
    g_baseCaptured = true;
    g_basePinned = true;
    g_baseScale = scale;
#endif
    if (scale <= 0.0f)
        aurora_set_frame_buffer_scale(scale);
    ApplyRenderRows();
}

float PortRenderScale(void)
{
    return g_renderScale;
}

#endif // PORT_USE_AURORA

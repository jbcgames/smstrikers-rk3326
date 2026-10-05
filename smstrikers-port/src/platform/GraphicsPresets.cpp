#include "port/GraphicsPresets.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

static int s_currentPreset = GPRESET_MEDIUM;

static int s_optCutsceneSkin = 0;       // 0 = rigid, 1 = blend
static int s_optCutsceneCrowd = 0;      // 0 = hide, 1 = show
static int s_optCutsceneShadows = 0;    // 0 = hide, 1 = show
static int s_optCutsceneEffects = 0;    // 0 = hide, 1 = show
static int s_optCutsceneExtra = 1;      // 1 = cull extra props, 0 = keep
static float s_optCutsceneMaxDist = 85.0f;
static float s_optCutsceneLod = 0.035f;
static int s_optFastTransitions = 1;    // 1 = fade, 2 = cut, 0 = original 3D
static int s_optFenceSparks = 0;
static float s_optFenceRate = 0.06f;
static int s_optEnableDof = 0;
static int s_optMaxParticles = 512;

static const char* s_presetNames[4] = {
    "Ninguna",
    "Bajo",
    "Medio",
    "Alto"
};

static const char* GetEnvFilePath()
{
    static char s_path[512] = { 0 };
    if (s_path[0] != '\0') return s_path;

    const char* gamedir = getenv("GAMEDIR");
    if (gamedir && gamedir[0] != '\0')
    {
        snprintf(s_path, sizeof(s_path), "%s/graphics_options.env", gamedir);
    }
    else
    {
        snprintf(s_path, sizeof(s_path), "/roms/ports/strikers/graphics_options.env");
    }
    return s_path;
}

static void SavePresetToFile(int presetIdx)
{
    const char* filepath = GetEnvFilePath();
    FILE* f = fopen(filepath, "w");
    if (!f)
    {
        f = fopen("graphics_options.env", "w");
    }
    if (f)
    {
        fprintf(f, "# Super Mario Strikers - Configuración de Rendimiento y Gráficos\n");
        fprintf(f, "export STRIKERS_PRESET=\"%s\"\n", s_presetNames[presetIdx]);
        fprintf(f, "export STRIKERS_CUTSCENE_SKIN=\"%s\"\n", (s_optCutsceneSkin == 1 ? "blend" : "rigid"));
        fprintf(f, "export STRIKERS_CUTSCENE_CROWD=\"%d\"\n", s_optCutsceneCrowd);
        fprintf(f, "export STRIKERS_CUTSCENE_SHADOWS=\"%d\"\n", s_optCutsceneShadows);
        fprintf(f, "export STRIKERS_CUTSCENE_EFFECTS=\"%d\"\n", s_optCutsceneEffects);
        fprintf(f, "export STRIKERS_CUTSCENE_CULL_EXTRA=\"%d\"\n", s_optCutsceneExtra);
        fprintf(f, "export STRIKERS_CUTSCENE_MAX_DIST=\"%.1f\"\n", s_optCutsceneMaxDist);
        fprintf(f, "export STRIKERS_CUTSCENE_LOD=\"%.3f\"\n", s_optCutsceneLod);
        fprintf(f, "export STRIKERS_FAST_TRANSITIONS=\"%s\"\n", (s_optFastTransitions == 2 ? "cut" : (s_optFastTransitions == 1 ? "1" : "0")));
        fprintf(f, "export STRIKERS_DISABLE_SCREENGRAB=\"1\"\n");
        fprintf(f, "export STRIKERS_DISABLE_TRANSITION_OUTLINE=\"1\"\n");
        fprintf(f, "export STRIKERS_CULL_EXTRA_MODELS=\"1\"\n");
        fprintf(f, "export STRIKERS_MAX_PARTICLES=\"%d\"\n", s_optMaxParticles);
        fprintf(f, "export STRIKERS_FENCE_SPARKS=\"%d\"\n", s_optFenceSparks);
        fprintf(f, "export STRIKERS_FENCE_RATE=\"%.2f\"\n", s_optFenceRate);
        fprintf(f, "export STRIKERS_ENABLE_DOF=\"%d\"\n", s_optEnableDof);
        fclose(f);
    }
}

void Strikers_ApplyGraphicsPreset(int presetIdx, bool saveToFile)
{
    if (presetIdx < 0) presetIdx = 0;
    if (presetIdx > 3) presetIdx = 3;
    s_currentPreset = presetIdx;

    switch (presetIdx)
    {
    case GPRESET_NONE: // 0: Ninguna (Original GameCube)
        s_optCutsceneSkin = 1; // blend
        s_optCutsceneCrowd = 1;
        s_optCutsceneShadows = 1;
        s_optCutsceneEffects = 1;
        s_optCutsceneExtra = 0;
        s_optCutsceneMaxDist = 200.0f;
        s_optCutsceneLod = 0.0f;
        s_optFastTransitions = 0; // 3D break glass
        s_optFenceSparks = 1;
        s_optFenceRate = 0.01f;
        s_optEnableDof = 0;
        s_optMaxParticles = 1024;
        break;

    case GPRESET_LOW: // 1: Bajo
        s_optCutsceneSkin = 0; // rigid
        s_optCutsceneCrowd = 1;
        s_optCutsceneShadows = 1;
        s_optCutsceneEffects = 1;
        s_optCutsceneExtra = 1;
        s_optCutsceneMaxDist = 120.0f;
        s_optCutsceneLod = 0.01f;
        s_optFastTransitions = 1; // smooth fade
        s_optFenceSparks = 1;
        s_optFenceRate = 0.03f;
        s_optEnableDof = 0;
        s_optMaxParticles = 768;
        break;

    case GPRESET_MEDIUM: // 2: Medio (Recomendado)
    default:
        s_optCutsceneSkin = 0; // rigid
        s_optCutsceneCrowd = 0;
        s_optCutsceneShadows = 0;
        s_optCutsceneEffects = 0;
        s_optCutsceneExtra = 1;
        s_optCutsceneMaxDist = 85.0f;
        s_optCutsceneLod = 0.035f;
        s_optFastTransitions = 1; // smooth fade
        s_optFenceSparks = 0;
        s_optFenceRate = 0.06f;
        s_optEnableDof = 0;
        s_optMaxParticles = 512;
        break;

    case GPRESET_HIGH: // 3: Alto
        s_optCutsceneSkin = 0; // rigid
        s_optCutsceneCrowd = 0;
        s_optCutsceneShadows = 0;
        s_optCutsceneEffects = 0;
        s_optCutsceneExtra = 1;
        s_optCutsceneMaxDist = 60.0f;
        s_optCutsceneLod = 0.05f;
        s_optFastTransitions = 2; // instant cut
        s_optFenceSparks = 0;
        s_optFenceRate = 0.10f;
        s_optEnableDof = 0;
        s_optMaxParticles = 256;
        break;
    }

    if (saveToFile)
    {
        SavePresetToFile(presetIdx);
    }
}

void Strikers_InitGraphicsPreset()
{
    // Try to load preset name from graphics_options.env
    int preset = GPRESET_MEDIUM;
    const char* filepath = GetEnvFilePath();
    FILE* f = fopen(filepath, "r");
    if (!f)
    {
        f = fopen("graphics_options.env", "r");
    }
    if (f)
    {
        char line[256];
        while (fgets(line, sizeof(line), f))
        {
            if (strstr(line, "STRIKERS_PRESET="))
            {
                if (strstr(line, "Ninguna")) preset = GPRESET_NONE;
                else if (strstr(line, "Bajo")) preset = GPRESET_LOW;
                else if (strstr(line, "Medio")) preset = GPRESET_MEDIUM;
                else if (strstr(line, "Alto")) preset = GPRESET_HIGH;
                break;
            }
        }
        fclose(f);
    }
    else
    {
        const char* envPreset = getenv("STRIKERS_PRESET");
        if (envPreset)
        {
            if (strcmp(envPreset, "Ninguna") == 0) preset = GPRESET_NONE;
            else if (strcmp(envPreset, "Bajo") == 0) preset = GPRESET_LOW;
            else if (strcmp(envPreset, "Medio") == 0) preset = GPRESET_MEDIUM;
            else if (strcmp(envPreset, "Alto") == 0) preset = GPRESET_HIGH;
        }
    }

    Strikers_ApplyGraphicsPreset(preset, false);
}

int Strikers_GetCurrentGraphicsPreset()
{
    return s_currentPreset;
}

int Strikers_GetCutsceneSkin() { return s_optCutsceneSkin; }
int Strikers_GetCutsceneCrowd() { return s_optCutsceneCrowd; }
int Strikers_GetCutsceneShadows() { return s_optCutsceneShadows; }
int Strikers_GetCutsceneEffects() { return s_optCutsceneEffects; }
int Strikers_GetCutsceneCullExtra() { return s_optCutsceneExtra; }
float Strikers_GetCutsceneMaxDist() { return s_optCutsceneMaxDist; }
float Strikers_GetCutsceneLod() { return s_optCutsceneLod; }
int Strikers_GetFastTransitions() { return s_optFastTransitions; }
int Strikers_GetFenceSparks() { return s_optFenceSparks; }
float Strikers_GetFenceRate() { return s_optFenceRate; }
int Strikers_GetEnableDof() { return s_optEnableDof; }
int Strikers_GetMaxParticles() { return s_optMaxParticles; }

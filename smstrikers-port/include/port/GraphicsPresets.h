#ifndef _GRAPHICS_PRESETS_H_
#define _GRAPHICS_PRESETS_H_

enum eGraphicsPreset
{
    GPRESET_NONE = 0,   // Ninguna (Original GameCube)
    GPRESET_LOW = 1,    // Bajo (Mínimos recortes)
    GPRESET_MEDIUM = 2, // Medio (Recomendado RK3326)
    GPRESET_HIGH = 3    // Alto (Máximo rendimiento)
};

enum eResolutionPreset
{
    RESO_AUTO = 0,  // Auto (Detección de pantalla)
    RESO_4_3  = 1,  // 4:3 (Estándar 640x480)
    RESO_16_9 = 2,  // 16:9 (Panorámica 854x480)
    RESO_1_1  = 3,  // 1:1 (Cuadrada 720x720)
    RESO_3_2  = 4   // 3:2 (Clásica 480x320)
};

void Strikers_InitGraphicsPreset();
void Strikers_ApplyGraphicsPreset(int presetIdx, bool saveToFile = true);
int Strikers_GetCurrentGraphicsPreset();

void Strikers_InitResolutionPreset();
void Strikers_ApplyResolutionPreset(int resIdx, bool saveToFile = true);
int Strikers_GetCurrentResolutionPreset();
const char* Strikers_GetResolutionPresetString(int resIdx);
float Strikers_GetCurrentResScale();
void Strikers_SetCurrentResScale(float scale);

// Accessors for active settings
int Strikers_GetCutsceneSkin();
int Strikers_GetCutsceneCrowd();
int Strikers_GetCutsceneShadows();
int Strikers_GetGameplayShadows();
void Strikers_SetGameplayShadows(int enable);
int Strikers_GetCutsceneEffects();
int Strikers_GetCutsceneCullExtra();
float Strikers_GetCutsceneMaxDist();
float Strikers_GetCutsceneLod();
int Strikers_GetFastTransitions();
int Strikers_GetFenceSparks();
float Strikers_GetFenceRate();
int Strikers_GetEnableDof();
int Strikers_GetMaxParticles();
int Strikers_GetReplaysEnabled();
void Strikers_SetReplaysEnabled(int enable);

#endif // _GRAPHICS_PRESETS_H_

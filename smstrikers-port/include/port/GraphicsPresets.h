#ifndef _GRAPHICS_PRESETS_H_
#define _GRAPHICS_PRESETS_H_

enum eGraphicsPreset
{
    GPRESET_NONE = 0,   // Ninguna (Original GameCube)
    GPRESET_LOW = 1,    // Bajo (Mínimos recortes)
    GPRESET_MEDIUM = 2, // Medio (Recomendado RK3326)
    GPRESET_HIGH = 3    // Alto (Máximo rendimiento)
};

void Strikers_InitGraphicsPreset();
void Strikers_ApplyGraphicsPreset(int presetIdx, bool saveToFile = true);
int Strikers_GetCurrentGraphicsPreset();

// Accessors for active settings
int Strikers_GetCutsceneSkin();
int Strikers_GetCutsceneCrowd();
int Strikers_GetCutsceneShadows();
int Strikers_GetCutsceneEffects();
int Strikers_GetCutsceneCullExtra();
float Strikers_GetCutsceneMaxDist();
float Strikers_GetCutsceneLod();
int Strikers_GetFastTransitions();
int Strikers_GetFenceSparks();
float Strikers_GetFenceRate();
int Strikers_GetEnableDof();
int Strikers_GetMaxParticles();

#endif // _GRAPHICS_PRESETS_H_

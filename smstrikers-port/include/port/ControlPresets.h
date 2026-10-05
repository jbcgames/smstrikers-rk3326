#ifndef _CONTROL_PRESETS_H_
#define _CONTROL_PRESETS_H_

enum eControlPreset
{
    CPRESET_ARKOS = 0,       // ArkOS / RG351MP (SWAP Y)
    CPRESET_NORMAL = 1,      // Normal / Estándar
    CPRESET_DPAD = 2,        // Cruceta D-Pad como Stick
    CPRESET_SWAP_STICKS = 3  // Invertir Sticks
};

void Strikers_InitControlPreset();
void Strikers_ApplyControlPreset(int presetIdx, bool saveToFile = true);
int Strikers_GetCurrentControlPreset();
void Strikers_SetGamepadStickMode(bool swapSticks, bool swapY, bool invertY, bool dpadAsStick);

#endif // _CONTROL_PRESETS_H_

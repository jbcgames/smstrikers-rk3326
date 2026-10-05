#include "port/ControlPresets.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern void Strikers_SetGamepadStickMode(bool swapSticks, bool swapY, bool invertY, bool dpadAsStick);

static int s_currentControlPreset = CPRESET_ARKOS;

static const char* s_controlPresetNames[4] = {
    "ArkOS",
    "Normal",
    "Cruceta",
    "Invertir"
};

static const char* GetControlEnvFilePath()
{
    static char s_path[512] = { 0 };
    if (s_path[0] != '\0') return s_path;

    const char* gamedir = getenv("GAMEDIR");
    if (gamedir && gamedir[0] != '\0')
    {
        snprintf(s_path, sizeof(s_path), "%s/custom_controls.env", gamedir);
    }
    else
    {
        snprintf(s_path, sizeof(s_path), "/roms/ports/strikers/custom_controls.env");
    }
    return s_path;
}

static const char* GetControlTxtFilePath()
{
    static char s_path[512] = { 0 };
    if (s_path[0] != '\0') return s_path;

    const char* gamedir = getenv("GAMEDIR");
    if (gamedir && gamedir[0] != '\0')
    {
        snprintf(s_path, sizeof(s_path), "%s/custom_controls.txt", gamedir);
    }
    else
    {
        snprintf(s_path, sizeof(s_path), "/roms/ports/strikers/custom_controls.txt");
    }
    return s_path;
}

static void SaveControlPresetToFile(int presetIdx)
{
    bool swapY = (presetIdx == CPRESET_ARKOS);
    bool dpadAsStick = (presetIdx == CPRESET_DPAD);
    bool swapSticks = (presetIdx == CPRESET_SWAP_STICKS);
    bool invertY = false;

    // 1. Write custom_controls.env
    const char* envPath = GetControlEnvFilePath();
    FILE* fEnv = fopen(envPath, "w");
    if (!fEnv)
    {
        fEnv = fopen("custom_controls.env", "w");
    }
    if (fEnv)
    {
        fprintf(fEnv, "# Super Mario Strikers - Controles (%s)\n", s_controlPresetNames[presetIdx]);
        fprintf(fEnv, "export STRIKERS_PAD_SWAP_Y=%d\n", swapY ? 1 : 0);
        fprintf(fEnv, "export STRIKERS_PAD_SWAP_STICKS=%d\n", swapSticks ? 1 : 0);
        fprintf(fEnv, "export STRIKERS_PAD_INVERT_Y=%d\n", invertY ? 1 : 0);
        fprintf(fEnv, "export STRIKERS_PAD_DPAD_AS_STICK=%d\n", dpadAsStick ? 1 : 0);
        fclose(fEnv);
    }

    // 2. Write custom_controls.txt
    const char* txtPath = GetControlTxtFilePath();
    FILE* fTxt = fopen(txtPath, "w");
    if (!fTxt)
    {
        fTxt = fopen("custom_controls.txt", "w");
    }
    if (fTxt)
    {
        fprintf(fTxt, "# Preset %s\n", s_controlPresetNames[presetIdx]);
        if (presetIdx == CPRESET_ARKOS || presetIdx == CPRESET_DPAD)
        {
            fprintf(fTxt, "19000f6a706c61795f6a6f7973746900,play_joystick,a:b0,b:b1,x:b2,y:b3,back:b8,guide:b10,start:b9,leftstick:b11,rightstick:b12,leftshoulder:b4,rightshoulder:b5,dpup:b14,dpdown:b15,dpleft:b16,dpright:b17,leftx:a0,lefty:a3,rightx:a2,righty:a1,lefttrigger:b6,righttrigger:b7,platform:Linux,\n");
        }
        else if (presetIdx == CPRESET_SWAP_STICKS)
        {
            fprintf(fTxt, "19000f6a706c61795f6a6f7973746900,play_joystick,a:b0,b:b1,x:b2,y:b3,back:b8,guide:b10,start:b9,leftstick:b11,rightstick:b12,leftshoulder:b4,rightshoulder:b5,dpup:b14,dpdown:b15,dpleft:b16,dpright:b17,leftx:a2,lefty:a1,rightx:a0,righty:a3,lefttrigger:b6,righttrigger:b7,platform:Linux,\n");
        }
        else // Normal
        {
            fprintf(fTxt, "19000f6a706c61795f6a6f7973746900,play_joystick,a:b0,b:b1,x:b2,y:b3,back:b8,guide:b10,start:b9,leftstick:b11,rightstick:b12,leftshoulder:b4,rightshoulder:b5,dpup:b14,dpdown:b15,dpleft:b16,dpright:b17,leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:b6,righttrigger:b7,platform:Linux,\n");
        }
        fclose(fTxt);
    }
}

void Strikers_ApplyControlPreset(int presetIdx, bool saveToFile)
{
    if (presetIdx < 0) presetIdx = 0;
    if (presetIdx > 3) presetIdx = 3;
    s_currentControlPreset = presetIdx;

    bool swapY = (presetIdx == CPRESET_ARKOS);
    bool dpadAsStick = (presetIdx == CPRESET_DPAD);
    bool swapSticks = (presetIdx == CPRESET_SWAP_STICKS);
    bool invertY = false;

    Strikers_SetGamepadStickMode(swapSticks, swapY, invertY, dpadAsStick);

    if (saveToFile)
    {
        SaveControlPresetToFile(presetIdx);
    }
}

void Strikers_InitControlPreset()
{
    int preset = CPRESET_ARKOS; // Default on handheld
    const char* filepath = GetControlEnvFilePath();
    FILE* f = fopen(filepath, "r");
    if (!f)
    {
        f = fopen("custom_controls.env", "r");
    }
    if (f)
    {
        char line[256];
        bool hasSwapY = false;
        bool hasSwapSticks = false;
        bool hasDpadStick = false;

        while (fgets(line, sizeof(line), f))
        {
            if (strstr(line, "STRIKERS_PAD_SWAP_Y=1")) hasSwapY = true;
            if (strstr(line, "STRIKERS_PAD_SWAP_STICKS=1")) hasSwapSticks = true;
            if (strstr(line, "STRIKERS_PAD_DPAD_AS_STICK=1")) hasDpadStick = true;
        }
        fclose(f);

        if (hasDpadStick) preset = CPRESET_DPAD;
        else if (hasSwapSticks) preset = CPRESET_SWAP_STICKS;
        else if (hasSwapY) preset = CPRESET_ARKOS;
        else preset = CPRESET_NORMAL;
    }
    else
    {
        const char* eDpad = getenv("STRIKERS_PAD_DPAD_AS_STICK");
        const char* eSwapSticks = getenv("STRIKERS_PAD_SWAP_STICKS");
        const char* eSwapY = getenv("STRIKERS_PAD_SWAP_Y");

        if (eDpad && strcmp(eDpad, "1") == 0) preset = CPRESET_DPAD;
        else if (eSwapSticks && strcmp(eSwapSticks, "1") == 0) preset = CPRESET_SWAP_STICKS;
        else if (eSwapY && strcmp(eSwapY, "1") == 0) preset = CPRESET_ARKOS;
        else if (eSwapY && strcmp(eSwapY, "0") == 0) preset = CPRESET_NORMAL;
    }

    Strikers_ApplyControlPreset(preset, false);
}

int Strikers_GetCurrentControlPreset()
{
    return s_currentControlPreset;
}

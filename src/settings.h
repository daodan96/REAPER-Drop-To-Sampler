#pragma once

#include "platform/platform.h"

// User settings, stored in REAPER ExtState

enum SamplerMode
{
    Sampler_RS5k = 0,
    Sampler_Cartridge = 1,
    Sampler_AlwaysPrompt = 2
};

enum RecArmMode
{
    RecArm_Default = 0,
    RecArm_On = 1,
    RecArm_Auto = 2
};

enum ShowSamplerMode
{
    ShowSampler_Default = 0,
    ShowSampler_Yes = 1,
    ShowSampler_No = 2
};

enum MultiFilesMode
{
    MultiFiles_OneTrack = 0,
    MultiFiles_MultiTracks = 1,
    MultiFiles_AlwaysPrompt = 2
};

enum MultiSamplersMode
{
    MultiSamplers_Layers = 0,
    MultiSamplers_Chromatic = 1,
    MultiSamplers_AlwaysPrompt = 2
};

enum ExistingSamplerMode
{
    Existing_Add = 0,
    Existing_Replace = 1,
    Existing_ReplaceOnSlot = 2
};

enum SurfaceDropMode
{
    SurfaceDrop_Disabled = 0,
    SurfaceDrop_SamplerWithModifier = 1,
    SurfaceDrop_NativeWithModifier = 2
};

// Single modifier key (no chords). 0 = not assigned.
enum ModifierKey
{
    ModKey_None = 0,
    ModKey_Shift = 1,
    ModKey_Ctrl = 2,
    ModKey_Alt = 3
};

struct Settings
{
    bool debug = false;
    int samplerMode = Sampler_RS5k;
    int recArmMode = RecArm_Auto;
    int showSamplerMode = ShowSampler_Default;
    int multiFilesMode = MultiFiles_MultiTracks;
    int multiSamplersMode = MultiSamplers_Layers;
    int existingMode = Existing_ReplaceOnSlot;
    int arrangeDropMode = SurfaceDrop_SamplerWithModifier;
    int tcpDropMode = SurfaceDrop_NativeWithModifier;

    // Modifier assignments (defaults match classic Shift / Ctrl / Alt roles)
    int modSurface = ModKey_Alt;      // Arrange + TCP/MCP insert sampler <-> item
    int modPlacement = ModKey_Shift;  // one track <-> multi tracks
    int modLayers = ModKey_Ctrl;      // Layers <-> Chromatic
    int modReplace = ModKey_None;     // Add <-> Replace (Not set: use settings only)
};

extern Settings g_settings;

void LoadSettings();
void SaveSettings();

void DebugLog(const char* fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 1, 2)))
#endif
    ;

inline bool WantSamplerForMode(int surfaceMode, bool modifierDown)
{
    if (surfaceMode == SurfaceDrop_Disabled)
        return false;
    if (surfaceMode == SurfaceDrop_SamplerWithModifier)
        return modifierDown;
    return !modifierDown;
}

inline bool IsModifierDown(const Modifiers& m, int key)
{
    switch (key)
    {
    case ModKey_Shift: return m.shift;
    case ModKey_Ctrl:  return m.ctrl;
    case ModKey_Alt:   return m.alt;
    default:           return false;
    }
}

// UI / status: "Shift", "Ctrl"/"Cmd", "Alt"/"Option", or nullptr if Not set
inline const char* ModifierKeyName(int key)
{
    switch (key)
    {
    case ModKey_Shift: return DTS_KEY_SHIFT;
    case ModKey_Ctrl:  return DTS_KEY_CTRL;
    case ModKey_Alt:   return DTS_KEY_ALT;
    default:           return nullptr;
    }
}

// Assign key to a slot; any other slot that held the same key becomes Not set.
void AssignModifierUnique(int* slots[], int count, int slotIndex, int key);

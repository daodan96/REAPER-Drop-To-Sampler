#include "drop.h"

#include "reaper_plugin.h"
#include "reaper_plugin_functions.h"
#include "prompts.h"
#include "sampler.h"
#include "settings.h"
#include "tracks.h"

#include <cstdio>
#include <cstring>

// Shift toggles one track <-> multiple tracks (not with Always prompt)
static int ToggledMultiFilesMode(int mode, const Modifiers& mods)
{
    if (!IsModifierDown(mods, g_settings.modPlacement)) return mode;
    if (mode == MultiFiles_OneTrack) return MultiFiles_MultiTracks;
    if (mode == MultiFiles_MultiTracks) return MultiFiles_OneTrack;
    return mode;
}

static int ToggledMultiSamplersMode(int mode, const Modifiers& mods)
{
    if (!IsModifierDown(mods, g_settings.modLayers)) return mode;
    if (mode == MultiSamplers_Layers) return MultiSamplers_Chromatic;
    if (mode == MultiSamplers_Chromatic) return MultiSamplers_Layers;
    return mode;
}

// ---------------------------------------------------------------------------
// Status line under REAPER panels (Help_Set) during drag
// ---------------------------------------------------------------------------
void ClearDropHelp()
{
    if (Help_Set)
        Help_Set("", true);
}

static void SetDropHelp(const char* msg)
{
    // Temporary help, re-asserted on every drag-over so REAPER keeps showing it
    if (Help_Set && msg)
        Help_Set(msg, true);
}

DropVisualKind UpdateDropHelp(const Modifiers& mods, int screenX, int screenY, int fileCount)
{
    if (!Help_Set) return DropVisual_Insert;
    if (fileCount < 1) fileCount = 1;

    const Settings& s = g_settings;
    const bool multiFiles = (fileCount > 1);

    int multiFilesMode = s.multiFilesMode;
    if (multiFiles)
        multiFilesMode = ToggledMultiFilesMode(multiFilesMode, mods);

    // Replace-on-slot: Alt ignored. Slot hit -> replace, else -> Add.
    // Add/Replace: Alt toggles. Multiple media: always Add.
    int existingMode = s.existingMode;
    if (multiFiles)
        existingMode = Existing_Add;
    else if (s.existingMode == Existing_ReplaceOnSlot)
        existingMode = Existing_ReplaceOnSlot;
    else if (IsModifierDown(mods, s.modReplace))
        existingMode = (s.existingMode == Existing_Add) ? Existing_Replace : Existing_Add;

    int multiSamplersMode = ToggledMultiSamplersMode(s.multiSamplersMode, mods);

    int samplerType = (s.samplerMode == Sampler_Cartridge) ? Sampler_Cartridge : Sampler_RS5k;

    MediaTrack* track = nullptr;
    char info[256] = {};
    if (GetThingFromPoint)
        track = GetThingFromPoint(screenX, screenY, info, sizeof(info));

    bool overFxSlot = (strstr(info, "tcp.fxlist") != nullptr || strstr(info, "mcp.fxlist") != nullptr
        || strstr(info, "arrange.fxlist") != nullptr);
    bool slotHasFx = overFxSlot && (strstr(info, "fx:") != nullptr);
    // arrange: same drop semantics as TCP/MCP when Drop to arrange is enabled
    bool onTrackPanel = (strstr(info, "tcp") == info || strstr(info, "mcp") == info
        || strstr(info, "arrange") == info);

    auto layerLabel = [&]() -> const char* {
        if (multiSamplersMode == MultiSamplers_AlwaysPrompt)
            return "Ask: Layers/Chromatic";
        const char* key = ModifierKeyName(s.modLayers);
        const bool down = IsModifierDown(mods, s.modLayers);
        if (multiSamplersMode == MultiSamplers_Chromatic)
        {
            if (!key || down) return "Chromatic";
            static char buf[64];
            snprintf(buf, sizeof(buf), "Chromatic (%s toggles)", key);
            return buf;
        }
        if (!key || down) return "Layers";
        static char buf2[64];
        snprintf(buf2, sizeof(buf2), "Layers (%s toggles)", key);
        return buf2;
    };

    auto placeLabel = [&]() -> const char* {
        if (multiFilesMode == MultiFiles_AlwaysPrompt)
            return "Ask: one / multiple tracks";
        const char* key = ModifierKeyName(s.modPlacement);
        const bool down = IsModifierDown(mods, s.modPlacement);
        if (multiFilesMode == MultiFiles_MultiTracks)
        {
            if (!key || down) return "On multiple tracks";
            static char buf[64];
            snprintf(buf, sizeof(buf), "On multiple tracks (%s toggles)", key);
            return buf;
        }
        if (!key || down) return "On one track";
        static char buf2[64];
        snprintf(buf2, sizeof(buf2), "On one track (%s toggles)", key);
        return buf2;
    };

    // --- Replace on FX slot ---
    if (!multiFiles && existingMode == Existing_ReplaceOnSlot && slotHasFx && track)
    {
        int fx = ParseFxIndexFromHitInfo(info);
        if (fx >= 0 && TrackFX_GetFXName)
        {
            char name[256] = {};
            TrackFX_GetFXName(track, fx, name, sizeof(name));
            if (FxNameMatchesSamplerType(name, samplerType)
                || (s.samplerMode == Sampler_AlwaysPrompt
                    && (FxNameIsRS5k(name) || FxNameIsCartridge(name))))
            {
                SetDropHelp("Replace media");
                return DropVisual_Replace;
            }
        }
    }

    // --- Replace (settings Replace, or Alt from Add) ---
    if (!multiFiles && existingMode == Existing_Replace && track && onTrackPanel)
    {
        int cnt = 0;
        FindSamplerFx(track, samplerType, &cnt);
        if (cnt == 1)
        {
            // "(Alt toggles)" only when key not held and base mode is Replace or Add (toggle available)
            if (!IsModifierDown(mods, s.modReplace) && s.existingMode != Existing_ReplaceOnSlot)
                SetDropHelp("Replace media (" DTS_KEY_ALT " toggles)");
            else
                SetDropHelp("Replace media");
            return DropVisual_Replace;
        }
    }

    int existingOnTrack = 0;
    if (track && onTrackPanel)
        FindSamplerFx(track, samplerType, &existingOnTrack);

    // --- Multiple media ---
    if (multiFiles)
    {
        char buf[220];
        if (multiFilesMode == MultiFiles_MultiTracks)
            snprintf(buf, sizeof(buf), "New samplers (%d) · %s", fileCount, placeLabel());
        else
            snprintf(buf, sizeof(buf), "New samplers (%d) · %s · %s",
                fileCount, placeLabel(), layerLabel());
        SetDropHelp(buf);
        return DropVisual_Insert;
    }

    // --- Single media, Add path (also Replace-on-slot when not on slot) ---
    if (existingOnTrack >= 1)
    {
        char buf[180];
        snprintf(buf, sizeof(buf), "New sampler · %s", layerLabel());
        SetDropHelp(buf);
        return DropVisual_Insert;
    }

    SetDropHelp("New sampler");
    return DropVisual_Insert;
}

// ---------------------------------------------------------------------------
// Drop
// ---------------------------------------------------------------------------
void ProcessDrop(const std::vector<std::string>& files, int screenX, int screenY, const Modifiers& mods)
{
    const int nFiles = (int)files.size();
    if (nFiles == 0) return;

    bool createdNew = false;
    MediaTrack* track = ResolveTrackAtPoint(screenX, screenY, &createdNew);
    if (!track)
    {
        DebugLog("[Drop To Sampler] could not resolve track\n");
        return;
    }

    // Resolve prompts in order:
    // 1) Multiple files (if needed)
    // 2) Sampler type (if needed)
    // 3) Multiple samplers Layers/Chromatic only when stacking on one track
    int multiFilesMode = g_settings.multiFilesMode;
    if (nFiles > 1 && multiFilesMode == MultiFiles_AlwaysPrompt)
    {
        multiFilesMode = PromptMultiFilesChoice();
        if (multiFilesMode < 0)
            return;
    }
    else if (nFiles > 1)
    {
        multiFilesMode = ToggledMultiFilesMode(multiFilesMode, mods);
        if (IsModifierDown(mods, g_settings.modPlacement))
            DebugLog(multiFilesMode == MultiFiles_MultiTracks
                ? "[Drop To Sampler] placement mod: multiple tracks\n"
                : "[Drop To Sampler] placement mod: one track\n");
    }

    int resolvedSampler = g_settings.samplerMode;
    if (resolvedSampler == Sampler_AlwaysPrompt)
    {
        resolvedSampler = PromptSamplerChoice();
        if (resolvedSampler < 0)
            return;
    }
    int samplerType = (resolvedSampler == Sampler_Cartridge) ? Sampler_Cartridge : Sampler_RS5k;

    // Existing samplers of chosen type on the drop target track
    int existingOnTrack = 0;
    FindSamplerFx(track, samplerType, &existingOnTrack);

    // Effective "If sampler exists" mode:
    // Replace-on-slot: Alt ignored (slot -> replace, else add).
    // Add/Replace: Alt toggles Add <-> Replace.
    int existingMode = g_settings.existingMode;
    if (IsModifierDown(mods, g_settings.modReplace) && existingMode != Existing_ReplaceOnSlot)
    {
        existingMode = (existingMode == Existing_Add) ? Existing_Replace : Existing_Add;
        DebugLog(existingMode == Existing_Add
            ? "[Drop To Sampler] replace mod: Add\n"
            : "[Drop To Sampler] replace mod: Replace\n");
    }

    // Single file that will REPLACE -> no Layers/Chromatic prompt
    bool willReplaceExisting = false;
    if (nFiles == 1)
    {
        if (existingMode == Existing_ReplaceOnSlot)
        {
            if (HitTestSamplerSlot(screenX, screenY, track, samplerType) >= 0)
                willReplaceExisting = true;
        }
        else if (existingMode == Existing_Replace)
        {
            int cnt = 0;
            FindSamplerFx(track, samplerType, &cnt);
            if (cnt == 1)
                willReplaceExisting = true;
        }
    }

    int multiSamplersMode = ToggledMultiSamplersMode(g_settings.multiSamplersMode, mods);
    if (IsModifierDown(mods, g_settings.modLayers) && multiSamplersMode != MultiSamplers_AlwaysPrompt)
        DebugLog(multiSamplersMode == MultiSamplers_Chromatic
            ? "[Drop To Sampler] layers mod: Chromatic\n"
            : "[Drop To Sampler] layers mod: Layers\n");

    // Multi-track layout only applies when dropping several files; a single file can still use Chromatic
    const bool multiTracks = (multiFilesMode == MultiFiles_MultiTracks && nFiles > 1);
    if (multiTracks || willReplaceExisting)
    {
        multiSamplersMode = MultiSamplers_Layers;
    }
    else if (multiSamplersMode == MultiSamplers_AlwaysPrompt)
    {
        bool willStack = (nFiles > 1) || (existingOnTrack >= 1);
        if (willStack)
        {
            multiSamplersMode = PromptMultiSamplersChoice();
            if (multiSamplersMode < 0)
                return;
        }
        else
        {
            multiSamplersMode = MultiSamplers_Layers;
        }
    }

    // Chromatic only when stacking; replace / empty track -> full range
    bool chromatic = false;
    if (!willReplaceExisting && multiSamplersMode == MultiSamplers_Chromatic)
    {
        if (nFiles > 1 && multiFilesMode == MultiFiles_OneTrack)
            chromatic = true;
        else if (nFiles == 1 && existingOnTrack >= 1)
            chromatic = true;
    }

    // Chromatic: rebuild existing samplers on the target track from C4, then new ones continue
    if (chromatic && !multiTracks)
        RemapTrackSamplersChromatic(track, samplerType);

    if (Undo_BeginBlock) Undo_BeginBlock();

    if (g_settings.debug && GetThingFromPoint)
    {
        char info[256] = {};
        GetThingFromPoint(screenX, screenY, info, sizeof(info));
        DebugLog("[Drop To Sampler] drop hit info=%s\n", info);
    }

    AddSamplerOptions opt;
    opt.existingMode = existingMode;
    opt.screenX = screenX;
    opt.screenY = screenY;
    opt.chromatic = chromatic;
    opt.samplerMode = resolvedSampler;

    MediaTrack* lastNewTrack = nullptr;

    if (multiTracks)
    {
        // Drop target is only an anchor: insert N new tracks AFTER it (or at end if empty area).
        // Example: drop 3 files on "2-Drums" with "3-Synth" below ->
        //   2-Drums (unchanged), 3-SampleA, 4-SampleB, 5-SampleC, 6-Synth
        // Empty TCP area: ResolveTrackAtPoint already appended one track for the drop;
        // reuse it as the first new track.
        int insertAt;
        if (createdNew)
        {
            insertAt = GetTrackIndex0(track);
            if (insertAt < 0)
                insertAt = CountTracks ? CountTracks(nullptr) - 1 : 0;
        }
        else
        {
            int anchor = GetTrackIndex0(track);
            if (anchor < 0)
                anchor = (CountTracks ? CountTracks(nullptr) : 1) - 1;
            insertAt = anchor + 1;
        }

        opt.forceHide = true;
        opt.isLastOnTrack = true;
        opt.allowReplace = false;
        for (int i = 0; i < nFiles; ++i)
        {
            MediaTrack* tr = (createdNew && i == 0) ? track : InsertTrackAt(insertAt + i);
            if (!tr) continue;
            ApplyRecArmForNewTrack(tr);
            lastNewTrack = tr;
            AddSamplerWithFile(tr, files[i].c_str(), opt);
            MaybeNameTrackAfterSample(tr, files[i].c_str(), false, false);
        }
    }
    else
    {
        if (createdNew)
        {
            ApplyRecArmForNewTrack(track);
            lastNewTrack = track;
        }
        opt.forceHide = false;
        opt.allowReplace = (nFiles == 1);
        for (int i = 0; i < nFiles; ++i)
        {
            opt.isLastOnTrack = (i + 1 == nFiles);
            AddSamplerWithFile(track, files[i].c_str(), opt);
        }
        // Multiple samples stacked on one track -> do not auto-name.
        // Single sample + empty name + no sampler before -> name from file.
        if (nFiles == 1)
            MaybeNameTrackAfterSample(track, files[0].c_str(), existingOnTrack >= 1, false);
    }

    if (lastNewTrack)
        SelectOnlyTrack(lastNewTrack);

    if (Undo_EndBlock)
        Undo_EndBlock("Insert sampler from TCP drop", -1);
}

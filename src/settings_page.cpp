#include "settings_page.h"

#include "reaper_plugin_functions.h"
#include "resource.h"
#include "settings.h"
#include "platform/platform.h"
#include "version.h"

#include <cstdio>

#ifndef _WIN32
// Dialog from resource.rc, converted for SWELL by swell_resgen at build time
#include "swell/swell-dlggen.h"
#include "resource.rc_mac_dlg"
#endif

// REAPER Preferences window: help text area and Apply button, and the message
// it sends to pages on OK/Apply
static const int IDC_PREFS_HELP = 0x4eb;
static const int IDC_PREFS_APPLY_BTN = 0x478;
#define WM_PREFS_APPLY (WM_USER * 2)

static REAPER_PLUGIN_HINSTANCE g_hInstance = nullptr;
static const char* g_shownPrefsHelp = nullptr;

static const char* HintForControlId(int id)
{
    switch (id)
    {
    case IDC_SAMPLER_COMBO:
    case IDC_LABEL_SAMPLER:
        return "Which sampler to insert. If Cartridge fails to load, RS5k is used instead.";
    case IDC_RECARM_COMBO:
    case IDC_LABEL_RECARM:
        return "Set record-arm or auto record-arm for newly created tracks. Also sets input to All MIDI inputs if enabled.";
    case IDC_SHOW_COMBO:
    case IDC_LABEL_SHOW:
        return "Show floating FX window after insert. Default follows REAPER preferences.";
    case IDC_EXISTING_COMBO:
    case IDC_LABEL_EXISTING:
        return "Add or Replace sample when a sampler is already on the track. Replace ignored when multiple media dragged. Optional modifier toggles Add/Replace.";
    case IDC_MOD_REPLACE_COMBO:
    case IDC_LABEL_MOD_REPLACE:
        return "Modifier that toggles Add/Replace while dragging. Not set: only the setting above applies. Replace on FX slot ignores this modifier.";
    case IDC_MULTI_COMBO:
    case IDC_LABEL_MULTI:
        return "Where to place multiple dropped media. Optional modifier toggles one track / multiple tracks.";
    case IDC_MOD_PLACE_COMBO:
    case IDC_LABEL_MOD_PLACE:
        return "Modifier that toggles one track vs multiple tracks for multi-file drops.";
    case IDC_MULTISAMP_COMBO:
    case IDC_LABEL_MULTISAMP:
        return "Layers: default note range. Chromatic: one note per sample starting from C4 (including existing samplers).";
    case IDC_MOD_LAYERS_COMBO:
    case IDC_LABEL_MOD_LAYERS:
        return "Modifier that toggles Layers vs Chromatic.";
    case IDC_ARRANGE_COMBO:
    case IDC_LABEL_ARRANGE:
        return "Arrange drops. Disabled: no intercept. Insert sampler with modifier: modifier -> sampler, else item. Insert item with modifier: modifier -> item, else sampler.";
    case IDC_TCP_COMBO:
    case IDC_LABEL_TCP:
        return "TCP/MCP drops. Disabled: no intercept. Insert sampler with modifier: modifier -> sampler, else item. Insert item with modifier: modifier -> item, else sampler.";
    case IDC_MOD_SURFACE_COMBO:
    case IDC_LABEL_MOD_SURFACE:
        return "Shared modifier for Arrange and TCP/MCP insert sampler vs insert item modes.";
    case IDC_DEBUG_CHECK:
        return "Log drop details to the REAPER console.";
    default:
        return "";
    }
}

// Shows the hint for the control under the mouse in the Preferences help area
static void UpdatePrefsHelp(HWND page)
{
    if (!IsWindowVisible(page))
    {
        g_shownPrefsHelp = nullptr;
        return;
    }
    HWND prefs = GetParent(page);
    if (!prefs) return;

    POINT pt;
    GetCursorPos(&pt);
    HWND child = WindowFromPoint(pt);
    // walk up to a direct child of page
    while (child && GetParent(child) != page && child != page)
        child = GetParent(child);

    const char* hint = "";
    if (child && child != page)
        hint = HintForControlId((int)GetWindowLong(child, GWL_ID));
    if (hint == g_shownPrefsHelp)
        return;
    g_shownPrefsHelp = hint;
    SetDlgItemText(prefs, IDC_PREFS_HELP, hint);
}

static void NotifyPrefsApplyEnabled(HWND page)
{
    HWND prefs = GetParent(page);
    HWND apply = prefs ? GetDlgItem(prefs, IDC_PREFS_APPLY_BTN) : nullptr;
    if (apply) EnableWindow(apply, TRUE);
}

struct ComboSetting
{
    int id;
    int* value;
    const char* items[4]; // null-terminated list
};

static ComboSetting* ComboSettings()
{
    static ComboSetting combos[] = {
        { IDC_SAMPLER_COMBO, &g_settings.samplerMode,
            { "ReaSamplOmatic5000 (Cockos)", "Cartridge (DsgDnB)", "Always prompt", nullptr } },
        { IDC_RECARM_COMBO, &g_settings.recArmMode,
            { "Default", "Rec-arm", "Auto rec-arm", nullptr } },
        { IDC_SHOW_COMBO, &g_settings.showSamplerMode,
            { "Default", "Yes", "No", nullptr } },
        { IDC_EXISTING_COMBO, &g_settings.existingMode,
            { "Add", "Replace", "Replace on sampler FX slot", nullptr } },
        { IDC_MULTI_COMBO, &g_settings.multiFilesMode,
            { "On one track", "On multiple tracks", "Always prompt", nullptr } },
        { IDC_MULTISAMP_COMBO, &g_settings.multiSamplersMode,
            { "Layers", "Chromatic", "Always prompt", nullptr } },
        { IDC_ARRANGE_COMBO, &g_settings.arrangeDropMode,
            { "Disabled", "Insert sampler with modifier", "Insert item with modifier", nullptr } },
        { IDC_TCP_COMBO, &g_settings.tcpDropMode,
            { "Disabled", "Insert sampler with modifier", "Insert item with modifier", nullptr } },
        { 0, nullptr, {} }
    };
    return combos;
}

static const char* kModItems[] = { "Not set", "Shift", "Ctrl", "Alt", nullptr };

struct ModCombo
{
    int id;
    int* value;
};

static ModCombo* ModCombos()
{
    static ModCombo mods[] = {
        { IDC_MOD_REPLACE_COMBO, &g_settings.modReplace },
        { IDC_MOD_PLACE_COMBO, &g_settings.modPlacement },
        { IDC_MOD_LAYERS_COMBO, &g_settings.modLayers },
        { IDC_MOD_SURFACE_COMBO, &g_settings.modSurface },
        { 0, nullptr }
    };
    return mods;
}

static void PopulateControls(HWND hwnd)
{
    for (ComboSetting* c = ComboSettings(); c->id; ++c)
    {
        HWND combo = GetDlgItem(hwnd, c->id);
        for (int i = 0; c->items[i]; ++i)
            SendMessage(combo, CB_ADDSTRING, 0, (LPARAM)c->items[i]);
        SendMessage(combo, CB_SETCURSEL, *c->value, 0);
    }
    for (ModCombo* m = ModCombos(); m->id; ++m)
    {
        HWND combo = GetDlgItem(hwnd, m->id);
        SendMessage(combo, CB_ADDSTRING, 0, (LPARAM)"Not set");
        SendMessage(combo, CB_ADDSTRING, 0, (LPARAM)DTS_KEY_SHIFT);
        SendMessage(combo, CB_ADDSTRING, 0, (LPARAM)DTS_KEY_CTRL);
        SendMessage(combo, CB_ADDSTRING, 0, (LPARAM)DTS_KEY_ALT);
        SendMessage(combo, CB_SETCURSEL, *m->value, 0);
    }
    CheckDlgButton(hwnd, IDC_DEBUG_CHECK, g_settings.debug ? BST_CHECKED : BST_UNCHECKED);

    char ver[64];
    snprintf(ver, sizeof(ver), "v%s", DTS_VERSION_STR);
    SetDlgItemText(hwnd, IDC_VERSION_LABEL, ver);
}

static void ApplyControls(HWND hwnd)
{
    g_settings.debug = IsDlgButtonChecked(hwnd, IDC_DEBUG_CHECK) == BST_CHECKED;
    for (ComboSetting* c = ComboSettings(); c->id; ++c)
    {
        int sel = (int)SendMessage(GetDlgItem(hwnd, c->id), CB_GETCURSEL, 0, 0);
        int n = 0;
        while (c->items[n]) ++n;
        if (sel >= 0 && sel < n)
            *c->value = sel;
    }

    // Read modifiers then resolve conflicts (last slot in list wins for a shared key)
    int* slots[] = {
        &g_settings.modReplace,
        &g_settings.modPlacement,
        &g_settings.modLayers,
        &g_settings.modSurface,
    };
    const int kSlotCount = 4;
    int raw[4] = {};
    int ids[4] = {
        IDC_MOD_REPLACE_COMBO, IDC_MOD_PLACE_COMBO, IDC_MOD_LAYERS_COMBO, IDC_MOD_SURFACE_COMBO
    };
    for (int i = 0; i < kSlotCount; ++i)
    {
        int sel = (int)SendMessage(GetDlgItem(hwnd, ids[i]), CB_GETCURSEL, 0, 0);
        raw[i] = (sel >= 0 && sel <= ModKey_Alt) ? sel : ModKey_None;
    }
    for (int i = 0; i < kSlotCount; ++i)
        *slots[i] = ModKey_None;
    for (int i = 0; i < kSlotCount; ++i)
        AssignModifierUnique(slots, kSlotCount, i, raw[i]);

    // Refresh UI so cleared slots show Not set
    for (int i = 0; i < kSlotCount; ++i)
        SendMessage(GetDlgItem(hwnd, ids[i]), CB_SETCURSEL, *slots[i], 0);

    SaveSettings();
}

static INT_PTR CALLBACK PrefsDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    (void)lParam;
    switch (msg)
    {
    case WM_INITDIALOG:
        PopulateControls(hwnd);
        return TRUE;

    case WM_PREFS_APPLY:
        ApplyControls(hwnd);
        return TRUE;

    case WM_COMMAND:
        if (HIWORD(wParam) == CBN_SELCHANGE || HIWORD(wParam) == BN_CLICKED)
            NotifyPrefsApplyEnabled(hwnd);
        return TRUE;

    case WM_SETCURSOR:
        UpdatePrefsHelp(hwnd);
        break;

    case WM_MOUSEMOVE:
        UpdatePrefsHelp(hwnd);
        return TRUE;
    }
    return FALSE;
}

static HWND PrefsPageCreate(HWND parent)
{
    return CreateDialog((HINSTANCE)g_hInstance, MAKEINTRESOURCE(IDD_PREFS), parent, PrefsDlgProc);
}

static prefs_page_register_t g_prefsPage = {
    "drop_to_sampler",
    "Drop To Sampler",
    PrefsPageCreate,
    0x9a, // parent: Plug-ins
    "",
    0,
    nullptr,
    nullptr,
    {}
};

void RegisterSettingsPage(reaper_plugin_info_t* rec, REAPER_PLUGIN_HINSTANCE hInstance)
{
    g_hInstance = hInstance;
    rec->Register("prefpage", (void*)&g_prefsPage);
}

void UnregisterSettingsPage()
{
    if (plugin_register)
        plugin_register("-prefpage", (void*)&g_prefsPage);
}

void OpenSettingsPage()
{
    if (ViewPrefs)
        ViewPrefs(0, "drop_to_sampler");
}

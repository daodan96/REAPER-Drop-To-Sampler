/*
 * Drop To Sampler
 * File drop on TCP/MCP -> insert sampler with sample loaded.
 *
 * main.cpp       entry point, action/menu, attaching drop targets to TCP/MCP/arrange
 * drop.cpp       what a drop does (modes, modifiers, prompts) + status line preview
 * sampler.cpp    RS5k / Cartridge FX: add, replace sample, chromatic mapping
 * tracks.cpp     track helpers
 * settings*.cpp  settings storage and the Preferences page
 * platform/      OS-specific parts (drop interception, cursor overlay, files)
 */

#define REAPERAPI_IMPLEMENT
#include "reaper_plugin.h"
#include "reaper_plugin_functions.h"

#include "drop.h"
#include "platform/platform.h"
#include "settings.h"
#include "settings_page.h"

#include <algorithm>
#include <cstring>
#include <vector>

// Forwards drags from DropTargets to the drop logic, with status line + overlay feedback
class SamplerDragHandler : public DragHandler
{
public:
    void OnDragEnter(int fileCount, int x, int y, const Modifiers& mods) override
    {
        m_fileCount = fileCount;
        OnDragOver(x, y, mods);
    }

    void OnDragOver(int x, int y, const Modifiers& mods) override
    {
        DropVisualKind kind = UpdateDropHelp(mods, x, y, m_fileCount);
        if (m_overlay) m_overlay->ShowAtCursor(kind);
    }

    void OnDragLeave() override
    {
        if (m_overlay) m_overlay->Hide();
        ClearDropHelp();
        m_fileCount = 0;
    }

    void OnDrop(const std::vector<std::string>& files, int x, int y, const Modifiers& mods) override
    {
        OnDragLeave();
        ProcessDrop(files, x, y, mods);
    }

    std::unique_ptr<DragOverlay> m_overlay;

private:
    int m_fileCount = 0;
};

static SamplerDragHandler* g_dragHandler = nullptr;
static int g_cmdSettings = 0;

struct AttachedWindow
{
    HWND hwnd = nullptr;
    std::unique_ptr<DropTarget> target;
};
static AttachedWindow g_tcp;
static std::vector<AttachedWindow> g_mcps;
static AttachedWindow g_arrange;
static int g_timerTicks = 0;

// ---------------------------------------------------------------------------
// TCP / MCP windows
// ---------------------------------------------------------------------------
static bool ClassNameIs(HWND hwnd, const char* name)
{
    char cls[256] = {};
    return hwnd && GetClassName(hwnd, cls, sizeof(cls)) && strcmp(cls, name) == 0;
}

static HWND FindTcpWindow()
{
    HWND main = GetMainHwnd ? GetMainHwnd() : nullptr;
    if (!main) return nullptr;

    // Older SWELL FindWindowEx() ignores the class name, so check it explicitly
    HWND tcp = FindWindowEx(main, nullptr, "REAPERTCPDisplay", nullptr);
    if (ClassNameIs(tcp, "REAPERTCPDisplay")) return tcp;

    HWND child = GetWindow(main, GW_CHILD);
    while (child)
    {
        char cls[256] = {};
        GetClassName(child, cls, sizeof(cls));
        if (strstr(cls, "TCP") || strstr(cls, "tcp"))
            return child;
        child = GetWindow(child, GW_HWNDNEXT);
    }
    return nullptr;
}

// Starting HWND for arrange OLE discovery. Control 1000 is often REAPERTrackListWindow
// (no OLE of its own); AttachPassthrough may walk parents to the real OLE owner.
static HWND FindArrangeWindow()
{
    HWND main = GetMainHwnd ? GetMainHwnd() : nullptr;
    if (!main) return nullptr;
    HWND view = GetDlgItem(main, 1000);
    return view ? view : main;
}

// The mixer can have several REAPERMCPDisplay windows (e.g. master and tracks, or a
// master docked apart): take over drops on all of them. Drops reach only the window
// they were registered on (Windows/macOS) or the deepest window under the cursor
// (Linux), so attaching to one of them is not enough.
static bool IsMcpClass(HWND hwnd)
{
    char cls[256] = {};
    GetClassName(hwnd, cls, sizeof(cls));
    return strstr(cls, "MCP") || strstr(cls, "mcp");
}

static bool IsMixerTitle(HWND hwnd)
{
    char title[256] = {};
    GetWindowText(hwnd, title, sizeof(title));
    return strstr(title, "Mixer") || strstr(title, "mixer");
}

struct McpSearch
{
    std::vector<HWND> displays;
    HWND mixerHost = nullptr; // fallback: docked / floating host titled "Mixer"
};

// Own recursion: EnumChildWindows is recursive on Windows but not everywhere in SWELL
static void CollectMcpWindows(HWND hwnd, McpSearch& search)
{
    if (IsMcpClass(hwnd))
    {
        if (std::find(search.displays.begin(), search.displays.end(), hwnd) == search.displays.end())
            search.displays.push_back(hwnd);
    }
    else if (!search.mixerHost && IsMixerTitle(hwnd))
    {
        search.mixerHost = hwnd;
    }
    for (HWND child = GetWindow(hwnd, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
        CollectMcpWindows(child, search);
}

static BOOL CALLBACK CollectMcpTopLevel(HWND hwnd, LPARAM lParam)
{
    CollectMcpWindows(hwnd, *(McpSearch*)lParam);
    return TRUE;
}

static std::vector<HWND> FindMcpWindows()
{
    McpSearch search;
    HWND main = GetMainHwnd ? GetMainHwnd() : nullptr;
    if (main)
        CollectMcpWindows(main, search);
    EnumWindows(CollectMcpTopLevel, (LPARAM)&search); // floating mixer / dockers
    if (search.displays.empty() && search.mixerHost)
        search.displays.push_back(search.mixerHost);
    return search.displays;
}

static void LogAttach(const char* what, HWND hwnd, bool ok)
{
    char cls[256] = {}, parentCls[256] = {};
    GetClassName(hwnd, cls, sizeof(cls));
    if (HWND parent = GetParent(hwnd))
        GetClassName(parent, parentCls, sizeof(parentCls));
    DebugLog("[Drop To Sampler] %s drop target %s (%s in %s)\n", what, ok ? "registered" : "failed",
        cls, parentCls[0] ? parentCls : "top level");
}

// (Re)attaches a drop target when the window appears or is recreated
// TCP/MCP: no Alt -> sampler; Alt -> native via parent OLE (REAPER main).
// Child HWND often has no OLE of its own; AttachClaim registers on the child and
// forwards bypass drops to the parent drop target (same idea that made Alt work
// on tracks before, including empty TCP area).
static bool TcpMcpIntercept(const Modifiers& mods, int screenX, int screenY)
{
    (void)screenX;
    (void)screenY;
    return WantSamplerForMode(g_settings.tcpDropMode, IsModifierDown(mods, g_settings.modSurface));
}

static void UpdateTcpWindow()
{
    if (g_settings.tcpDropMode == SurfaceDrop_Disabled)
    {
        if (g_tcp.hwnd || g_tcp.target)
        {
            g_tcp = AttachedWindow();
            DebugLog("[Drop To Sampler] TCP drop target detached\n");
        }
        return;
    }

    if (g_tcp.target && g_tcp.hwnd && IsWindow(g_tcp.hwnd))
        return;

    if (!g_tcp.hwnd || !IsWindow(g_tcp.hwnd))
    {
        g_tcp.target.reset();
        g_tcp.hwnd = FindTcpWindow();
    }
    if (!g_tcp.hwnd) return;

    if (!g_tcp.target)
    {
        g_tcp.target = DropTarget::AttachClaim(g_tcp.hwnd, g_dragHandler, TcpMcpIntercept);
        LogAttach("TCP", g_tcp.hwnd, g_tcp.target != nullptr);
        if (!g_tcp.target)
            DebugLog("[Drop To Sampler] TCP: parent OLE not found yet; will retry\n");
    }
}


static void UpdateMcpWindows()
{
    if (g_settings.tcpDropMode == SurfaceDrop_Disabled)
    {
        if (!g_mcps.empty())
        {
            g_mcps.clear();
            DebugLog("[Drop To Sampler] MCP drop targets detached\n");
        }
        return;
    }

    std::vector<HWND> found = FindMcpWindows();
    found.erase(std::remove(found.begin(), found.end(), g_tcp.hwnd), found.end());

    g_mcps.erase(std::remove_if(g_mcps.begin(), g_mcps.end(), [&](const AttachedWindow& w) {
        return !IsWindow(w.hwnd) || std::find(found.begin(), found.end(), w.hwnd) == found.end();
    }), g_mcps.end());

    for (HWND hwnd : found)
    {
        bool attached = std::any_of(g_mcps.begin(), g_mcps.end(),
            [&](const AttachedWindow& w) { return w.hwnd == hwnd; });
        if (attached) continue;
        AttachedWindow w;
        w.hwnd = hwnd;
        w.target = DropTarget::AttachClaim(hwnd, g_dragHandler, TcpMcpIntercept);
        LogAttach("MCP", hwnd, w.target != nullptr);
        if (w.target)
            g_mcps.push_back(std::move(w));
    }
}


// Alt held -> sampler; otherwise leave the drop to REAPER (native item insert).
static bool ArrangeIntercept(const Modifiers& mods, int screenX, int screenY)
{
    char info[256] = {};
    if (GetThingFromPoint)
        GetThingFromPoint(screenX, screenY, info, sizeof(info));

    if (strncmp(info, "tcp", 3) == 0 || strncmp(info, "mcp", 3) == 0)
        return false;
    if (info[0] && strncmp(info, "arrange", 7) != 0)
        return false;

    return WantSamplerForMode(g_settings.arrangeDropMode, IsModifierDown(mods, g_settings.modSurface));
}

// When Enabled: OLE passthrough on arrange (Alt = our handler). Disabled: detach.
static void UpdateArrangeWindow()
{
    if (g_settings.arrangeDropMode == SurfaceDrop_Disabled)
    {
        if (g_arrange.hwnd || g_arrange.target)
        {
            g_arrange = AttachedWindow();
            DebugLog("[Drop To Sampler] Arrange drop target detached\n");
        }
        return;
    }

    if (g_arrange.hwnd && IsWindow(g_arrange.hwnd) && g_arrange.target)
        return;

    g_arrange.target.reset();
    g_arrange.hwnd = FindArrangeWindow();
    if (!g_arrange.hwnd) return;
    // allowParentWalk=true: OLE for arrange is often on a parent of control 1000
    g_arrange.target = DropTarget::AttachPassthrough(
        g_arrange.hwnd, g_dragHandler, ArrangeIntercept, true);
    // Avoid spamming the console every timer tick while OLE is not ready
    static bool s_loggedArrangeFail = false;
    if (g_arrange.target)
    {
        LogAttach("Arrange", g_arrange.hwnd, true);
        s_loggedArrangeFail = false;
    }
    else if (!s_loggedArrangeFail)
    {
        LogAttach("Arrange", g_arrange.hwnd, false);
        s_loggedArrangeFail = true;
    }
}


static void OnTimer()
{
    if (++g_timerTicks < 20) return;
    g_timerTicks = 0;

    UpdateTcpWindow();
    UpdateMcpWindows();
    UpdateArrangeWindow();
}

// ---------------------------------------------------------------------------
// Action / menu
// ---------------------------------------------------------------------------
static bool HookCommand2(KbdSectionInfo* sec, int command, int val, int valhw, int relmode, HWND hwnd)
{
    (void)sec; (void)val; (void)valhw; (void)relmode; (void)hwnd;
    if (g_cmdSettings != 0 && command == g_cmdSettings)
    {
        OpenSettingsPage();
        return true;
    }
    return false;
}

static void HookCustomMenu(const char* menuidstr, void* menuPtr, int flag)
{
    if (flag != 0 || !menuidstr || !menuPtr || g_cmdSettings <= 0)
        return;
    if (strcmp(menuidstr, "Main extensions") != 0)
        return;

    HMENU menu = (HMENU)menuPtr;
    MENUITEMINFO mi = {};
    mi.cbSize = sizeof(mi);
    mi.fMask = MIIM_TYPE | MIIM_ID;
    mi.fType = MFT_STRING;
    mi.wID = (UINT)g_cmdSettings;
    mi.dwTypeData = (char*)"Drop To Sampler";
    InsertMenuItem(menu, GetMenuItemCount(menu), TRUE, &mi);
}

// ---------------------------------------------------------------------------
// Entry
// ---------------------------------------------------------------------------
static void Cleanup()
{
    UnregisterSettingsPage();
    g_tcp = AttachedWindow();
    g_mcps.clear();
    g_arrange = AttachedWindow();
    if (g_dragHandler)
    {
        g_dragHandler->OnDragLeave();
        delete g_dragHandler;
        g_dragHandler = nullptr;
    }
}

extern "C" REAPER_PLUGIN_DLL_EXPORT int ReaperPluginEntry(
    REAPER_PLUGIN_HINSTANCE hInstance, reaper_plugin_info_t* rec)
{
    if (!rec)
    {
        Cleanup();
        return 0;
    }

    if (rec->caller_version != REAPER_PLUGIN_VERSION)
        return 0;

    REAPERAPI_LoadAPI(rec->GetFunc);

    if (!GetMainHwnd)
        return 0;

    LoadSettings();

    g_dragHandler = new SamplerDragHandler();
    g_dragHandler->m_overlay = DragOverlay::Create();

    custom_action_register_t action = {};
    action.uniqueSectionId = 0;
    action.idStr = "DROP_TO_SAMPLER_SETTINGS";
    action.name = "Drop To Sampler";
    action.extra = nullptr;
    g_cmdSettings = (int)(intptr_t)rec->Register("custom_action", &action);

    rec->Register("hookcommand2", (void*)HookCommand2);

    if (AddExtensionsMainMenu)
        AddExtensionsMainMenu();
    rec->Register("hookcustommenu", (void*)HookCustomMenu);

    RegisterSettingsPage(rec, hInstance);

    rec->Register("timer", (void*)OnTimer);
    return 1;
}

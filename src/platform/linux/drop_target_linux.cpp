// Linux: SWELL (GDK) turns an XDND file drop into WM_DROPFILES, sent to the deepest
// child window under the cursor. Attach() subclasses the TCP/MCP window and takes
// WM_DROPFILES before REAPER's window procedure sees it.
//
// Drag hover is not reported per window: SWELL calls the process-wide
// SWELL_DDrop_onDragEnter/Over/Leave hooks, which REAPER sets for its own feedback.
// SWELL_ExtendedAPI can set them but not read them back, so the hooks are found in
// libSwell's data by setting ours and looking for the word that changed. The old
// value is REAPER's handler: ours call it first, then report hover over TCP/MCP.
// File count is unknown while hovering (SWELL passes at most one path).

#include "platform/platform.h"
#include "settings.h"

#include <dlfcn.h>
#include <link.h>

#include <cstdint>
#include <cstring>
#include <map>

// ---------------------------------------------------------------------------
// Subclassed TCP/MCP windows
// ---------------------------------------------------------------------------

// Per subclassed window. Outlives the DropTarget when someone else subclassed the
// window after us: we cannot unhook then, so the hook stays as a pass-through.
struct Hook
{
    WNDPROC prevProc = nullptr;
    DragHandler* handler = nullptr;
};
static std::map<HWND, Hook> g_hooks;

static Modifiers CurrentModifiers()
{
    Modifiers m;
    m.shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    m.ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    m.alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    return m;
}

static std::vector<std::string> DroppedFiles(HDROP hDrop)
{
    std::vector<std::string> files;
    UINT count = DragQueryFile(hDrop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < count; ++i)
    {
        char path[4096] = {};
        if (DragQueryFile(hDrop, i, path, sizeof(path)) && path[0])
            files.push_back(path);
    }
    return files;
}

// false = not ours, let REAPER handle it
static bool HandleDropFiles(HWND hwnd, HDROP hDrop, DragHandler* handler)
{
    std::vector<std::string> files = DroppedFiles(hDrop);
    if (files.empty()) return false;

    POINT pt = {};
    DragQueryPoint(hDrop, &pt);
    ClientToScreen(hwnd, &pt);
    handler->OnDrop(files, pt.x, pt.y, CurrentModifiers());
    return true;
}

// ---------------------------------------------------------------------------
// Drag hover: SWELL_DDrop_* hooks, chained to REAPER's
// ---------------------------------------------------------------------------
typedef void (*DragEnterProc)(void* hGlobal, POINT pt);
typedef void (*DragOverProc)(POINT pt);
typedef void (*DragLeaveProc)();

struct SwellHook
{
    const char* key;
    void* ours;
    void** slot = nullptr; // the libSwell variable
    void* prev = nullptr;  // REAPER's handler
};

static void HookDragEnter(void* hGlobal, POINT pt);
static void HookDragOver(POINT pt);
static void HookDragLeave();

static SwellHook g_swellHooks[] = {
    { "SWELL_DDrop_onDragEnter", (void*)HookDragEnter },
    { "SWELL_DDrop_onDragOver", (void*)HookDragOver },
    { "SWELL_DDrop_onDragLeave", (void*)HookDragLeave },
};
static bool g_swellHooksInstalled = false;

static HWND g_hoverHwnd = nullptr; // attached window the drag is over
static int g_hoverFileCount = 0;

// Attached window that contains the screen point, if any
static HWND AttachedWindowAt(POINT pt)
{
    for (HWND h = WindowFromPoint(pt); h; h = GetParent(h))
    {
        auto it = g_hooks.find(h);
        if (it != g_hooks.end() && it->second.handler)
            return h;
    }
    return nullptr;
}

static DragHandler* HandlerOf(HWND hwnd)
{
    auto it = g_hooks.find(hwnd);
    return it != g_hooks.end() ? it->second.handler : nullptr;
}

static void HoverLeave()
{
    if (DragHandler* handler = HandlerOf(g_hoverHwnd))
        handler->OnDragLeave();
    g_hoverHwnd = nullptr;
}

static void HoverAt(POINT pt)
{
    HWND hwnd = AttachedWindowAt(pt);
    if (hwnd != g_hoverHwnd)
    {
        HoverLeave();
        if (DragHandler* handler = HandlerOf(hwnd))
        {
            g_hoverHwnd = hwnd;
            handler->OnDragEnter(g_hoverFileCount, pt.x, pt.y, CurrentModifiers());
        }
        return;
    }
    if (DragHandler* handler = HandlerOf(hwnd))
        handler->OnDragOver(pt.x, pt.y, CurrentModifiers());
}

static void HookDragEnter(void* hGlobal, POINT pt)
{
    if (g_swellHooks[0].prev) ((DragEnterProc)g_swellHooks[0].prev)(hGlobal, pt);

    // An external drag has no path yet: SWELL passes a placeholder under /tmp
    g_hoverFileCount = 0;
    char path[4096] = {};
    if (hGlobal && DragQueryFile((HDROP)hGlobal, 0, path, sizeof(path)) && !strstr(path, "<<<<<"))
        g_hoverFileCount = (int)DragQueryFile((HDROP)hGlobal, 0xFFFFFFFF, nullptr, 0);
    HoverAt(pt);
}

static void HookDragOver(POINT pt)
{
    if (g_swellHooks[1].prev) ((DragOverProc)g_swellHooks[1].prev)(pt);
    HoverAt(pt);
}

static void HookDragLeave()
{
    if (g_swellHooks[2].prev) ((DragLeaveProc)g_swellHooks[2].prev)();
    HoverLeave();
}

struct Range { uintptr_t begin, end; };

struct FindRangesCtx
{
    uintptr_t addr;
    std::vector<Range> writable;
};

// Writable segments of the shared object that contains ctx->addr
static int FindWritableSegments(dl_phdr_info* info, size_t, void* data)
{
    FindRangesCtx* ctx = (FindRangesCtx*)data;
    bool contains = false;
    for (int i = 0; i < info->dlpi_phnum; ++i)
    {
        const ElfW(Phdr)& ph = info->dlpi_phdr[i];
        uintptr_t begin = info->dlpi_addr + ph.p_vaddr;
        if (ph.p_type == PT_LOAD && ctx->addr >= begin && ctx->addr < begin + ph.p_memsz)
            contains = true;
    }
    if (!contains) return 0;
    for (int i = 0; i < info->dlpi_phnum; ++i)
    {
        const ElfW(Phdr)& ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_LOAD || !(ph.p_flags & PF_W)) continue;
        uintptr_t begin = (info->dlpi_addr + ph.p_vaddr + sizeof(void*) - 1) & ~(uintptr_t)(sizeof(void*) - 1);
        ctx->writable.push_back({ begin, info->dlpi_addr + ph.p_vaddr + ph.p_memsz });
    }
    return 1;
}

// Sets one hook and finds where libSwell keeps it. False if the variable could not
// be identified; the hook is then left set, as there is no way to put REAPER's back.
static bool InstallSwellHook(SwellHook& hook, const std::vector<Range>& ranges)
{
    std::vector<std::vector<void*>> before;
    for (const Range& r : ranges)
        before.emplace_back((void**)r.begin, (void**)r.begin + (r.end - r.begin) / sizeof(void*));

    SWELL_ExtendedAPI(hook.key, hook.ours);

    int found = 0;
    for (size_t i = 0; i < ranges.size(); ++i)
    {
        void** words = (void**)ranges[i].begin;
        for (size_t w = 0; w < before[i].size(); ++w)
        {
            if (words[w] == hook.ours && before[i][w] != hook.ours)
            {
                hook.slot = &words[w];
                hook.prev = before[i][w];
                ++found;
            }
        }
    }
    if (found == 1) return true;
    hook.slot = nullptr;
    hook.prev = nullptr;
    return false;
}

static void InstallSwellHooks()
{
    if (g_swellHooksInstalled || !SWELL_ExtendedAPI) return;
    g_swellHooksInstalled = true;

    FindRangesCtx ctx;
    ctx.addr = (uintptr_t)(void*)SWELL_ExtendedAPI;
    dl_iterate_phdr(FindWritableSegments, &ctx);
    if (ctx.writable.empty())
    {
        DebugLog("[Drop To Sampler] libSwell not found, no drag hover feedback\n");
        return;
    }
    for (SwellHook& hook : g_swellHooks)
    {
        bool ok = InstallSwellHook(hook, ctx.writable);
        DebugLog("[Drop To Sampler] %s: %s\n", hook.key, ok ? "chained" : "not found, REAPER's handler is lost");
    }
}

static void RemoveSwellHooks()
{
    if (!g_swellHooksInstalled) return;
    g_swellHooksInstalled = false;
    HoverLeave();
    for (SwellHook& hook : g_swellHooks)
    {
        // Put REAPER's back unless someone hooked over us
        if (hook.slot && *hook.slot == hook.ours)
            SWELL_ExtendedAPI(hook.key, hook.prev);
        hook.slot = nullptr;
        hook.prev = nullptr;
    }
}

// ---------------------------------------------------------------------------
// DropTarget
// ---------------------------------------------------------------------------
static int g_activeTargets = 0;

static LRESULT WINAPI HookWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    auto it = g_hooks.find(hwnd);
    if (it == g_hooks.end()) return DefWindowProc(hwnd, msg, wParam, lParam);
    Hook hook = it->second; // the handler may detach while handling

    if (msg == WM_DROPFILES && hook.handler && HandleDropFiles(hwnd, (HDROP)wParam, hook.handler))
        return 0;
    if (msg == WM_DESTROY)
    {
        if (hwnd == g_hoverHwnd) HoverLeave();
        g_hooks.erase(hwnd);
    }
    return CallWindowProc(hook.prevProc, hwnd, msg, wParam, lParam);
}

class SwellDropTarget : public DropTarget
{
public:
    explicit SwellDropTarget(HWND hwnd) : m_hwnd(hwnd)
    {
        if (g_activeTargets++ == 0)
            InstallSwellHooks();
    }

    ~SwellDropTarget() override
    {
        if (m_hwnd == g_hoverHwnd) HoverLeave();
        auto it = g_hooks.find(m_hwnd);
        if (it != g_hooks.end()) // else the window was destroyed
        {
            if (IsWindow(m_hwnd) && (WNDPROC)GetWindowLong(m_hwnd, GWL_WNDPROC) == HookWndProc)
            {
                SetWindowLong(m_hwnd, GWL_WNDPROC, (LONG_PTR)it->second.prevProc);
                g_hooks.erase(it);
            }
            else
            {
                it->second.handler = nullptr;
            }
        }
        if (--g_activeTargets == 0)
            RemoveSwellHooks();
    }

private:
    HWND m_hwnd;
};

std::unique_ptr<DropTarget> DropTarget::Attach(HWND hwnd, DragHandler* handler)
{
    if (!hwnd || !handler) return nullptr;
    auto it = g_hooks.find(hwnd);
    if (it != g_hooks.end())
    {
        if (it->second.handler) return nullptr; // already attached
        it->second.handler = handler; // reuse a pass-through hook
    }
    else
    {
        Hook& hook = g_hooks[hwnd];
        hook.handler = handler;
        hook.prevProc = (WNDPROC)SetWindowLong(hwnd, GWL_WNDPROC, (LONG_PTR)HookWndProc);
    }
    return std::unique_ptr<DropTarget>(new SwellDropTarget(hwnd));
}

std::unique_ptr<DropTarget> DropTarget::AttachPassthrough(HWND hwnd, DragHandler* handler, InterceptFn shouldIntercept, bool allowParentWalk)
{
    // TODO: native passthrough for arrange. Not implemented yet.
    (void)hwnd; (void)handler; (void)shouldIntercept; (void)oleFallback; (void)allowParentWalk;
    return nullptr;
}

std::unique_ptr<DropTarget> DropTarget::AttachClaim(HWND hwnd, DragHandler* handler, InterceptFn shouldIntercept, HWND oleFallback)
{
    (void)hwnd; (void)handler; (void)shouldIntercept; (void)oleFallback;
    return nullptr;
}

namespace platform {
void ReleaseSavedReaperNative() {}
}

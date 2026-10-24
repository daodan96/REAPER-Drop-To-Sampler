#pragma once

#include "reaper_plugin.h"

#include <memory>
#include <string>
#include <vector>

#ifdef __APPLE__
#define DTS_KEY_CTRL "Cmd"
#define DTS_KEY_ALT "Option"
#else
#define DTS_KEY_CTRL "Ctrl"
#define DTS_KEY_ALT "Alt"
#endif
#define DTS_KEY_SHIFT "Shift"

struct Modifiers
{
    bool shift = false;
    bool ctrl = false;
    bool alt = false;
};

class DragHandler
{
public:
    virtual ~DragHandler() = default;
    virtual void OnDragEnter(int fileCount, int x, int y, const Modifiers& mods) = 0;
    virtual void OnDragOver(int x, int y, const Modifiers& mods) = 0;
    virtual void OnDragLeave() = 0;
    virtual void OnDrop(const std::vector<std::string>& files, int x, int y, const Modifiers& mods) = 0;
};

class DropTarget
{
public:
    virtual ~DropTarget() = default;

    // Full replace (no native forward). Prefer AttachClaim / AttachPassthrough when Alt matters.
    static std::unique_ptr<DropTarget> Attach(HWND hwnd, DragHandler* handler);

    // Wrap existing OLE on hwnd (or parents if allowParentWalk). Used for arrange.
    using InterceptFn = bool (*)(const Modifiers& mods, int screenX, int screenY);
    static std::unique_ptr<DropTarget> AttachPassthrough(
        HWND hwnd, DragHandler* handler, InterceptFn shouldIntercept, bool allowParentWalk = false);

    // Register on hwnd even if it has no OLE (TCP/MCP). Forward non-intercepted
    // drops to the nearest parent OLE target (REAPER's real handler).
    static std::unique_ptr<DropTarget> AttachClaim(
        HWND hwnd, DragHandler* handler, InterceptFn shouldIntercept);
};

enum DropVisualKind { DropVisual_Insert = 0, DropVisual_Replace = 1 };

class DragOverlay
{
public:
    virtual ~DragOverlay() = default;
    virtual void ShowAtCursor(DropVisualKind kind) = 0;
    virtual void Hide() = 0;
    static std::unique_ptr<DragOverlay> Create();
};

namespace platform {

std::string CartridgeDataDir();
bool FileExists(const char* path);
bool WriteTextFile(const char* path, const char* text);

} // namespace platform

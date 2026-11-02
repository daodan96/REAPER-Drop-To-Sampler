#pragma once

// What happens when media is dragged over / dropped on TCP or MCP.
// Platform-independent: DropTarget implementations feed it via DragHandler.

#include "platform/platform.h"

#include <string>
#include <vector>

// Shows a preview of the drop result in REAPER's status line;
// returns which overlay icon to show.
DropVisualKind UpdateDropHelp(const Modifiers& mods, int screenX, int screenY, int fileCount);
void ClearDropHelp();

// True for files REAPER treats as media (by extension)
bool PathIsMediaFile(const char* path);

// Inserts samplers for the dropped files (UTF-8 paths)
void ProcessDrop(const std::vector<std::string>& files, int screenX, int screenY, const Modifiers& mods);

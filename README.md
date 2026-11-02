# Drop To Sampler

**English** · [Русский](README.ru.md)

An extension for [REAPER](https://www.reaper.fm/): dropping media onto the **TCP / MCP** inserts a sampler (RS5k or [Cartridge](https://dsgdnb.com/plugins/cartridge)) with the sample loaded, instead of inserting an item on the track.

Windows (x64), macOS (Apple Silicon and Intel), Linux (x86_64 and aarch64).

## Install

1. **Extensions → ReaPack → Import repositories…**

   ```
   https://raw.githubusercontent.com/daodan96/REAPER-Drop-To-Sampler/main/index.xml
   ```

2. **Extensions → ReaPack → Browse packages…** → **Drop To Sampler** → install, restart REAPER.

If you installed the plugin by hand before, delete the old `reaper_drop_to_sampler.dll` from `UserPlugins`, otherwise REAPER loads both copies.

## Usage

Drag files from the Media Explorer (or Explorer / Finder / a file manager) onto the track list or mixer to insert a sampler. In the Arrange view, hold the configured modifier (Alt by default) to insert a sampler; otherwise, REAPER handles the drop normally. To replace a sample, drop onto a sampler FX slot when **Replace on sampler FX slot** is selected in Settings.

The status line beneath the TCP shows the current mode while dragging.[^linux-file-count]

Default modifiers (can be changed in Settings):
* **Shift** — one track / multiple tracks
* **Ctrl** (**Cmd** on macOS) — Layers / Chromatic
* **Alt** (**Option** on macOS) — bypass sampler handling on TCP/MCP; enable it in Arrange

**Settings:** *Extensions → Drop To Sampler* or *Preferences → Plug-ins → Drop To Sampler*.

`sampler_insert.png` / `sampler_replace.png` in `UserPlugins` override the embedded icons. The active icon appears near the cursor while the extension handles the drag.

[^linux-file-count]: On Linux, the file count is unknown until the drop, so the status line describes a single file while dragging, even when multiple files are selected.

## Discussion

[Forum thread on Cockos](https://forum.cockos.com/showthread.php?p=2968712).

## Build

**Windows:** MSYS2 UCRT64 (`mingw-w64-ucrt-x86_64-gcc`, `cmake`, `ninja`) or Visual Studio 2022+ with "Desktop development with C++".

```bat
build.bat
```

MinGW is picked if found, otherwise Visual Studio. Custom paths: copy `build.config.example.bat` to `build.config.bat`.

**macOS / Linux:** CMake, Perl and a C++17 compiler (Xcode command line tools on macOS).

```sh
./build.sh
```

`build.bat deploy` / `./build.sh deploy` also copies the plugin to `UserPlugins` (and restarts REAPER on Windows).

## Release

```sh
git tag v0.1.0
git push origin v0.1.0
```

The tag must point to a commit in `main`. GitHub Actions builds all platforms and creates a **draft** release. Write the description (it becomes the ReaPack changelog; English, not empty) and publish. `index.xml` is updated automatically, so `git pull` afterwards.

To drop a platform, delete its file from the draft. To rebuild the draft, move the tag: `git tag -f v0.1.0 && git push -f origin v0.1.0`. Changelog fixes: edit the release description.

## How it works

* File drops on the TCP/MCP windows are intercepted, so REAPER's native item insert does not happen; the FX is added via `TrackFX_AddByName`.
* RS5k: `FILE0` + `DONE`; Cartridge: `pending_load.txt` + the Load Trigger parameter.
* The drop target (track / FX slot) is resolved with `GetThingFromPoint`.
* Feedback: `Help_Set` + a PNG overlay next to the cursor.
* Settings are stored in REAPER ExtState.

Code layout (`src/`):

* `drop`, `sampler`, `tracks`, `settings*`, `prompts` — plugin logic; uses only the REAPER API and the Win32 subset REAPER provides on macOS and Linux through SWELL.
* `platform/platform.h` — interfaces for everything OS-specific: `DropTarget` (takes over drops on a window), `DragOverlay` (cursor icon), file paths.
* `platform/win/` — OLE `IDropTarget`, GDI+ layered window.
* `platform/mac/` — the SWELL view's `NSDraggingDestination` methods overridden in a runtime subclass, `NSPanel` overlay.
* `platform/linux/` — the TCP/MCP window procedure subclassed to take `WM_DROPFILES` (SWELL turns XDND drops into it). Hover comes from SWELL's process-wide `SWELL_DDrop_*` hooks, chained in front of REAPER's own. Overlay: an ARGB GDK window drawn with cairo, both looked up at runtime from REAPER's process.
* `platform/posix/` — file helpers shared by macOS and Linux.
* The Preferences page is a dialog in `resource.rc`; on macOS and Linux it is converted with `swell_resgen.pl` from the [WDL](https://github.com/justinfrankel/WDL) submodule at build time.

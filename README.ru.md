# Drop To Sampler

[English](README.md) · **Русский**

Расширение для [REAPER](https://www.reaper.fm/): если перетащить медиафайл на **TCP / MCP**, вместо айтема на треке появится сэмплер (RS5k или [Cartridge](https://dsgdnb.com/plugins/cartridge)) с уже загруженным сэмплом.

Windows (x64), macOS (Apple Silicon и Intel), Linux (x86_64 и aarch64).

## Установка

1. **Extensions → ReaPack → Import repositories…**

   ```
   https://raw.githubusercontent.com/daodan96/REAPER-Drop-To-Sampler/main/index.xml
   ```

2. **Extensions → ReaPack → Browse packages…** → **Drop To Sampler** → установить, перезапустить REAPER.

Если раньше плагин ставили вручную, удалите старый `reaper_drop_to_sampler.dll` из `UserPlugins`, иначе REAPER загрузит обе копии.

## Использование

Перетащите медиафайлы из Media Explorer (или Проводника / Finder / файлового менеджера) на список треков или в микшер, чтобы вставить сэмплер. В Arrange удерживайте назначенную клавишу-модификатор (по умолчанию Alt), чтобы вставить сэмплер; иначе REAPER обработает дроп обычным образом. Чтобы заменить сэмпл, перетащите файл на FX-слот сэмплера, если в настройках выбрано **Replace on sampler FX slot**.

Во время перетаскивания строка состояния под TCP показывает текущий режим.[^linux-file-count]

Модификаторы по умолчанию (можно изменить в настройках):
* **Shift** — переключение между одним треком и несколькими
* **Ctrl** (**Cmd** на macOS) — переключение между Layers и Chromatic
* **Alt** (**Option** на macOS) — отключает обработку дропа плагином на TCP/MCP; включает её в Arrange

**Настройки:** *Extensions → Drop To Sampler* или *Preferences → Plug-ins → Drop To Sampler*.

Файлы `sampler_insert.png` и `sampler_replace.png` в `UserPlugins` заменяют встроенные иконки. Активная иконка отображается у курсора, когда расширение обрабатывает перетаскивание.

[^linux-file-count]: В Linux количество файлов неизвестно до дропа, поэтому строка состояния описывает один файл, даже если перетаскивается несколько.

## Обсуждение

[Ветка на форуме Cockos](https://forum.cockos.com/showthread.php?p=2968712).

## Сборка

**Windows:** MSYS2 UCRT64 (`mingw-w64-ucrt-x86_64-gcc`, `cmake`, `ninja`) или Visual Studio 2022+ с компонентом «Разработка классических приложений на C++».

```bat
build.bat
```

Если найден MinGW, используется он, иначе Visual Studio. Свои пути: скопируйте `build.config.example.bat` в `build.config.bat`.

**macOS / Linux:** CMake, Perl и компилятор C++17 (на macOS — Xcode command line tools).

```sh
./build.sh
```

`build.bat deploy` / `./build.sh deploy` дополнительно копирует плагин в `UserPlugins` (на Windows ещё и перезапускает REAPER).

## Релиз

```sh
git tag v0.1.0
git push origin v0.1.0
```

Тег должен указывать на коммит из `main`. GitHub Actions соберёт все платформы и создаст **черновик** релиза. Напишите описание (оно станет changelog в ReaPack; на английском, не пустое) и опубликуйте. `index.xml` обновится автоматически, после этого сделайте `git pull`.

Чтобы не выпускать платформу, удалите её файл из черновика. Чтобы пересобрать черновик, перенесите тег: `git tag -f v0.1.0 && git push -f origin v0.1.0`. Исправить changelog — отредактировать описание релиза.

## Как это работает

* Дроп файлов на окна TCP/MCP перехватывается, поэтому стандартная вставка айтема не происходит; FX добавляется через `TrackFX_AddByName`.
* RS5k: `FILE0` + `DONE`; Cartridge: `pending_load.txt` + параметр Load Trigger.
* Цель дропа (трек / слот FX) определяется через `GetThingFromPoint`.
* Индикация: `Help_Set` + PNG-иконка у курсора.
* Настройки хранятся в ExtState REAPER.

Структура кода (`src/`):

* `drop`, `sampler`, `tracks`, `settings*`, `prompts` — логика плагина; использует только REAPER API и подмножество Win32, которое REAPER на macOS и Linux предоставляет через SWELL.
* `platform/platform.h` — интерфейсы всего платформенного: `DropTarget` (перехват дропа на окне), `DragOverlay` (иконка у курсора), пути к файлам.
* `platform/win/` — OLE `IDropTarget`, layered-окно на GDI+.
* `platform/mac/` — методы `NSDraggingDestination` вида SWELL, переопределённые в подклассе, созданном в рантайме; оверлей на `NSPanel`.
* `platform/linux/` — подмена оконной процедуры TCP/MCP, чтобы перехватывать `WM_DROPFILES` (в него SWELL превращает XDND-дропы). Наведение — через глобальные хуки SWELL `SWELL_DDrop_*`, встроенные перед хуками REAPER. Оверлей — ARGB-окно GDK, рисуется через cairo; обе библиотеки берутся из процесса REAPER во время работы.
* `platform/posix/` — работа с файлами, общая для macOS и Linux.
* Страница настроек — диалог в `resource.rc`; на macOS и Linux он конвертируется `swell_resgen.pl` из сабмодуля [WDL](https://github.com/justinfrankel/WDL) при сборке.

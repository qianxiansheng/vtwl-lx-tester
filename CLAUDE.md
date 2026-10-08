# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

`lxTester` is a Windows production-test host app (产测上位机) written in C++20 with Dear ImGui (GLFW + OpenGL3 backend) and the wjwwood `serial` library. `readme.md` is the **requirements spec** for the app being built: up to 6 test stations (1 COM/WinUSB port = 1 station = 1 test board + 1 DUT), 3 fixed tests (RF packet rx/tx, frequency offset, power consumption), a strict global state machine, and fixed status colors. Read it before implementing UI or business logic. Its "boundary constraints" and "do not implement" lists are binding.

The codebase started as a copy of a sibling project (`../LxRecorder`, a voice-recording tool). Much of the existing code (speaker info, word lists, recording to `.bin`, background music) belongs to that recorder and is reference material, not part of the tester.

## Build

There is no CMake or `.sln` in the repo. Build the MSBuild project `lxTester.vcxproj` (toolset v145, Visual Studio). Note that `.gitignore` excludes `*.vcxproj*` and `*.sln`, so the project file exists only locally:

```
msbuild lxTester.vcxproj /p:Configuration=Debug /p:Platform=x64
```

- Output goes to `$(SolutionDir)..\bin\<Platform>\<Config>\` and intermediates to `..\tmp\...`. When you build the vcxproj directly, that means `../bin` and `../tmp`, outside the repo.
- Prebuilt libs are checked in locally under `thirdparty/glfw/lib/<Platform>` and `thirdparty/wjwwood/lib/<Config>/<Platform>` (`*.lib` is gitignored).
- Include dirs: `src`, `src/imgui`, `src/imgui/backends`, `src/imgui/extensions`, `thirdparty/glfw/include`, `thirdparty/wjwwood/include`, `thirdparty/nlohmann/include` (nlohmann/json v3.11.3, a single header).
- **Which files get compiled is controlled by the vcxproj, not by what's in `src/`.** Right now it compiles `main.cpp`, `ui_main.cpp`, `tester/*.cpp`, `utils/*`, and the imgui sources. `Domain.cpp`, `Model.cpp`, and `main_lxrecorder.cpp` are **not** in the project. If you add new .cpp files, add them to `lxTester.vcxproj` (and `.filters`).
- The project compiles with `/utf-8`. All sources are UTF-8 without a BOM.
- `main.cpp` and `main_lxrecorder.cpp` both define `main_init`/`main_gui`, so only one of them can be compiled at a time.
- There are no tests and no lint setup.

The window starts at 1280×880 scaled by monitor DPI (capped to the work area) and can be resized freely. Inside it is an ImGui dockspace. The dock layout is saved to `imgui.ini`, and user settings to `config.json`; both live in the working directory. It also loads the font `C:/Windows/Fonts/msyh.ttc`, which is needed for Chinese text.

## Architecture

**UI shell (`ui_main.cpp` / `ui_main.h`)**: owns `WinMain`/`main`, the GLFW window, the OpenGL3 context, and the ImGui frame loop. Each frame it drains the `PostToUI` task queue, then calls the app hooks `main_init(argc, argv)` (once) and `main_gui()` (every frame; a non-zero return closes the window). `UiDpiContext` (singleton) handles DPI scaling and font rebuilding. `CreateTexture`/`DeleteTexture` are the GL texture adapters used by ImFileDialog. `APP_TITLE` and helper macros are in `pch.h`.

**App layer (`main.cpp`)**: implements `main_init`/`main_gui`/`main_shutdown`. It wires up the tester MVVM objects and creates the root window with a dockspace and a menu bar. The 视图 (View) menu toggles panels and resets the layout, and the 调试 (Debug) menu injects demo data and alerts from the business thread. `BuildDefaultDockLayout` builds the default layout when `imgui.ini` has no `TesterDockSpace` node. The panel window names in `TesterView.h` (`kXxxWindow`) are the dock IDs, so renaming one invalidates saved layouts.

**Tester (`src/tester/`, namespace `Lx::Tester`)**: MVVM split. ImGui has no data binding, so dirty flags stand in for it.
- `TesterTypes.h`: the domain enums and structs (global, DUT, station and test-item states, `TestConfig`, `AlertEvent`). No ImGui here.
- `TesterModel`: a thread-safe store and the single source of truth. Every setter ORs a `Dirty_*` bit. Logs carry a monotonically increasing `seq` so the UI can fetch them incrementally. Alerts sit in a queue that is drained with `takeAlerts()`.
- `TesterService` (implements `ITesterService`): owns the business thread. Every command is posted to that thread and returns immediately, and the thread is the only writer of the model. Right now it only does data management (add/remove stations, pick ports, config, logs). Connect, test, and scheduling are `TODO` stubs.
- `TesterViewModel`: used on the UI thread only, with no locks. `sync()` runs once per frame at the start of `main_gui`. It consumes the dirty flags and re-snapshots only the parts that changed (formatted strings, an alert deque). It also exposes `canXxx()` enable rules and forwards commands to the service.
- `TesterView`: plain ImGui drawing that touches only the ViewModel. There is one dockable window per panel: 控制台 (console), 工位管理 (stations), 测试参数 (parameters), 日志 (log). Log selection state (a range of seqs) lives in the VM, and copying text goes through `ImGui::SetClipboardText`.
- `TesterConfigStore`: maps `PersistentConfig` (per-test `TestConfig` plus station numbers and ports) to and from `config.json`. Each test config is a shared `common` part (retry count) plus its own thresholds. Loading tolerates missing fields and clamps values with `sanitize()`; a corrupt file is copied to `config.json.bad`. Saving writes `*.tmp` first and then renames it over the original.
- Persistence: the model bumps `m_persistRevision` only on user-config changes. Runtime-only station fields go through `updateStation`; port changes must go through `setStationPort`. The service loads the config when its thread starts, saves every 5 s if the revision changed, and saves once more on exit. Before saving on exit it drains any commands still queued.
- Data flow: View → VM command → `ITesterService` (business thread) → `TesterModel` setter + dirty bit → `VM::sync()` → View.
- Alerts from the business thread: call `notify()`/`confirm()` inside the service. This pushes an `AlertEvent` into the model. The VM queues it and `View::DrawAlert` shows the head of the queue. For `Confirm`, the user's answer travels back through `replyAlert(id, accepted)`, and the business thread runs the callback stored for that id. The alert window is non-modal and sits over the log area, so it never blocks the emergency stop.

**Legacy recorder code**: `main_lxrecorder.cpp` and `Model.h` (`BusinessThread`/`DataModel`, with INI autosave to `config.ini`) are the recorder's version of this same pattern, kept for reference.

**Serial abstraction (`Domain.h/.cpp`, namespace `Lx::Domain`)**: `ISerialPort` provides sync and async open/send/recv. `SerialPortLibSerial` wraps wjwwood `serial::Serial` and uses a worker thread with a send queue and a data callback. `SerialPortWin` is an unimplemented stub, and WinUSB support doesn't exist yet.

**Utilities**: `utils/thread_pool.h`, `utils/concurrentqueue` (moodycamel), and `utils/INIReader` + `ini.c` (inih). `utils/utils.h` provides string/encoding helpers and `utils::Alert`. `BinFileWriter.h` handles async binary file writes (recorder-specific).

**Vendored code**: `src/imgui/` (Dear ImGui plus extensions: ImConsole, ImFileDialog, TextEditor, imspinner, memory editor), `src/stb_image.h`, and `thirdparty/`. Don't modify these unless you have to.

## Conventions

- Source files contain Chinese strings and comments. Write UI text as plain `"..."` literals; `/utf-8` makes them UTF-8. Don't use `u8""` literals: in C++20 they are `char8_t` and won't pass to ImGui's `const char*`. The default font's glyph range has no math or arrow symbols (`≥ → ±`), so write `>=` and the like in ASCII.
- In station cards, labels and status bars are aligned with `ImGuiDCXAxisAlign` (from `pch.h`) to one shared X. If you add a row, add its label to the width calculation in `DrawStationCard`.
- The status colors are fixed by the spec: gray `#888888` (pending/disconnected), yellow `#ffcc00` (running/waiting), green `#00bb44` (PASS), red `#dd2222` (FAIL), orange `#ff7700` (ABORT).
- PASS/FAIL is decided on the host by comparing raw measurements against host-side thresholds. Thresholds are never sent to the test board. ABORT comes from the board and has no measurement value. RF tests (packet and frequency offset) hold a global mutex, so only one station runs RF at a time. Power tests can run in parallel.

# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

`lxTester` is a Windows production-test host app (产测上位机) written in C++20 with Dear ImGui (GLFW + OpenGL3 backend) and the wjwwood `serial` library. `readme.md` is the **requirements spec** for the app being built: up to 6 test stations (1 COM/WinUSB port = 1 station = 1 test board + 1 DUT), 3 fixed tests (RF packet rx/tx, frequency offset, power consumption), a strict global state machine, and fixed status colors. Read it before implementing UI or business logic. Its "boundary constraints" and "do not implement" lists are binding.

The codebase started as a copy of a sibling project (`../LxRecorder`, a voice-recording tool). Much of the existing code (speaker info, word lists, recording to `.bin`, background music) belongs to that recorder and is reference material, not part of the tester.

## Build

The build is CMake + vcpkg in manifest mode (`CMakeLists.txt`, `CMakePresets.json`, `vcpkg.json`). `VCPKG_ROOT` must point at a vcpkg checkout (`C:\develop\vcpkg` on the dev machine):

```
cmake --preset x64                  # or x86; uses the Visual Studio 18 2026 generator
cmake --build --preset x64-debug    # x64-release, x86-debug, x86-release
```

- Output goes to `out/build/<preset>/<Config>/`. vcpkg copies `glfw3.dll` and `serial.dll` next to the exe. The VS debugger's working directory is the repo root, so `config.json`, `imgui.ini` and `results/` live there.
- Dependencies come from vcpkg: `glfw3`, `nlohmann-json` (3.12.x, header-only), and `serial`. `serial` comes from the private registry `https://github.com/qianxiansheng/vcpkg-registry-A.git` (it builds upstream wjwwood/serial master with a no-catkin patch, and the target is `serial::serial`). `vcpkg-configuration.json` pins the baselines of both the builtin registry and that one. To pick up a new port version, bump the matching `baseline` there.
- The executable is a console-subsystem app. `app.manifest` enables the segment heap.
- Include dirs: `src`, `src/imgui`, `src/imgui/backends`, `src/imgui/extensions`, plus the vcpkg packages.
- **Which files get compiled is controlled by the source lists in `CMakeLists.txt`, not by what's in `src/`.** Right now it compiles `main.cpp`, `ui_main.cpp`, `protocol/*.cpp`, `link/*.cpp`, `tester/*.cpp`, `utils/*`, and the imgui sources. `Domain.cpp`, `Model.cpp`, and `main_lxrecorder.cpp` are **not** in the build. If you add new .cpp files, add them to `lxTester_SRCS`.
- The project compiles with `/utf-8`. All sources are UTF-8 without a BOM.
- `main.cpp` and `main_lxrecorder.cpp` both define `main_init`/`main_gui`, so only one of them can be compiled at a time.
- There are no tests and no lint setup. To check the protocol codec without the GUI, compile a throwaway `main` together with `src/protocol/BoardProtocol.cpp` using `cl /std:c++20 /utf-8`. That file has no dependencies. Use the simulated test boards for everything else (see below).

The window starts at 1280×880 scaled by monitor DPI (capped to the work area) and can be resized freely. Inside it is an ImGui dockspace. The dock layout is saved to `imgui.ini`, and user settings to `config.json`; both live in the working directory. It also loads the font `C:/Windows/Fonts/msyh.ttc`, which is needed for Chinese text.

## Architecture

**UI shell (`ui_main.cpp` / `ui_main.h`)**: owns `WinMain`/`main`, the GLFW window, the OpenGL3 context, and the ImGui frame loop. Each frame it drains the `PostToUI` task queue, then calls the app hooks `main_init(argc, argv)` (once) and `main_gui()` (every frame; a non-zero return closes the window). `UiDpiContext` (singleton) handles DPI scaling and font rebuilding. `CreateTexture`/`DeleteTexture` are the GL texture adapters used by ImFileDialog. `APP_TITLE` and helper macros are in `pch.h`.

**App layer (`main.cpp`)**: implements `main_init`/`main_gui`/`main_shutdown`. It wires up the tester MVVM objects and creates the root window with a dockspace and a menu bar. The 视图 (View) menu toggles panels and resets the layout, and the 调试 (Debug) menu holds the simulated test-board settings (fault injection) and alert demos. `BuildDefaultDockLayout` builds the default layout when `imgui.ini` has no `TesterDockSpace_v2` node. Bump that name whenever the set of panels changes. The panel window names in `TesterView.h` (`kXxxWindow`) are the dock IDs, so renaming one invalidates saved layouts.

**Protocol (`src/protocol/`, `Lx::Proto`)**: implements the host-to-test-board protocol from `产测协议规范.md`. Each frame is `55 AA | TID(LE) | OpCode(**BE**) | Len | Payload(≤54) | CRC16-MODBUS(LE) | 33`. An ack's OpCode is the request's OpCode `| 0x8000`. `FrameParser` reassembles a byte stream and resyncs by dropping one byte when it hits a bad header, length, CRC or tail. Every ack payload starts with `result_code`. When `rc != RET_OK`, only the rc is guaranteed to be present.

**Link (`src/link/`, `Lx::Link`)**:
- `ITransport`: a byte-stream transport whose callbacks run on its own IO thread. Two implementations exist:
  - `SerialTransport`, built directly on `serial::Serial`. Any read exception means the port was physically disconnected.
  - `UsbTransport` uses the native WinUSB API: SetupAPI, cfgmgr32 and `WinUsb_*`, with the libraries pulled in by `#pragma comment`.
    - The port name is the device **instance ID**.
    - `enumerateUsbDevices(vid, pid)` lists present USB nodes that match the VID/PID. Composite parents (`usbccgp`) are skipped and their `MI_xx` children are listed instead.
    - The interface path comes from the `DeviceInterfaceGUIDs` registry value, falling back to `GUID_DEVINTERFACE_USB_DEVICE`.
    - Endpoints are auto-picked: the first IN and OUT pipe, bulk preferred over interrupt.
    - The reader thread uses overlapped `WinUsb_ReadPipe`. `close()` keeps calling `WinUsb_AbortPipe` until the reader exits. Any read error counts as a disconnect.
  - `SimTransport`, a simulated test board plus DUT. Port names `SIM1`..`SIM6` select it. Its behaviour is controlled by the global `SimSettings`, which the 调试 → 模拟测试板 menu edits.
- `BoardLink`: one per port, used only on the business thread. It matches replies by TID, runs a per-request timeout, sends a 500 ms `ALIVE` heartbeat, and reports the board lost after 3 consecutive timeouts or a transport close. After a loss or `close()`, pending replies are dropped and never called back.
- `IEventLoop`: `post`, `after`, `cancel`. `TesterService` implements it.
- USB VID/PID live in `CommConfig::usbVid`/`usbPid` (default 0000), and changing them re-enumerates the ports. New stations default to `PortType::Usb`. `PortEntry` carries `type`, a short `label` and `usable`, which is false when the device isn't bound to WinUSB.

**Tester (`src/tester/`, namespace `Lx::Tester`)**: MVVM split. ImGui has no data binding, so dirty flags stand in for it.
- `TesterTypes.h`: the domain enums and structs (global, DUT, station and test-item states, `TestConfig`, `AlertEvent`). No ImGui here.
- `TesterModel`: a thread-safe store and the single source of truth. Every setter ORs a `Dirty_*` bit. Logs carry a monotonically increasing `seq` so the UI can fetch them incrementally. Alerts sit in a queue that is drained with `takeAlerts()`.
- `TesterService` (implements `ITesterService`, `IEventLoop`, `IStationHost`): owns the business thread, an event loop of a task queue plus timers that never blocks. Every UI command is posted to that thread, and only that thread writes the model. It also owns the global state machine (`recomputeGlobal`), the FIFO RF lock (`acquireRf`/`releaseRf`), the per-round CSV (`ResultCsv`, `results/results_YYYYMMDD.csv`, UTF-8 with a BOM) and the statistics.
- `StationController`: one per station, and the place where the protocol flow lives.
  - Connect: open the port → first heartbeat ack (board connected) → `GET_VERSION` → `CONN_CHECK_DUT`. If the DUT isn't ready, the handshake retries every 1 s, but only while the station is not testing and not finished.
  - Each test item: start op → poll `GET_TEST_STATUS` every poll interval until idle or error → `GET_RESULT` → judge on the host.
  - Order: 收发包 → 频偏 while holding the RF lock, then 功耗.
  - After the items:
    - If all three PASS, write the FT flag (`ft*` methods): `RESET_DUT` (retried) → `CONN_CHECK_DUT` → `SET_FT_FLAG` (retried) → delay → `GET_FT_FLAG`, which must read back 1. Any failure makes the round FAIL.
    - Otherwise the flag step is `Skipped`, and the station only does `RESET_DUT` + `CONN_CHECK_DUT`.
    - The flag result lives in `StationInfo::ftFlag` / `RoundRecord::ftFlag`, separate from `items[]`.
  - Retries: an ABORT that wasn't caused by the emergency stop is retried after `RESET_DUT` + `CONN_CHECK_DUT`. FAIL is never retried.
  - Async callbacks are guarded by `m_connGen`/`m_testGen` plus a `weak_ptr` (`guardConn`/`guardTest`). Bump the matching generation whenever you abandon a flow.
- `TesterViewModel`: used on the UI thread only, with no locks. `sync()` runs once per frame at the start of `main_gui`. It consumes the dirty flags and re-snapshots only the parts that changed (formatted strings, an alert deque). It also exposes `canXxx()` enable rules and forwards commands to the service.
- `TesterView`: plain ImGui drawing that touches only the ViewModel. There is one dockable window per panel: 控制台 (console), 工位管理 (stations), 测试参数 (parameters), 日志 (log), 统计 (statistics; it shares a tab group with the log). Log selection state (a range of seqs) lives in the VM, and copying text goes through `ImGui::SetClipboardText`.
- `TesterConfigStore`: maps `PersistentConfig` (per-test `TestConfig` + `CommConfig`, plus station number, port type and port) to and from `config.json`. Each test config is a shared `common` part (retry count) plus its own thresholds. Loading tolerates missing fields and clamps values with `sanitize()`; a corrupt file is copied to `config.json.bad`. Saving writes `*.tmp` first and then renames it over the original.
- Persistence: the model bumps `m_persistRevision` only on user-config changes. Runtime-only station fields go through `updateStation`; port changes must go through `setStationPort`. The service loads the config when its thread starts, saves every 5 s if the revision changed, and saves once more on exit. Before saving on exit it drains any commands still queued.
- Data flow: View → VM command → `ITesterService` (business thread) → `TesterModel` setter + dirty bit → `VM::sync()` → View.
- Alerts from the business thread: call `notify()`/`confirm()` inside the service. This pushes an `AlertEvent` into the model. The VM queues it and `View::DrawAlert` shows the head of the queue. For `Confirm`, the user's answer travels back through `replyAlert(id, accepted)`, and the business thread runs the callback stored for that id. The alert window is non-modal and sits over the log area, so it never blocks the emergency stop.

**Legacy recorder code**: `main_lxrecorder.cpp` and `Model.h` (`BusinessThread`/`DataModel`, with INI autosave to `config.ini`) are the recorder's version of this same pattern, kept for reference.

**Legacy serial abstraction (`Domain.h/.cpp`, not compiled)**: this is the recorder's serial wrapper. Its worker thread swallows I/O errors, so it can't detect an unplugged port. The tester uses `link/SerialTransport.cpp` instead.

**Utilities**: `utils/thread_pool.h`, `utils/concurrentqueue` (moodycamel), and `utils/INIReader` + `ini.c` (inih). `utils/utils.h` provides string/encoding helpers and `utils::Alert`. `BinFileWriter.h` handles async binary file writes (recorder-specific).

**Vendored code**: `src/imgui/` (Dear ImGui plus extensions: ImConsole, ImFileDialog, TextEditor, imspinner, memory editor), and `src/stb_image.h`. Don't modify these unless you have to.

## Conventions

- Source files contain Chinese strings and comments. Write UI text as plain `"..."` literals; `/utf-8` makes them UTF-8. Don't use `u8""` literals: in C++20 they are `char8_t` and won't pass to ImGui's `const char*`. The default font's glyph range has no math or arrow symbols (`≥ → ±`), so write `>=` and the like in ASCII.
- In station cards, labels and status bars are aligned with `ImGuiDCXAxisAlign` (from `pch.h`) to one shared X. If you add a row, add its label to the width calculation in `DrawStationCard`.
- The status colors are fixed by the spec: gray `#888888` (pending/disconnected), yellow `#ffcc00` (running/waiting), green `#00bb44` (PASS), red `#dd2222` (FAIL), orange `#ff7700` (ABORT).
- PASS/FAIL is decided on the host by comparing raw measurements against host-side thresholds. Thresholds are never sent to the test board. ABORT comes from the board and has no measurement value. RF tests (packet and frequency offset) hold a global mutex, so only one station runs RF at a time. Power tests can run in parallel.

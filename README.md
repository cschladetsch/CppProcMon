<p align="center">
  <img src="assets/logo.png" width="140" alt="CppProcMon logo" />
</p>

# CppProcMon
A small Dear ImGui desktop app for Windows that shows realtime, per-process
CPU%, RAM and VRAM usage in a sortable, filterable table — refreshed every
second by default (adjustable live in the UI).

## Features

- Per-process CPU % (normalized system-wide, same convention as Task Manager)
- Per-process RAM: working set and private/commit bytes
- Per-process VRAM: dedicated (on-GPU) and shared (system RAM borrowed by
  the GPU) usage, plus summed GPU engine utilization %
- Sortable/resizable/reorderable table, live substring filter by process name
- Adjustable refresh interval (0.25s–10s, default 1s)
- Polling runs on a background thread so the UI stays responsive
- Right-click any row for full process details (image path, exact CPU/RAM/VRAM
  figures) plus Copy PID, Copy image path, and Kill Process (with a
  confirmation prompt)

## Prerequisites

- Windows 10 (1803+) or Windows 11 — per-process GPU counters require this
- **Clang 17+ (18+ recommended)** on PATH as `clang++` — this is the default
  compiler (see "Build system" below). Install via the
  [LLVM releases page](https://releases.llvm.org/), `winget install LLVM.LLVM`,
  or `choco install llvm`. MSVC also works (`-Compiler cl`) if you'd rather
  use Visual Studio 2022's toolchain.
- [Ninja](https://ninja-build.org/) recommended (auto-detected if present;
  the usual pairing with `clang++` on Windows outside a VS environment).
  `winget install Ninja-build.Ninja` or `choco install ninja`.
- [CMake](https://cmake.org/download/) 3.25+
- Git (both for CMake's `FetchContent`, which pulls GLFW and Dear ImGui, and
  for CppLogicMake's git-backed source resolution — see below)
- Internet access on first configure (dependencies are fetched, then cached
  locally in `build/_deps`)
- Optional: [CppLogicMake](https://github.com/cschladetsch/CppLogiMake)
  (`logimake` on PATH, or `$env:LOGICMAKE_ROOT` set) to regenerate
  `generated/CMakeLists.txt` from `ProcMon.lm`. Not required to build — a
  pre-generated copy is checked in.

## Build system

`procmon`'s own target definition — sources, include paths, links, compile
definitions — is authored as Prolog facts in [`ProcMon.lm`](ProcMon.lm) and
transpiled to `generated/CMakeLists.txt` by
[CppLogicMake](https://github.com/cschladetsch/CppLogiMake), pulled into the
root `CMakeLists.txt` via `add_subdirectory(generated)`. CppLogicMake doesn't
yet model `FetchContent` (an explicit non-goal of the tool for now), so
fetching and building GLFW + Dear ImGui stays hand-written CMake in
[`cmake/ThirdParty.cmake`](cmake/ThirdParty.cmake), included from the root
`CMakeLists.txt` alongside the generated target. A checked-in copy of
`generated/CMakeLists.txt` ships in this archive, so the project builds
out of the box even without CppLogicMake installed; `s.ps1` regenerates it
automatically when `logimake` is available.

Note the executable itself is named lowercase **`procmon.exe`** — Prolog
atoms (CppLogicMake target names) are lowercase by syntax, so that's what
`add_executable` in the generated file produces, even though the project
and window title are still "ProcMon".

To regenerate `generated/CMakeLists.txt` by hand (requires this to be a git
checkout, since CppLogicMake resolves `sources(procmon, "src/*.cpp")` via
`git ls-files`):

```powershell
git init; git add -A       # only needed once, if not already a git repo
logimake generate -Input ProcMon.lm -Output generated/CMakeLists.txt -WorkingDirectory .
```

## Build & run

Easiest path — from PowerShell:

```powershell
.\s.ps1
```

This regenerates `generated/CMakeLists.txt` (if `logimake` is available),
configures, builds (Release, `clang++`, parallel) and launches the app.
Useful switches:

```powershell
.\s.ps1 -Configuration Debug                          # Debug build
.\s.ps1 -Compiler cl -Generator "Visual Studio 17 2022"  # use MSVC instead
.\s.ps1 -NoRegenerate                                  # skip the CppLogicMake step
.\s.ps1 -Clean                                         # wipe build/ first
.\s.ps1 -NoRun                                         # build only, don't launch
```

Manual equivalent:

```powershell
logimake generate -Input ProcMon.lm -Output generated/CMakeLists.txt -WorkingDirectory .   # optional
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
.\build\procmon.exe
```

## Usage

- Click a column header to sort; shift-click to add a secondary sort in
  some ImGui builds. Click again to flip direction.
- Type in the filter box to narrow the list by process name.
- Drag the "Update interval" slider to change how often the table refreshes
  (default 1 second).
- Rows for protected/elevated processes may show `0 B` / `0.0%` if the app
  wasn't run elevated — Windows denies `OpenProcess` for those otherwise.
  Run as Administrator to see full detail for system processes.
- Right-click a row to open its context menu: process name, PID, full image
  path, and exact CPU/RAM/VRAM/GPU figures at the top, then Copy PID, Copy
  image path, and Kill Process below. Kill Process asks for confirmation
  before calling `TerminateProcess` — killing protected/elevated processes
  without running as Administrator will fail with an access-denied message
  shown at the top of the window instead of silently doing nothing.

## How it works

- **CPU / RAM** (`src/ProcessMonitor.cpp`): enumerates processes via
  `CreateToolhelp32Snapshot`, and for each PID reads `GetProcessTimes`
  (kernel+user ticks, differenced between polls and normalized by elapsed
  time × core count) and `GetProcessMemoryInfo` (working set + private
  bytes).
- **VRAM / GPU %** (`src/GpuMonitor.cpp`): uses the PDH (Performance Data
  Helper) API against the `GPU Process Memory` and `GPU Engine` performance
  counter objects — the same data source Task Manager's per-process GPU
  columns use. Counter instances are wildcard-expanded and re-synced every
  poll since GPU contexts are created/destroyed constantly; the PID is
  parsed out of each instance name (`pid_1234_luid_..._phys_0`).
- **System totals** (`src/SystemInfo.cpp`): `GlobalMemoryStatusEx` for total
  RAM, DXGI adapter enumeration for total VRAM capacity shown in the header.
- **UI** (`src/main.cpp`): GLFW + OpenGL3 + Dear ImGui, with polling on a
  dedicated worker thread (`std::thread` + mutex-guarded snapshot) so a slow
  PDH cycle never stalls rendering.
- **Kill Process**: opens a *separate* short-lived handle with just
  `PROCESS_TERMINATE` (the polling handles in `ProcessMonitor` intentionally
  don't carry that right) and calls `TerminateProcess`, gated behind an
  in-app confirmation modal.
- **Build graph** (`ProcMon.lm` → `generated/CMakeLists.txt`): CppLogicMake
  resolves `procmon`'s sources via `git ls-files` against the pathspec in
  `ProcMon.lm`, resolves its link/define facts through
  `prolog/targets.pl`'s Prolog rules, and emits plain
  `add_executable`/`target_link_libraries`/`target_compile_definitions`
  calls — no CppLogicMake-specific machinery is needed at CMake-configure
  time, only at the (optional) regeneration step.

## Known limitations

- Windows-only by design (PDH, PSAPI, ToolHelp32, DXGI are all Win32 APIs).
- GPU counters depend on the display driver exposing `GPU Process Memory` /
  `GPU Engine`; on unsupported/older drivers VRAM columns will read 0.
- A newly-appeared GPU engine instance can read 0% for one poll cycle since
  PDH's rate counters need two samples before they're meaningful.
- Multi-GPU systems: per-process VRAM is summed across all adapters/engines
  rather than broken out per GPU.
- `generated/CMakeLists.txt` always contains `project(generated LANGUAGES CXX)`
  (CppLogicMake hard-codes that project name) — harmless since it's pulled
  in via `add_subdirectory`, but don't be surprised if `PROJECT_NAME` reads
  "generated" partway through configure.
- Regenerating requires a git checkout with `src/*.cpp` tracked; a fresh
  `git init && git add -A` is enough (no commit needed) since resolution
  reads the index via `git ls-files`, not HEAD.

## License

MIT for the code in this repository. GLFW and Dear ImGui are fetched at
build time and remain under their own licenses (zlib and MIT respectively).

# Windows VSCode Build & Run — Design Spec

**Date:** 2026-05-15  
**Status:** Approved

## Problem

`dpi_engine` is a macOS ARM64 binary. It cannot run on Windows. The C++ source is already cross-platform (C++17 standard library only — no POSIX/unistd/pthread headers). It just needs to be compiled for Windows and wired into VSCode so output prints in the integrated terminal.

## Goal

Run `.\dpi_engine.exe test_dpi.pcap output.pcap` from the VSCode integrated terminal and see all logs (`std::cout`/`std::cerr`) inline.

## Solution

Create `.vscode/tasks.json` inside `Packet_analyzer/` with two tasks.

### Task 1 — Build DPI Engine (default build task)

- **Trigger:** `Ctrl+Shift+B`
- **Command:** `g++` with flags:
  - `-std=c++17 -pthread -O2`
  - `-I include`
  - `-o dpi_engine.exe`
  - Sources: `src/dpi_mt.cpp src/pcap_reader.cpp src/packet_parser.cpp src/sni_extractor.cpp src/types.cpp`
- **cwd:** `${workspaceFolder}` (i.e. `Packet_analyzer/`)
- **Problem matcher:** `$gcc` — surfaces compile errors in the Problems panel
- **Group:** `build`, `isDefault: true`

### Task 2 — Run DPI Engine

- **Trigger:** `Ctrl+Shift+P` → Tasks: Run Task → Run DPI Engine
- **Command:** `.\dpi_engine.exe test_dpi.pcap output.pcap`
- **cwd:** `${workspaceFolder}`
- **dependsOn:** Build DPI Engine (auto-builds before run)
- **Panel:** shared, revealed on execution — logs print in VSCode terminal

## Files Changed

| File | Action |
|------|--------|
| `.vscode/tasks.json` | Create (new) |

## Files NOT Changed

- All `src/*.cpp` and `include/*.h` — zero source changes needed
- `dpi_engine` (macOS binary) — left as-is
- `CMakeLists.txt` — not modified (tasks.json replaces it for this workflow)

## Assumptions

- MinGW/GCC is installed and `g++` is on the system PATH
- VSCode workspace root is `Packet_analyzer/`
- `test_dpi.pcap` exists in `Packet_analyzer/`

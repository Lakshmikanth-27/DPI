# Windows VSCode Build & Run Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Create `.vscode/tasks.json` so the DPI engine compiles to `dpi_engine.exe` with `Ctrl+Shift+B` and runs with output printed in the VSCode integrated terminal.

**Architecture:** A single `tasks.json` file wires the existing cross-platform C++ source (`dpi_mt.cpp` + supporting files) into VSCode's task runner using g++/MinGW. A Build task compiles the exe; a Run task depends on Build and launches the exe in the integrated terminal so all stdout/stderr is visible.

**Tech Stack:** C++17, g++ (MinGW), VSCode tasks.json v2.0.0

---

### Task 1: Create `.vscode/tasks.json`

**Files:**
- Create: `Packet_analyzer/.vscode/tasks.json`

- [ ] **Step 1: Verify g++ is on PATH**

Open the VSCode integrated terminal (`Ctrl+\``) and run:
```powershell
g++ --version
```
Expected output (version may differ):
```
g++ (x86_64-posix-seh-rev0, Built by MinGW-W64 project) 13.x.x
```
If `g++ is not recognized`, install MinGW via MSYS2 and add `C:\msys64\mingw64\bin` to PATH, then restart VSCode.

- [ ] **Step 2: Verify test_dpi.pcap exists**

```powershell
Test-Path "test_dpi.pcap"
```
Expected: `True`

If `False`, generate it:
```powershell
python generate_test_pcap.py
```

- [ ] **Step 3: Create the .vscode directory**

```powershell
New-Item -ItemType Directory -Force -Path ".vscode"
```

- [ ] **Step 4: Write tasks.json**

Create `Packet_analyzer/.vscode/tasks.json` with this exact content:

```json
{
    "version": "2.0.0",
    "tasks": [
        {
            "label": "Build DPI Engine",
            "type": "shell",
            "command": "g++",
            "args": [
                "-std=c++17",
                "-pthread",
                "-O2",
                "-I", "include",
                "-o", "dpi_engine.exe",
                "src/dpi_mt.cpp",
                "src/pcap_reader.cpp",
                "src/packet_parser.cpp",
                "src/sni_extractor.cpp",
                "src/types.cpp"
            ],
            "group": {
                "kind": "build",
                "isDefault": true
            },
            "problemMatcher": ["$gcc"],
            "presentation": {
                "reveal": "always",
                "panel": "shared"
            }
        },
        {
            "label": "Run DPI Engine",
            "type": "shell",
            "command": ".\\dpi_engine.exe",
            "args": [
                "test_dpi.pcap",
                "output.pcap"
            ],
            "dependsOn": "Build DPI Engine",
            "group": "test",
            "presentation": {
                "reveal": "always",
                "panel": "shared",
                "focus": true
            },
            "problemMatcher": []
        }
    ]
}
```

- [ ] **Step 5: Build via Ctrl+Shift+B**

Press `Ctrl+Shift+B` in VSCode (or `Ctrl+Shift+P` → "Tasks: Run Build Task").

Expected terminal output ends with something like:
```
 *  Terminal will be reused by tasks, press any key to close it.
```
And `dpi_engine.exe` appears in the `Packet_analyzer/` folder:
```powershell
Test-Path "dpi_engine.exe"
# True
```

If build fails with `undefined reference to std::thread`, ensure `-pthread` flag is present (already in the args above).

- [ ] **Step 6: Run via task**

Press `Ctrl+Shift+P` → type `Tasks: Run Task` → select **Run DPI Engine**.

Expected terminal output includes the DPI engine banner and ends with:
```
Output written to: output.pcap
```

All `std::cout` and `std::cerr` lines from the engine print directly in the VSCode integrated terminal.

- [ ] **Step 7: Commit**

```powershell
git add .vscode/tasks.json
git commit -m "build: add VSCode tasks for Windows g++ build and run"
```

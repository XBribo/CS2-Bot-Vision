# BotVision Developer Guide

Installation and usage: [README.md](README.md).

## Build

Run commands from the repository root.

### Requirements

- **Windows:** Visual Studio 2026 C++ build tools and CMake.
- **Linux:** `g++`, CMake, and Make.
- **Build script:** PowerShell 7+; WSL for Linux (`Ubuntu-24.04` by default).
- **Dependencies:** Git and the `cs2_sdk` submodule.

```bash
git submodule update --init --recursive
```

### Build Script

On a Windows host:

```powershell
./build.ps1             # Windows and Linux (WSL)
./build.ps1 -Windows    # Windows only
./build.ps1 -Linux      # Linux only (WSL)
```

Packages: `dist/windows/` and `dist/linux/`. Use `-Clean` for a clean rebuild.

### Manual Build

Windows:

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
```

Linux:

```bash
cmake -S . -B out/linux -DCMAKE_BUILD_TYPE=Release
cmake --build out/linux -j$(nproc)
```

Native output: `<build-directory>/package/addons/`.

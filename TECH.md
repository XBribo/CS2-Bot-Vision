# BotVision Developer Guide

Build BotVision from source. For installation, configuration, and console commands, see [README.md](README.md).

## Build

Run commands from the repository root. Requirements:

- **Windows:** Visual Studio 2026 C++ build tools and CMake.
- **Linux:** `g++`, CMake, and Make.
- **Build script:** PowerShell 7 or newer on Windows; Linux builds use WSL (`Ubuntu-24.04` by default).
- **Dependencies:** Git and recursively initialized submodules.

```bash
git submodule update --init --recursive
```

Dependencies are provided by the `cs2_sdk` submodule.

### Build Script

On a Windows host:

```powershell
./build.ps1             # Windows and Linux (WSL)
./build.ps1 -Windows    # Windows only
./build.ps1 -Linux      # Linux only (WSL)
```

Packages are assembled in `dist/windows/` and `dist/linux/`. Add `-Clean` for a clean rebuild.

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

CMake output is under `<build-directory>/package/addons/`.

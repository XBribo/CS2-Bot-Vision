# BotVision Developer Guide

## Smoke visibility

- [BotVision.h](src/features/vision/BotVision.h) controls density thresholds and revealed players. A negative per-bot threshold restores the global setting.
- [NativeSmoke.h](src/features/smoke/NativeSmoke.h) provides `Density(from, to, corrected)`. Call it on the server thread with two world-space positions. Pass `true` to include HE and bullet effects. A non-finite result means density is unavailable; use the engine's visibility decision.

## Diagnostics

Use `bv_status` to check hooks and `bv_test_los <x1> <y1> <z1> <x2> <y2> <z2>` to compare raw and corrected density. To measure the geometry cache, compare tick time in the same scene with `bv_geometry_cache 0` and `1`.

## Build from source

Initialize dependencies with `git submodule update --init --recursive`. Run `./build.ps1 -Windows` or `./build.ps1 -Linux` (WSL). Packages are written to `dist/`.

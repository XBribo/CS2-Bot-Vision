# BotVision

**Make Bot Vision Great Again**

## Your stars⭐ are my motivation to keep updating

------------------------------------------------------------------------

## Overview

`BotVision` is a **Metamod:Source plugin** for **Counter-Strike 2**
servers that fixes bot line-of-sight through smoke.

Stock bots see through the new volumetric smoke.
Bots get blocked by smoke the same way a human would be.

------------------------------------------------------------------------

## Features

- Volumetric-smoke-aware bot vision.

------------------------------------------------------------------------

## Console commands

- `bv_status` — print hook hit/blocked counts and resolved sig status.
- `bv_smoke_mode <0|1>` — `0` = volume-smoke (fixed), `1` = ball-smoke (stock).
- `bv_density_threshold <d>` — mode-0 blocking threshold on density (default `0.2`).
- `bv_test_los <x1> <y1> <z1> <x2> <y2> <z2>` — query smoke density along a segment.

------------------------------------------------------------------------

## Install

1. Download the latest release for your platform from the
   [**GitHub Releases**](https://github.com/XBribo/CS2-Bot-Vision/releases/latest) page:

        BotVision-windows.zip   # for Windows servers

2. Extract the archive and copy the `/addons/` directory into `/game/csgo/`.

3. Restart your game server.

------------------------------------------------------------------------

## How to Build

**Windows:**

``` text
cmake -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
```

Env required: `HL2SDKCS2`, `MMSOURCE_DEV`, `CSGO_PROTO`, plus `protoc`
(3.21.x) on PATH.

------------------------------------------------------------------------

## License

GPLv3

------------------------------------------------------------------------

## Author

**XBribo**

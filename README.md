# BotVision

**Make Bot Vision Great Again**

## Your stars⭐ are my motivation to keep updating

------------------------------------------------------------------------

## Overview

`BotVision` is a **Metamod:Source plugin** for **Counter-Strike 2**
servers that fixes bot line-of-sight through volumetric smoke.

BotVision evaluates smoke density, HE blast holes, and bullet holes before deciding whether a bot can see a target.

------------------------------------------------------------------------

## Configuration

Startup settings are stored in `addons/BotVision/config.json`:

```json
{
    "smoke": {
        "mode": 0,
        "density_threshold": 0.23
    },
    "he_holes": {
        "radius": 250.0,
        "duration": 5.0
    },
    "bullet_holes": {
        "enabled": true,
        "radius": 20.0,
        "shotgun_radius": 80.0,
        "duration": 1.0,
        "range": 8192.0
    }
}
```

Set `smoke.mode` to `0` for volumetric smoke or `1` for the stock ball-smoke calculation. Radii, durations, the density threshold, and the bullet range must be nonnegative. Missing or invalid settings use the defaults shown above. If `config.json` is missing, BotVision creates it on startup. Reload the plugin or restart the server after changing the configuration.

------------------------------------------------------------------------

## Console commands

- `bv_status` - print hook, smoke, HE, and bullet-tunnel diagnostics.
- `bv_smoke_mode <0|1>` - `0` uses volumetric smoke; `1` uses the stock
  ball-smoke calculation.
- `bv_density_threshold <d>` - set the global smoke-density blocking
  threshold for the current session.
- `bv_bot_density [<slot> <d>]` - list, query, or set a per-bot density
  threshold; a negative value restores the global threshold.
- `bv_test_los <x1> <y1> <z1> <x2> <y2> <z2>` - query smoke density along
  a segment.
- `bv_bullet_holes <0|1>` - enable or disable bullet smoke tunnels.

### Through-smoke reveal

Slots range from `0` to `63`.

```text
bv_reveal add 10
bv_reveal add 11
bv_reveal list
bv_reveal remove 10
bv_reveal clear
```

- `add <slot>` - reveal a player to bots through smoke.
- `remove <slot>` - remove one player from the reveal set.
- `list` - list selected players and their entity handles.
- `clear` - clear the reveal set.

------------------------------------------------------------------------

## Install

Requirement: [RayTrace](https://github.com/FUNPLAY-pro-CS2/Ray-Trace)

1. Download the latest Windows release from
   [GitHub Releases](https://github.com/XBribo/CS2-Bot-Vision/releases/latest):

   ```text
   BotVision-windows.zip
   ```

2. Extract the archive and copy the `addons/` directory into
   `game/csgo/`.

3. Restart the game server.

------------------------------------------------------------------------

## How to Build

**Windows:**

```text
cmake -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
```

Required environment variables: `HL2SDKCS2`, `MMSOURCE_DEV`, and
`CSGO_PROTO`. `protoc` 3.21.x must also be available on `PATH`.

------------------------------------------------------------------------

## License

CS2-Bot-Vision is licensed under the GNU Affero General Public License
version 3 (AGPL-3.0). Commercial use involving closed-source distribution
or hosted services may require a separate license. See `LICENSE` for details.

------------------------------------------------------------------------

## Author

**XBribo**

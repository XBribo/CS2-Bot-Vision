# BotVision

**Make Bot Vision Great Again**

## Your stars⭐ are my motivation to keep updating

------------------------------------------------------------------------

## Overview

`BotVision` is a **Metamod:Source plugin** for **Counter-Strike 2**
servers that fixes bot line-of-sight through volumetric smoke.

BotVision evaluates smoke density, HE blast holes, and bullet holes before deciding whether a bot can see a target.

------------------------------------------------------------------------

## Console commands

- `bv_status` - print hook, smoke, HE, and bullet-tunnel diagnostics.
- `bv_smoke_mode <0|1>` - `0` uses volumetric smoke; `1` uses the stock
  ball-smoke calculation.
- `bv_density_threshold <d>` - set the global smoke-density blocking
  threshold.
- `bv_bot_density [<slot> <d>]` - list, query, or set a per-bot density
  threshold; a negative value restores the global threshold.
- `bv_test_los <x1> <y1> <z1> <x2> <y2> <z2>` - query smoke density along
  a segment.
- `bv_he_radius <r>` - set the HE smoke-hole radius.
- `bv_he_duration <s>` - set the HE smoke-hole lifetime.
- `bv_bullet_holes <0|1>` - enable or disable bullet smoke tunnels.
- `bv_bullet_radius <r>` - set the normal bullet-tunnel radius.
- `bv_bullet_radius_shotgun <r>` - set the shotgun bullet-tunnel radius.
- `bv_bullet_duration <s>` - set the bullet-tunnel lifetime.

### Through-smoke reveal

Player slots range from `0` to `63`. Multiple human players or bots can be
selected at the same time:

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

`hook=active` in `bv_reveal list` confirms that the player-visibility hook
was installed successfully.

------------------------------------------------------------------------

## Install

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

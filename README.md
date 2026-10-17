# BotVision

A Metamod:Source plugin for Counter-Strike 2 that corrects bot visibility through volumetric smoke, including HE blast holes and bullet tunnels.

- [Install](#install)
- [Configuration](#configuration)
- [Console Commands](#console-commands)
- [Developer Guide](TECH.md)

## Install

1. Install Metamod:Source on the server.
2. Download the package for your server platform from [GitHub Releases](https://github.com/XBribo/CS2-Bot-Vision/releases/latest).
3. Copy the package's `addons/` directory into `game/csgo/`, then restart the server.
4. Run `bv_status` to check hook and smoke diagnostics.

## Configuration

Startup settings are stored in `game/csgo/addons/BotVision/config.json`. Defaults:

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
        "duration": 1.0
    }
}
```

- `smoke.mode`: `0` uses volumetric density; `1` uses the stock ball-smoke calculation. The density threshold controls mode `0`.
- Radii, durations, and the density threshold must be finite, nonnegative values within the supported numeric range. Missing or invalid settings use defaults.
- If the file is missing, the plugin attempts to create it and uses defaults. Reload the plugin or restart the server after editing it.
- Console changes affect runtime state only; they do not update `config.json`.

## Console Commands

Slots are engine player slots from `0` to `63`, not user IDs or entity indices.

| Command | Purpose |
| --- | --- |
| `bv_status` | Show hook, smoke, HE, and bullet-tunnel diagnostics |
| `bv_smoke_mode [0\|1]` | Query or change the smoke calculation mode |
| `bv_density_threshold [d]` | Query or change the global volumetric blocking threshold |
| `bv_bot_density [<slot> [<d>]]` | List, query, or set per-bot thresholds; negative `d` restores the global threshold |
| `bv_bullet_holes [0\|1]` | Query, enable, or disable bullet smoke tunnels |
| `bv_test_los <x1> <y1> <z1> <x2> <y2> <z2>` | Query smoke density along a segment |
| `bv_reveal add <slot>` | Let bots see the selected player through smoke |
| `bv_reveal remove <slot>` | Remove a selected player from the reveal set |
| `bv_reveal list` | List selected players and latched entity handles |
| `bv_reveal clear` | Clear the reveal set |

Per-bot thresholds require the `IsVisiblePos` hook; target reveal requires `IsVisiblePlayer`. Check `bv_status` if either feature is unavailable. If the engine density function is unavailable, mode `0` falls back to ball-smoke calculation.

## License

CS2-Bot-Vision is licensed under the GNU Affero General Public License version 3 (AGPL-3.0). Commercial use involving closed-source distribution or hosted services may require a separate license. See [LICENSE](LICENSE) for details.

Author: **XBribo**.

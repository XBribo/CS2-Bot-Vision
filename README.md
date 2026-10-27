<div align="center">

# BotVision

<p>
  <a href="https://github.com/XBribo/CS2-Bot-Vision/releases/latest"><img src="https://img.shields.io/github/v/release/XBribo/CS2-Bot-Vision?style=flat-square" alt="Release"></a>
  <a href="https://github.com/XBribo/CS2-Bot-Vision/releases/latest"><img src="https://img.shields.io/badge/platforms-Windows%20%7C%20Linux-0078D4?style=flat-square" alt="Platforms"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-AGPL--3.0-blue?style=flat-square" alt="License"></a>
</p>

[Quick Start](#quick-start) · [Configuration](#configuration) · [Commands](#commands) · [Developer Guide](TECH.md)

</div>

A Metamod:Source plugin for Counter-Strike 2 that corrects bot vision through volumetric smoke, HE blast holes, and bullet tunnels.

### Signature Status

[![CS2 Bot Signatures Tracker](https://raw.githubusercontent.com/XBribo/cs2-bot-signatures/main/.github/docs/summary.svg)](https://github.com/XBribo/cs2-bot-signatures)

## Quick Start

1. Install Metamod:Source on the server.
2. Download your platform's package from [Releases](https://github.com/XBribo/CS2-Bot-Vision/releases/latest).
3. Copy `addons/` into `game/csgo/` and restart the server.
4. Run `bv_status` to check hooks and smoke diagnostics.

## Configuration

Edit `game/csgo/addons/BotVision/config.json`, then reload the plugin or restart the server. See the [default config](configs/addons/BotVision/config.json).

| Setting | Default | Description |
| --- | --- | --- |
| `smoke.mode` | `0` | `0`: volumetric density; `1`: stock ball-smoke |
| `smoke.density_threshold` | `0.23` | Blocking threshold in mode `0` |
| `he_holes.radius` | `250.0` | HE blast-hole radius |
| `he_holes.duration` | `5.0` | HE blast-hole duration |
| `bullet_holes.enabled` | `true` | Enable bullet tunnels |
| `bullet_holes.radius` | `20.0` | Bullet-tunnel radius |
| `bullet_holes.shotgun_radius` | `80.0` | Shotgun-tunnel radius |
| `bullet_holes.duration` | `1.0` | Bullet-tunnel duration |

Numeric values must be finite, nonnegative, and within supported ranges. Missing or invalid settings use defaults; missing files are created when possible.

> Console commands adjust runtime settings. Use `config.json` for persistent settings.

## Commands

Use engine player slots (`0–63`).

| Command | Description |
| --- | --- |
| `bv_status` | Show hook and smoke diagnostics |
| `bv_smoke_mode [0\|1]` | Query or set smoke mode |
| `bv_density_threshold [d]` | Query or set the global density threshold |
| `bv_bot_density [<slot> [<d>]]` | List, query, or set bot thresholds; negative `d` restores the global value |
| `bv_bullet_holes [0\|1]` | Query or toggle bullet tunnels |
| `bv_test_los <x1> <y1> <z1> <x2> <y2> <z2>` | Measure smoke density along a segment |
| `bv_reveal add <slot>` | Let bots see this player through smoke |
| `bv_reveal remove <slot>` | Remove a revealed player |
| `bv_reveal list` | List revealed players and latched entity handles |
| `bv_reveal clear` | Clear all reveals |

> Per-bot thresholds need `IsVisiblePos`; reveal needs `IsVisiblePlayer`. Check `bv_status` for missing hooks. Without the engine density function, mode `0` falls back to ball-smoke.

## Credits

Created by [XBribo](https://github.com/XBribo), with thanks to all contributors.

[![BotVision contributors](https://contrib.rocks/image?repo=XBribo/CS2-Bot-Vision)](https://github.com/XBribo/CS2-Bot-Vision/graphs/contributors)

## License

CS2-Bot-Vision is licensed under the GNU Affero General Public License version 3 (AGPL-3.0).
Commercial use involving closed-source distribution or hosted services may require a separate license.
See [LICENSE](LICENSE) for details.

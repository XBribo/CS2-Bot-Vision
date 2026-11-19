<div align="center">

# BotVision

<p>
  <a href="https://github.com/XBribo/CS2-Bot-Vision/releases/latest"><img src="https://img.shields.io/github/v/release/XBribo/CS2-Bot-Vision?style=flat-square" alt="Release"></a>
  <a href="https://github.com/XBribo/CS2-Bot-Vision/releases/latest"><img src="https://img.shields.io/badge/platforms-Windows%20%7C%20Linux-0078D4?style=flat-square" alt="Platforms"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-AGPL--3.0-blue?style=flat-square" alt="License"></a>
</p>

[Quick Start](#quick-start) · [Configuration](#configuration) · [Commands](#commands) · [Developer Guide](TECH.md)

</div>

Let CS2 bots see through gaps that HE grenades and bullets open in smoke.

<details>
<summary>Signature Status</summary>

[![Signature Status](https://raw.githubusercontent.com/XBribo/cs2-bot-signatures/main/.github/docs/summary.svg)](https://github.com/XBribo/cs2-bot-signatures)

</details>

## Quick Start

1. Install Metamod:Source on the server.
2. Download your platform's package from [Releases](https://github.com/XBribo/CS2-Bot-Vision/releases/latest).
3. Copy `addons/` into `game/csgo/` and restart the server.
4. Run `bv_status` to check plugin status.

## Configuration

Edit [`config.json`](configs/addons/BotVision/config.json) in `game/csgo/addons/BotVision/`. Console commands change the current session; use the file for permanent settings.

| Setting | Default | Description |
| --- | --- | --- |
| `smoke.mode` | `0` | `0` uses smoke density; `1` uses the game's standard smoke check |
| `smoke.density_threshold` | `0.23` | In mode `0`, lower values make thinner smoke block a bot’s view |
| `bullet_holes.enabled` | `true` | Let bots see through bullet trails in smoke |

## Commands

| Command | Description |
| --- | --- |
| `bv_status` | Show plugin and smoke status |
| `bv_smoke_mode [0\|1]` | Show or change the smoke mode |
| `bv_density_threshold [value]` | Show or change the global threshold |
| `bv_bot_density [slot [value]]` | Show or change a bot’s threshold; a negative value restores the global setting |
| `bv_bullet_holes [0\|1]` | Show or toggle bullet trails |
| `bv_reveal add <slot>` / `bv_reveal remove <slot>` | Let bots see the player through smoke, or remove that exception |
| `bv_reveal list` / `bv_reveal clear` | List or clear revealed players |

For example, `bv_bot_density 2 0.15` sets the threshold for the bot in slot 2. Use `bv_bot_density 2 -1` to restore the global value.

## Credits

Created by [XBribo](https://github.com/XBribo), with thanks to all contributors.

[![BotVision contributors](https://contrib.rocks/image?repo=XBribo/CS2-Bot-Vision)](https://github.com/XBribo/CS2-Bot-Vision/graphs/contributors)

## License

[GNU Affero General Public License v3.0](LICENSE).

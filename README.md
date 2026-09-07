# Boss Stats Randomizer

Randomizes your character's stats on every boss fight for **Elden Ring 1.16.2**.

> Mod link: https://www.nexusmods.com/games/eldenring/mods/10862

## What it does

Every time you walk into a boss fog gate, your stats are rolled to a new set of
values. The result is fully **deterministic**: the same game seed + the same boss
always gives the same stat loadout, so it works like a seeded speedrun modifier,
not a roll of the dice.

When the boss fight ends, your original stats are restored automatically. If you
quit mid-fight, the cache file lets a later launch pick the restore back up.

## Features

- **Stat randomization** (core): Vigor / Mind / Endurance / Strength / Dexterity /
  Intelligence / Faith / Arcane are rerolled per boss between configurable
  min/max limits, with optional per-attribute caps.
- **HP / FP refill**: your health and focus are topped up when stats change so a
  low roll never oneshots you out of the gate.
- **Optional: flask charges + flask power** — reroll flask count and the upgrade
  level each fight (restored after).
- **Optional: Wondrous Physick** — each boss gets a random mix of crystal tears,
  drawn from the full base-game pool (or including the 8 Shadow of the Erdtree
  tears).

All optional randomizations are **off by default** — the mod only does what you
turn on.

## Requirements

- Elden Ring **1.16.2**
- A DLL plugin loader / mod loader that loads this into the game process

## Installation

1. Extract the archive.
2. Copy `BossStatsRandomizer.dll` and the `BossStatsRandomizer` folder into your
   mod loader's `mods` directory (for Mod Engine 2: `<game>/mods/`).
3. Start the game. `BossStatsRandomizer.ini` is generated next to the log on first
   launch with sensible defaults.

## Configuration

Everything lives in `mods\BossStatsRandomizer\BossStatsRandomizer.ini`.

| Section | Key | Default | Notes |
| --- | --- | --- | --- |
| General | `GlobalSeed` | random on first launch | change to re-seed every boss |
| General | `LogLevel` | 1 | 0 = quiet, 1 = debug |
| General | `ShowConsole` | 0 | 1 = live log console window |
| Gate Detection | `EnableBossEncounterDetection` | 1 | fire randomization on fog-gate entry |
| Stat Randomization | `EnableRuntimeApplication` | 1 | master switch for randomization |
| Stat Randomization | `CacheInitialStats` | 1 | restore originals when the fight ends |
| Stat Randomization | `MinAttribute` / `MaxAttribute` | 1 / 99 | global stat range |
| Stat Randomization | `MaxVigor` … `MaxArcane` | 99 | per-attribute caps (99 = none) |
| Flask Randomization | `RandomizeFlaskCharges` | 0 | optional, off by default |
| Flask Randomization | `RandomizeFlaskLevel` | 0 | optional, off by default |
| Flask Randomization | `Min/MaxFlaskCharges` | 2 / 14 | combined HP+FP total |
| Flask Randomization | `Min/MaxFlaskLevel` | 1 / 12 | flask power ("sacred tear") level |
| Wondrous Physick | `RandomizeWondrousPhysick` | 0 | optional, off by default |
| Wondrous Physick | `WondrousPhysickIncludeDlc` | 0 | include SotE crystal tears |
| Healing | `FillVigorOnRandomize` / `FillFpOnRandomize` | 1 | top HP / FP up on change |

## How the randomization is seeded

The outcome for a given boss is `f(GlobalSeed, boss)`. Re-entering the same fog
gate — even on a new launch — yields the same result, as long as the seed is
unchanged.

## Uninstall

Remove `BossStatsRandomizer.dll` and the `BossStatsRandomizer` folder from your
mod loader's `mods` directory. Because restored/cached stats live in the same
cache file, fights you finish (or restore) before removing the mod leave your
character exactly as it was.

## Compatibility / disclaimer

- Built and tested against **Elden Ring 1.16.2** only. It may break on any other
  version.
- Modify stats and inventory in memory only during fights; attributes you change
  elsewhere (e.g. with a stat editor) are preserved.
- Use at your own risk in online play. This is a single-player convenience mod.

## Changelog

**1.0.0**
- Initial release.
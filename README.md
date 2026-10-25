# NFSMW Recompiled

[![Discord](https://img.shields.io/badge/Discord-Join%20the%20server-5865F2?logo=discord&logoColor=white)](https://discord.gg/THtWjtCtNx)

A native PC port of the Xbox 360 version of **Need for Speed: Most Wanted** (2005), made by static recompilation:
the game's PowerPC code is translated to C++ ahead of time and runs natively, with a Vulkan renderer that draws the
game directly instead of emulating the Xbox 360 GPU.

> This repository contains no game data and no code generated from the game. You need your own copy of the game.

## Supported editions

| Edition | Status |
|---|---|
| PAL, Spanish (`default.xex` SHA-256 `aad15fc2…`) | Supported |
| USA (`default.xex` SHA-256 `aebdf3c1…`) | Supported |
| Other editions | Not yet |

Both work from the same download: pick your `.iso` or game folder in the launcher, and it detects the edition and
starts the right executable.

## Features

- **Native Vulkan renderer**, much faster than GPU emulation.
- **Launcher** built into the game window, in English, Spanish and Portuguese:
  - pick the game as an `.iso` or an extracted folder;
  - generates the shader library (`nfsmw_shaders.nfsp`) from your own disc;
  - quality profiles (Low, Medium, High, Ultra) and a one-click "Xbox 360 defaults";
  - internal resolution up to your screen's (1440p and 4K are experimental), FPS limit 30/60/120/unlimited
    (the last two are experimental), shadows, reflections, anti-aliasing, anisotropic filtering, post-processing;
  - key remapping, controller button remapping and steering wheel support (axes, pedals, buttons);
  - save data manager (export, import, restore the previous save, delete);
  - cheats loaded from `nfsmw_trucos.txt`;
  - notifies when a new version is available.

## Playing

1. Download the release and unzip it anywhere.
2. Run `nfsmw.exe`.
3. In the launcher, choose your `.iso` (it is extracted once) or a folder with the extracted game.
4. Press **Play**. The first time, the launcher builds the shader library from your disc.

Settings are stored in `nfsmw.toml`, saves in the user folder, and a readable copy of each save in `saves/`.

## Building

See [docs/building.md](docs/building.md).

## License

GPL-3.0, see [LICENSE](LICENSE). This project is derived from other open source projects; see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

Need for Speed is a trademark of Electronic Arts Inc. This project is not affiliated with or endorsed by Electronic
Arts or Microsoft.

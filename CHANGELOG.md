# Changelog

Format based on [Keep a Changelog](https://keepachangelog.com/es-ES/1.1.0/).

## [0.0.2] - 2026-09-17

### Added

- The launcher accepts an `.iso` directly: it extracts it by itself the first time to
  `game_root_cache\` inside the portable folder and reuses that copy afterwards. Before,
  the only option was to point it to an already extracted folder (`--game_data_root`
  requires a directory, the SDK doesn't know how to mount `.iso`).
- Launcher window resizable and scrollable: the cover strip narrows on small screens
  instead of forcing horizontal scroll, and the settings center on wide screens instead
  of staying stuck to one side with a huge gap.
- Launcher settings in two columns instead of a long list.
- Dark theme for the launcher.

### Changed

- The launcher now starts at 1080p + x2 scale by default instead of 720p + x1 on a fresh
  install (no `lanzador.json` yet) — it matches what `nfsmw.toml` already ships
  configured by default, instead of starting lower than that with nobody asking for it.
- The launcher's cover fills the entire panel ("cover", not "fit"): before it left an
  empty black stretch at the bottom at tall window proportions.
- The game is launched with a higher process priority.

### Fixed

- Launcher window marked DPI-aware: on monitors with Windows scaling (125%, 150%...) it
  came out blurry due to Windows bitmap-stretch; now crisp.
- "Duplicated banner" when enlarging the launcher window: `ControlStyles.ResizeRedraw`
  was missing on the cover panel, so when the control grew only the newly exposed strip
  was invalidated and the old crop stayed underneath.
- The portable folder's `nfsmw.toml` had lost the resolution section
  (`video_mode_width`/`video_mode_height`/`resolution_scale`) when restoring an earlier
  backup; restored so it matches the repo's `app/nfsmw.toml`.

## [0.0.1] - 2026-09-10

First tidy version of the project. Everything below was done before this repository
existed; it's recorded here because it's the state it starts from.

### Added

- Full static recompilation of NFS Most Wanted (2005, Xbox 360, `454107D9`) that boots,
  gets through the prologue and reaches open world.
- `parche_desatasco.py`: fixes the XMA decoder hang that killed the audio when leaving
  the garage and froze the game when returning to the menu.
- `parche_presentador.py`: real vsync and fps limiter. Neither existed in the SDK.
- `parche_backend.py`: graphics API selector (D3D12 / Vulkan) from the F4 menu, with
  automatic fallback if the chosen one isn't compiled.
- `parche_velocidad.py`: game speed adjustable as a percentage, 0–200%.
- `parche_restaurar.py`: F4 menu improvements — pending-restart warning with a restart
  button, restore-startup-config button, sliders with limits for decimal settings, and
  the graphics API in use on display.
- `parche_gpu_fallback.py`: fallback to WARP if the D3D12 device can't be created.
- `parche_privilegios.py`: `grant_user_privileges` setting to get past the Xbox Live
  privileges gate. Off by default.
- Native C#/WinForms launcher with the cover beside it, built with the `csc.exe` Windows
  already ships. It shares settings with the old PowerShell launcher.
- Internal resolution scaling up to x4 from the launcher.

### Changed

- In the portable folder, `NFS_Most_Wanted.exe` becomes **the launcher** and the game is
  called `nfsmw.exe`, so the game icon opens the options window.
- The EDRAM's RTV path is the one selected by default: it almost doubles the fps on
  integrated graphics compared to ROV.
- The launcher settings are called "Window size" and "Internal resolution", which is what
  they do. They used to be "Output resolution" and "Render scale" and got confused.

### Fixed

- The F4 menu no longer always opens with a false "restart needed" warning.
- Patches no longer duplicate when run twice.
- `comprobar_dist.ps1` recognizes `mscoree.dll` as a system DLL, and no longer considers
  a folder containing the .NET launcher broken.

### Unresolved

- Vulkan renders black on Intel.
- Horizontal band with the RTV path on some integrated GPUs.
- Multiplayer: 114 of the SDK's 158 network functions are missing, including the System
  Link ones, and the session handlers are stubs. See
  [docs/diario/red-y-privilegios.md](docs/diario/red-y-privilegios.md).

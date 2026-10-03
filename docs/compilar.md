# Building from scratch

From a clean clone to a playable folder.

## What you need

| Thing | Why |
|---|---|
| Windows 10 or 11 x64 | The graphics backend is Direct3D 12 |
| Visual Studio 2022 Build Tools | MSVC and the Windows SDK. The IDE isn't needed |
| CMake 3.28+ and Ninja | The SDK and the application use presets |
| Clang 20+ | The generated C++ doesn't compile with MSVC |
| Python 3.10+ | The patches and the tools |
| The ReXGlue SDK | Cloned next to it, in `..\rexglue-sdk` |
| Your own ISO or GOD dump | The game. It isn't here and it won't be |

Environment setup details in [00-entorno.md](00-entorno.md).

## The expected structure

The scripts look for the SDK **next to** the project, not inside it:

```
Documents\
├── NFSMW Recompiled\     ← this repository
└── rexglue-sdk\          ← the SDK, cloned separately
```

If you have it elsewhere, the patches also look in `.\sdk`.

## The steps

### 1. The SDK

```powershell
.\tools\bootstrap.ps1
```

It checks the prerequisites, clones the SDK into `..\rexglue-sdk` and builds and
installs it.

### 2. Your `default.xex`

```bat
EXTRAER_XEX.bat
```

It extracts `default.xex` from your ISO and leaves it in `assets\`. That folder is in
`.gitignore` and that's where it stays.

Details and alternatives (GOD, XContent) in
[01-extraccion-xex.md](01-extraccion-xex.md).

### 3. Build

```bat
CONSTRUIR.bat
```

That's all. Internally it runs five phases:

1. **SDK patches.** Applies the project's nine patches to `..\rexglue-sdk`.
   Order matters: `parche_anillo` goes before `parche_desatasco`.
2. **Rebuild the SDK.** This is where the fixes end up, inside
   `rexruntime.dll`. It's configured with Vulkan enabled so the API selector
   has two real options.
3. **Generate and build the game.** Two ninja passes, and it's not a whim: see below.
4. **Assemble `build\`.** Copies the executable, the DLLs and the support files, and
   deletes leftovers from previous runs. Then it builds the launcher and renames the
   game.
5. **Check the folder is self-contained.** Reads the PE import table of every binary
   and follows the dependencies in a chain, to make sure no DLL is missing.

And when it finishes it assembles the distribution folders in `..\build release\`, so
there's no need to remember a manual step. This used to be "zip `build\` without the
ISO", and that step had a trap: the ISO is several GB and it's easy to send it by
mistake.

- `NFSMW Windows x64\` — playable, with the executable inside. Zip it and send it to
  someone who has their own copy. **Not published.**
- `NFSMW Windows x64 - Portable\` — everything except the game. This one is.

It takes a long while the first time: that's 131 generated C++ files, over a million
lines.

### 4. Play

Copy your ISO into `build\` and open `build\NFS_Most_Wanted.exe`.

That's **the launcher**, with the game's icon. The actual game is `nfsmw.exe`.
The name swap is so that double-clicking the icon brings up the options window;
see [lanzador.md](lanzador.md).

If you name your ISO `nfsmw.iso` it'll be the preferred one when there are several.

## Why two build passes

The generator rewrites `generated\default\nfsmw_pch.h`, and the precompiled header
used by the 131 generated files comes from that header.

In a single pass, ninja decides at startup which files are dirty. At that moment
`nfsmw_pch.h` hasn't changed yet, so it takes the precompiled header as good. Then,
already within the same pass, the generator changes it. When the `.cpp` files' turn
comes, clang compares and aborts:

```
fatal error: file 'nfsmw_pch.h' has been modified since the precompiled header was
built: size changed (was 18553, now 18522)
```

By launching the generator first and separately, the second pass starts with the final
headers.

## When something fails

### The SDK doesn't link

Most likely a patch is half-applied. Check the state:

```bat
for %f in (tools\parche_*.py) do python %f --estado
```

And if needed, revert them all and start over:

```bat
for %f in (tools\parche_*.py) do python %f --revertir
```

### A patch says the anchor doesn't appear exactly once

The SDK has changed from what the patch expects. The script hasn't touched anything.
You have to look at the block by hand and update the patch; see [parches.md](parches.md).

### `RC` and `vcvars64`

In this project's `.bat` files, return codes always go in a variable called `SALIDA`,
**never** `RC`. `vcvars64` sets `RC` to the resource compiler path and CMake reads it
when detecting the toolchain. Using `RC` for anything else breaks the configuration in
a hard-to-see way.

### The game boots but looks wrong

Before suspecting the code, look at `build\nfsmw.toml`. Settings saved from the F4 menu
end up there, and there are two debug ones that wreck the render without throwing
errors. See [problemas-conocidos.md](problemas-conocidos.md).

### Out of space or out of patience

The generated tree takes several GB. `app\generated\` can be deleted entirely: it gets
regenerated.

## Building only the launcher

```bat
CONSTRUIR_LANZADOR.bat
```

It uses the `csc.exe` Windows already ships inside `C:\Windows\Microsoft.NET\`. Nothing
needs to be installed. See [lanzador.md](lanzador.md).

## Rebuilding after touching a patch

No need to rebuild the game: the patches only touch the SDK.

```bat
python tools\parche_loquesea.py
cd ..\rexglue-sdk
cmake --build out/build/win-amd64 --config Release --target install
```

And copy the new `rexruntime.dll` to `build\`. `CONSTRUIR.bat` does all that, but if you
only changed a patch, this is much faster.

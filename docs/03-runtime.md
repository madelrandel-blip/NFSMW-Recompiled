# Phases 3 and 4 — Runtime, stubs and the long cycle

This is where the real work lives. Codegen is a weekend; this is months.

## What ReXGlue gives you

The SDK ships a runtime derived from Xenia:

- **Memory** — the guest memory map (base `0x82000000`), heaps, protections
- **Kernel State & Objects** — threads, events, mutexes, semaphores, TLS
- **Virtual File System** — maps the guest paths (`game:\`, `d:\`) to your disk
- **ReXApp** — the host application framework: window, main loop, presentation
- **CVar System** — runtime configuration variables
- **Logging** — essential; turn it up to `trace` when something breaks

What **you** provide: the kernel imports this game uses that aren't implemented, the
shaders, and all the NFSMW-specific patches.

## The cycle

```
compile → run → crash/hang → read the log → identify what's missing → implement → repeat
```

Always start with high logging the first time:

```bash
./app/build/nfsmw --log_level trace --log_file run.log
```

The first boot is going to die quickly. Almost always with something like:

```
[error] unimplemented kernel import: XamUserGetSigninState (ordinal 0x0000014B)
```

## Implementing a missing import

Unresolved imports are declared as stubs. Start with the dumbest version that lets
the game move forward:

```cpp
// app/src/stubs/xam.cpp
uint32_t XamUserGetSigninState(uint32_t user_index) {
    // 1 = signed in locally. Enough to pass the profile check.
    return user_index == 0 ? 1 : 0;
}
```

Rule of thumb: **return the minimum that doesn't break**, not the correct thing. If
the game asks about Xbox Live status, say there's no connection. If it asks about
achievements, return an empty list. You'll come back to it. What you can't do is
return garbage: an invalid handle treated as a pointer gives you a crash 40 frames
later, impossible to trace back to here.

Mark every stub:
```cpp
// STUB: always returns "no connection". Check whether career mode queries it.
```
and keep a `STUBS.md` with the list. You're going to accumulate hundreds.

## Mid-ASM hooks: the scalpel

When you need to intervene in the middle of a game function without rewriting it —
skip a check, force a value, instrument — you use an instruction-level hook:

```toml
[[midasm_hook]]
address           = 0x8214C880   # the exact instruction to intercept
name              = "SkipDiscCheck"
registers         = ["r3", "r4"]
after_instruction = false
return_on_true    = true
```

```cpp
bool SkipDiscCheck(PPCRegister& r3, PPCRegister& r4) {
    if (r3.u32 == 0) { r3.u32 = 1; return true; }  // return from the guest function
    return false;                                   // continue normally
}
```

Typical uses in a racing game:
- disable the disc / DVD region check
- force a resolution or aspect ratio other than the fixed 720p
- unlock the framerate (NFSMW 2005 is locked to 30)
- skip the EA intro without touching the assets

Hooks are the preferred tool over patching the binary: they live in the TOML, they're
versionable, and you don't distribute anything from the game.

## Graphics

The game emits Xenos shader microcode and ring buffer commands. You need to
translate it. Two paths:

1. **XenosRecomp** (from the same people as XenonRecomp) — recompiles the shaders to
   HLSL/SPIR-V ahead-of-time. It's what Unleashed Recompiled used.
2. Whatever ReXGlue brings in its graphics layer — check `Runtime Architecture
   Overview` in the wiki, which is evolving fast.

NFSMW 2005-specific things that will give you trouble:
- **Motion blur and bloom**: use render targets with formats that have no direct
  equivalent in D3D12/Vulkan. You'll need manual conversion.
- **EDRAM tiling**: the 360 resolves from EDRAM with predicated tiling. It has to be
  emulated as render passes.
- **Real-time car reflections**: dynamic cubemaps, sensitive to command order.

Advice: don't chase fidelity at first. A frame that draws *something* recognizable is
already a huge milestone. Geometry first, then textures, then post-processing.

## Audio

The weakest point of the stack. The 360's XMA decoder is an MMIO block, and both
XenonRecomp and ReXGlue have it incomplete. Options:

- Use Xenia's XMA decoder (portable code, there are FFmpeg-based implementations).
- Silent stub at first: return zeroed buffers. The game boots, you make progress,
  and you come back to audio when everything else works.

NFSMW 2005 mixes XMA (music, the licensed soundtrack) with ADPCM and engine streams.
Engine effects are procedural over short samples — those tend to be easier than the
music.

## Input

`XamInputGetState` mapped to SDL2/XInput. It's one of the most rewarding: a modern
Xbox controller maps 1:1 to the 360's. It usually works almost first try.

## Assets

The game looks for its files in guest paths. Configure the VFS so `game:\` points to
the folder where you extracted the ISO:

```
assets/game_root/
├── FRONTEND/
├── CARS/
├── TRACKS/
└── SOUND/
```

If the game hangs while reading, turn the VFS log up to `trace` and see which exact
path it's asking for. It's almost always a case-sensitivity issue (the 360 is
case-insensitive, Linux isn't) or a `\` vs `/` separator issue.

## Realistic milestones, in order

1. Codegen finishes without errors
2. The binary compiles and links
3. It boots and reaches the game's `main` without crashing
4. The VFS resolves the first file
5. First frame drawn (even if it's black with a triangle)
6. EA logo / loading screen visible
7. Main menu navigable with a controller
8. A race loads
9. A race is playable
10. Audio
11. Everything else

Half of the total effort goes between 3 and 5. It's normal to get stuck there for
weeks.

## How to ask for help

When you get stuck, the useful things you can share without distributing anything
from the game:
- the `codegen.log` with the error
- the relevant lines from `run.log`
- the TOML snippet you're trying
- the address and disassembly of the problematic function

The hedge-dev Discord (XenonRecomp / Unleashed Recompiled) and the ReXGlue repo are
where the people who know are.

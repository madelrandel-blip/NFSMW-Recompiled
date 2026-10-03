# The patches

Catalog of what this project changes in the SDK, why, and how it was verified.

All of them are applied to the `..\rexglue-sdk` source before building it. None of them
touch the game code.

## How they're used

```bat
python tools\parche_velocidad.py              apply
python tools\parche_velocidad.py --estado     show what's applied
python tools\parche_velocidad.py --revertir   undo
```

`CONSTRUIR.bat` applies them all in the correct order. Order matters in one case:
`parche_anillo.py` goes before `parche_desatasco.py`, because the second relies on the
headers (`<atomic>`, `<chrono>`) the first adds. The desatasco script refuses to apply
if the other hasn't run.

## The catalog

### `parche_desatasco.py` — the audio hang

**The important one.** Without it, after the prologue, when you leave the garage the
audio dies and the game freezes when you return to the menu.

It touches `xboxkrnl_audio_xma.cpp`. When an XMA context has been spinning for more than
250 ms with no input and with the read pointer stuck to the write pointer, it gives it
the "buffer finished" signal that the game itself knows how to interpret.

It's a deliberate hack: it breaks the deadlock instead of avoiding it, and it can cost
an audio stumble on that voice. The full story —and why the "obvious" fix was the wrong
one— is in [diario/audio-cuelgue.md](diario/audio-cuelgue.md).

**Verified:** the user played the zone that reproduced it without it hanging.

### `parche_anillo.py` — XMA instrumentation

Prerequisite for the previous one. Adds traces to the XMA kernel functions. With the
normal log level it prints nothing.

One detail that cost an afternoon: the *getters* are limited to one trace per second,
but the *setters* aren't. Limiting both equally hid exactly what needed to be seen —the
input submissions— and the diagnosis went down the wrong path.

### `parche_presentador.py` — vsync and fps limit

Out of the box **neither works**:

- `vsync` exists as a cvar but doesn't sync anything. It's read in a single place and
  only decides whether the command processor sleeps or spins during guest waits. The
  D3D12 presenter's `Present` had `SyncInterval` pinned to 0.
- There was no fps limiter at all. None.

This patch fixes both. The launcher shows a red warning if it isn't applied.

### `parche_gpu_fallback.py` — don't die without a GPU

If the D3D12 device can't be created, it falls back to WARP instead of showing an error
screen. Useful on machines without decent drivers.

### `parche_backend.py` — graphics API selector

Adds the `gpu_backend` cvar (`d3d12` / `vulkan`) and passes it to the plugin loader.

The plugin already knew how to choose by name; all that was missing was someone telling
it. `rex_app.cpp` called `LoadGpuPlugin` with a single argument, so it always came out as
`any`, which in practice is D3D12 for being first in the `if`.

Three things this patch learned the hard way:

- It's `kRequiresRestart`, not `kInitOnly`. With `kInitOnly` the menu drew it in red and
  it couldn't be touched.
- **There's no `any` option.** With `any` you couldn't tell which one was actually set,
  which was exactly what needed to be shown.
- It clears the pending-restart list when startup finishes, because otherwise the menu
  opened with a permanent false warning. See [arquitectura.md](arquitectura.md).

If the chosen API isn't compiled, it falls back to the other one and says so in the log
instead of failing to start.

**Verified:** three identical runs in a row, reverting and reapplying gives the same
result, with no leftovers from previous versions.

### `parche_restaurar.py` — the F4 menu

Five blocks in `settings_overlay.cpp`:

- Pending-restart warning, with a button to save and restart. The SDK already kept the
  count (`GetPendingRestartFlags`) but didn't show it anywhere, so changing the graphics
  API seemed to do nothing.
- Snapshot of the startup configuration, so you can go back to it.
- "Restore defaults" button that restores **that** snapshot, not the SDK's factory
  values.
- Slider for decimal settings with limits, instead of a text box.
- The graphics API in use, read from the cvar registry.

That last point has an expensive lesson behind it. The first version used a global
variable shared with `rex_app.cpp` and **didn't link**:

```
lld-link: error: undefined symbol: rex::ui::g_gpu_backend_en_uso
```

`rex_app.cpp` isn't compiled into the SDK: it's **installed as source** in
`share/rexglue/` and compiled by each application. So the definition ended up inside
`nfsmw.exe` and the reference inside `rexruntime.dll`. To talk between modules there's
the cvar registry.

### `parche_velocidad.py` — game speed

Adds `game_speed`, as a percentage, from 0 to 200. It's not an fps limit: it changes the
rate at which time passes inside the game.

As a percentage and not a multiplier because the F4 window shows a bare number and "1.0"
doesn't say what of. With a floor at 0.1% because a literal zero doesn't hang the game:
it crashes it, due to the division in `RecomputeGuestTickScalar`.

### `parche_privilegios.py` — the multiplayer gate

Out of the box `XamUserCheckPrivilege` denies **all** privileges, always. The original
comment says it: *"If we deny everything, games should hopefully not try to do stuff"*.
In Most Wanted the effect is the "The privileges you have on Xbox Live don't allow you
to access this feature" message.

It adds the `grant_user_privileges` setting, **off by default**. Turned on, it answers
yes to everything.

It opens the menu gate and nothing more. What's behind it doesn't work; see
[diario/red-y-privilegios.md](diario/red-y-privilegios.md).

### `parche_diagnostico.py` — startup traces

General instrumentation that stayed because it's cheap and useful. Among other things,
it's what gave the audio hang a name and a timestamp.

### `parche_fpu.py` — FP exception masks (USA)

The recompiled guest runs host floating-point code. If anything on the guest thread
(a host library, FFmpeg/XMA, ...) leaves the host MXCSR with the FP exception masks
cleared, the next FP instruction raises a hardware exception. The SEH filter does not
cover `0xC000008F` (float inexact), so the process dies with no log line.

That is exactly how the first USA builds died: Windows Error Reporting showed
exception `0xc000008f` inside a generated function and the log simply stopped. The
patch makes every write to MXCSR keep the exception masks set, and makes the
flush-mode helpers rewrite MXCSR unconditionally, so a dirty control word is repaired
before the FP code runs.

### `tools/diagnostico/parche_xma.py` — heavy XMA instrumentation

**Outside the default build.** Per-second tracing of the audio thread, every submission
and every silence. Applied by hand to investigate and reverted afterwards.

---

## Why the patches are written this way

It's not a whim. Every rule comes from a specific failure.

### Exact text replacement, no `.original`

The first version saved a copy of the file before touching it. It stops working as soon
as **two patches touch the same file**: the second saves as "original" a file that was
already patched, and reverting leaves the tree in a state that's neither before nor
after.

Now each patch applies and reverts by text, and saves nothing.

### Block by block, not one marker per file

There was a version with a single marker per file: if it was there, the patch counted as
applied. The day a new block was added to an already-applied patch, it **did nothing**
and said nothing. The symptom was *"I opened the exe and it didn't have the slider to
change the speed"*.

Now each block is checked and applied separately.

### They refuse to write if an anchor doesn't appear exactly once

A half-applied patch is worse than one that fails. If the SDK changed and the anchor is
no longer there, or is there twice, the script exits without touching anything.

### The migration rule, which took three attempts

When you change a patch that was already applied somewhere, you have to remove the old
version before putting in the new one. And there are two symmetric traps there:

- **The old block is a piece of the new one** (code was added to it). Searching for the
  old one finds it *inside* the good one, and replacing it with the anchor cuts the head
  off the freshly placed block. Then it gets reapplied and the tail ends up
  **duplicated**. The file grew on every pass.
- **The new block is a piece of the old one** (code was removed from it). Then "the good
  block is there" says yes even though what's there is still the entire old one, and the
  script considers itself applied while leaving dead code inside.

I tried to solve it with a per-version *fingerprint*: a piece that was only in that
version. It doesn't always exist: when the old one is an exact prefix of the new one,
everything in the old one is also in the new one.

The rule that does work needs no fingerprints:

```python
es_de_verdad_vieja = (viejo in txt) and (viejo not in nuevo or nuevo not in txt)
```

Both cases come out right with that, and it's checked using only the texts.

**And it's tested by running the patch twice in a row and comparing.** The duplication
failure isn't visible on the first pass, which is the only one people usually look at.

### Idempotence

A consequence of the above, but it deserves saying separately: running a patch N times
has to give the same file as running it once. Otherwise `CONSTRUIR.bat` corrupts the
tree a little more on every rebuild.

### A Windows detail

The patches read and write in text mode. On Windows that preserves CRLFs; on Linux it
converts them to LF on the first write. If you compare results across platforms,
normalize the line endings before shouting.

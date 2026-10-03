# Known issues

What's broken, and how far the investigation into each thing got. A problem with a
half-finished diagnosis is worth more than one not started.

## Open

### Vulkan renders black (Intel)

**Status:** reproducible, undiagnosed.

The Vulkan backend is complete in the SDK and compiles. It loads fine —so fine that the
automatic fallback doesn't trigger, because that only kicks in when the API doesn't even
exist in the copy— and the screen comes out black.

The next step would be to look at that run's log with `--log_level=debug` to see what it
says before going black. It hasn't been done.

In the meantime: the launcher always passes `--gpu_backend`, so choosing wrong here never
leaves the game unable to open. You go back to the launcher and select DirectX 12.

### Horizontal band with the RTV path

**Status:** seen, not narrowed down.

On some integrated GPUs the fast EDRAM path leaves a strange horizontal band. Since it
costs half the fps, it's worth investigating before giving up.

Not checked yet: whether it depends on the Intel driver version, and whether it
reproduces on other integrated GPUs or only on the Iris 540.

### Multiplayer

**Status:** thoroughly diagnosed, not implemented.

The privileges gate is solved. Below it, 114 of 158 network functions are missing,
including the System Link ones, and the session handlers are stubs that return success
without doing anything.

The full detail, with the table of what's missing, is in
[diario/red-y-privilegios.md](diario/red-y-privilegios.md).

### Too few cores

The SDK warns at startup:

```
Too few processor cores - scheduling will be wonky
```

It's not decorative. On machines with few cores the audio thread competes worse and the
XMA deadlock is more likely. If many `[desatasco]` lines show up in the log, this is why.

## Resolved, documented in case they come back

### Audio died and the game froze

Fixed by `parche_desatasco.py`. The full story, including the fix that looked obvious and
was wrong, in [diario/audio-cuelgue.md](diario/audio-cuelgue.md).

### Broken green screen that looked like a backend bug

**It wasn't the code.** It was two debug switches that had slipped into `nfsmw.toml` from
the F4 menu:

```toml
d3d12_tessellation_wireframe = true
native_stencil_value_output_d3d12_intel = true
```

The first draws tessellated geometry as wireframe. The second forces native stencil
output **on Intel**, which is exactly the case the SDK excludes on purpose. With the RTV
path it wrecks the render without giving a single GPU error.

**Lesson: if it suddenly looks wrong, check the toml before suspecting the code.**

### `NtCreateFile FAILED` in the log

43 warnings for game files that don't open, with `0xc000000f`. **It's normal.** The game
probes files that don't exist on this disc. It was confirmed by comparing with a long run
that made it to the end: exactly the same 43 show up.

Don't chase this.

### The permanent "restart needed" warning

The F4 menu always opened saying `Restart needed to apply: gpu_backend`, even if you
hadn't touched anything.

`SetFlagFromSource` records any `kRequiresRestart` cvar that's touched into the pending
list, without looking at where the value comes from. Since the launcher always passes
`--gpu_backend`, it entered the list despite already being applied. A warning that can't
be dismissed stops being read, and then it isn't read when it's real either.

`parche_backend.py` clears the list when startup finishes.

### Raising the resolution changed nothing

It wasn't a bug: they were two different controls with similar names. `--resolution` only
enlarges the image; the one that makes it finer is `--resolution_scale`. The launcher now
calls them "Window size" and "Internal resolution", and shows what the chosen scale does.

See [rendimiento.md](rendimiento.md).

## Things it's best not to try again

**Reserving a block in the XMA ring** to disambiguate full/empty. It looks like the
textbook fix and it's wrong: `output_buffer_valid = 0` with the ring full is the signal
the game uses to know the buffer finished. Taking it away removes its only exit.

**Looking for a DirectX 11 backend.** It doesn't exist in this SDK and it's not an
oversight: the Xenos emulation relies on DX12-generation features —rasterizer ordered
views, unbounded descriptors, typed writes from shaders for memexport—. A DX11 backend
isn't a setting, it's redoing the GPU plugin. And it wouldn't fix anything: the
bottleneck is the GPU at 100%, and the API doesn't change how many pixels have to be
shaded.

**Chasing EA's servers.** They're shut down. The only path to multiplayer is System Link.

# The launcher

A native Windows window in C# with WinForms, with the game cover next to it,
installer-style. It's what opens to play.

## Building it

```bat
CONSTRUIR_LANZADOR.bat
```

It uses the `csc.exe` from the .NET Framework that **already ships with Windows**, in
`C:\Windows\Microsoft.NET\Framework64\v4.0.30319\`. No Visual Studio, no .NET SDK,
nothing needed.

C# was chosen over the alternatives for that reason:

- **Raw Win32 C++**: you get a small exe, but hand-building a window with twenty
  controls is a ton of code for what it is.
- **Packaged Python**: you have to install Python and PyInstaller, and the exe ends up
  weighing 30 MB.
- **C# with the compiler Windows already ships**: a single file, the same controls the
  PowerShell launcher already used —WinForms is what was underneath—, icon and cover
  inside the exe, and zero installs.

### It builds with an old csc

The one Windows ships is C# 5 (2012). In `Lanzador.cs` you **cannot** use anything
modern: no interpolated strings `$"..."`, no `?.`, no `nameof`, no expression-bodied
members `=>`. Everything with `string.Format` and classic syntax.

If any of that slips in, the error you get doesn't say "you need a newer compiler": it
says strange things about missing `;`, and you lose half an afternoon.

## The names, which are swapped on purpose

In the portable folder:

| File | What it is |
|---|---|
| `NFS_Most_Wanted.exe` | **The launcher**, with the game's icon |
| `nfsmw.exe` | The actual game |

The only reason is so that double-clicking the game icon brings up the options window,
like any game with a launcher.

**The game still knows how to start on its own.** `nfsmw.exe` by itself works:
`nfsmw_app.h` sets `gpu_plugin`, `mnk_mode` and `readback_resolve` if nobody asked for
them, and looks for an ISO in its folder. It remains as a fallback if the launcher gives
you trouble.

Two consequences of the rename:

- The ISO finder prefers the one named the **same as the executable**. After the rename,
  an `NFS_Most_Wanted.iso` stops being the preferred one and falls under the second rule
  (the first alphabetically). With a single ISO it doesn't matter. Name it `nfsmw.iso`
  if you want it to be preferred again.
- **It doesn't change where the game stores its stuff.** That folder comes from
  `GetUserFolder() / GetName()`, and `GetName()` is in the code, not the file name. The
  shader cache is still in `Documents\nfsmw\cache`.

## The settings

Everything is saved in `lanzador.json`, in the same folder. The old PowerShell launcher
reads and writes the same file with the same field names, so they coexist.

| Group | What |
|---|---|
| Game image | The ISO |
| Display and resolution | Window size, internal resolution, windowed or fullscreen |
| Frames | Vsync and fps limit |
| Video engine | Automatic / Fast (rtv) / Exact (rov) |
| Graphics API | DirectX 12 / Vulkan |

### Why the graphics API has no "automatic"

It's an escape hatch, and that's why it differs from the rest.

`gpu_backend` can also be changed from the F4 menu. The problem: if you pick an API that
gives a black screen on your machine, save and restart, the value stays written in
`nfsmw.toml` and **there's no way back** — you need the menu to change it, and you need
to see something to reach the menu. It happened for real.

What fixes it is the SDK's cvar priority order: the command line beats the config file.
So the launcher **always** passes `--gpu_backend`, even if it matches the toml. The
window always wins.

An "automatic" that passed nothing would hand control back to the toml, which is exactly
the hole. Under "Video engine" it does make sense, because choosing wrong there doesn't
leave the game invisible.

### The two resolution settings

They used to be called "Output resolution" and "Render scale", and with those names it's
easy to touch the first expecting the second, see that nothing changes and call it
broken. Now:

- **Window size** → `--resolution`. It only enlarges the image.
- **Internal resolution** → `--resolution_scale`. The emulators' "x2".

Below the second there's a line saying what the chosen scale does. It deliberately
doesn't show the resolution in pixels: the scale doesn't multiply the window size, but
the game's render targets, which have a size of their own that the launcher doesn't
know. Writing "2560 x 1440" would be making it up.

### Fixed arguments

They're always passed, and they're not preferences:

| Argument | Why |
|---|---|
| `--readback_resolve=fast` | Without this the image comes out washed out and the sun blown out |
| `--gpu_plugin xenos` | It's the only backend built |
| `--mnk_mode` | Keyboard and mouse in addition to the controller |
| `--gpu_backend=...` | Always; see above |

## The cover

`tools/lanzador/portada.jpg` and `icono.ico` **aren't in the repository**: they're the
game's cover art, Electronic Arts artwork.

The launcher starts fine without them. `CargarRecurso` returns null if the resource
isn't there and the side panel is drawn black with the project title.

If you want to add one:

- `portada.jpg` — drawn whole, stuck to the top of the panel, with the gap below for the
  text. Recommended aspect ratio close to a cover (something like 760×1064).
- `icono.ico` — square, with sizes from 16 to 256.

`CONSTRUIR_LANZADOR.bat` warns if they're missing.

## Implementation details worth knowing

**The game is waited on in another thread.** The PowerShell launcher did `WaitForExit`
on the window thread, and while you played the window hung: Windows painted it white and
marked it as "not responding". Here it's launched separately and control returns with
`Invoke` when it finishes, with a check in front in case you closed the launcher while
playing.

**The json is parsed by hand.** It's ten key/value pairs with no nesting; no need to
bring in Newtonsoft (which would have to be downloaded) or `JavaScriptSerializer` (which
forces a reference to `System.Web.Extensions`). The only delicate part is the
backslashes in Windows paths, which are doubled in json.

**The cover panel is painted by hand**, not with a `PictureBox`, to control how it fits:
the image goes in whole and stuck to the top instead of being cropped at the sides,
which would eat part of the title.

## The old launcher

`tools/lanzador/lanzador.ps1` is the PowerShell version. It does the same thing, shares
the settings and opens with `LANZADOR.bat` in the portable folder. It's there in case
the compiled launcher gives trouble on some machine.

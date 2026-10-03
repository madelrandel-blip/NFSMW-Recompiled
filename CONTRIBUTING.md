# Contributing

Pull requests are welcome. This file is the short version; the detailed conventions
live in [docs/parches.md](docs/parches.md), alongside the source comments.

## Ground rules

**Never commit game data.** Not `default.xex`, not the generated C++, and not the
compiled game executable — it contains the game's own translated code. `.gitignore`
covers the usual paths, but check `git status` before you push. A PR containing any of
these will be closed.

**Never commit the cover art.** `tools/lanzador/portada.jpg` and `icono.ico` are
Electronic Arts' artwork. The launcher builds and runs without them.

## Where fixes belong

Most fixes do not go in this repository. The audio, graphics, input and kernel code all
live in the ReXGlue SDK, and this project changes them through patch scripts in
`tools/`. A fix to emulation behaviour is normally a new or edited `parche_*.py`, not a
change to `app/`.

Changes to `app/` are for things genuinely specific to this game: codegen overrides,
the `ReXApp` subclass, packaging.

If a fix is general enough to help every ReXGlue title, send it upstream to the SDK
instead. This project should carry as little as it can get away with.

## Writing a patch script

Copy an existing one — `tools/parche_velocidad.py` is a good short example. The rules
that matter:

- **Apply and revert by exact text replacement, block by block.** No `.original`
  backups; they go stale the moment a second patch touches the same file.
- **Refuse to write anything unless every anchor matches exactly once.** A patch that
  half-applies is worse than one that fails.
- **Be idempotent.** Running it twice must produce the same file as running it once.
  Test this — the failure mode is a block that duplicates on every run, and it does not
  show up on the first pass.
- **Support `--estado` and `--revertir`.**
- **When you change a patch that shipped, add the old block to its `VIEJOS` list** so
  existing checkouts migrate cleanly instead of ending up with two versions layered on
  top of each other.

`docs/parches.md` explains the migration rule and the three bugs that produced it.

## Comments

Comments in this project explain **why**, not what. If a line looks strange, the
comment should say what happens without it. Several of the patches exist because of a
non-obvious interaction — a scalar that divides by zero, a cvar lifecycle that silently
discards writes, an ordinal table that declares functions nobody implemented — and the
comment is where that knowledge lives.

Source comments are in Spanish. Keep new ones in Spanish for consistency, or write them
in English if that is what you are comfortable with; a correct comment in the wrong
language beats no comment.

## Claiming a change works

Say what evidence says so. "Fixed the stutter" is not reviewable; "the deadlock no
longer reproduces after 20 minutes in the zone that used to trigger it, and the log
shows 3 `[desatasco]` lines instead of the previous hang" is.

For anything touching timing, audio or the GPU, a log excerpt is worth more than a
description.

## Testing before you send

- Run your patch three times in a row against a clean SDK checkout and diff the result
  against a single run. They must be identical.
- Run `--revertir` and confirm the file returns to its original state.
- Build with `CONSTRUIR.bat` end to end at least once.
- If you touched the launcher, build it with `CONSTRUIR_LANZADOR.bat` and open it.

## Reporting a problem

Include the log. `LOG_DETALLADO.bat` runs the game with `--log_level=debug` and
`--log_noisy=true` and writes to `logs\detallado.log`; that file is what makes a report
actionable. Also say which GPU you are on and which EDRAM path and graphics API the
launcher was set to — several problems in this project turned out to be specific to one
combination.

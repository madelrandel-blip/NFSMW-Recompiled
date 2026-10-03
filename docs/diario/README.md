# Technical journal

The investigations that cost time, written down so they don't have to be repeated.

Each entry tells what the evidence said, not what was assumed. Including the reasonable
theories that turned out to be false — those are the ones that save the most, because
they're the ones someone is going to run into again.

| Entry | What it's about |
|---|---|
| [audio-cuelgue.md](audio-cuelgue.md) | The XMA decoder waiting for itself. The obvious fix was the wrong one |
| [red-y-privilegios.md](red-y-privilegios.md) | Why multiplayer doesn't work, with the exact count of what's missing |

## How diagnosis is done here

The pattern that has worked, in order:

1. **Instrument before theorizing.** Diagnostic patches are cheap and the log
   says things that reading the code doesn't.
2. **Beware of your own instrumentation.** Once I limited *all* XMA functions,
   getters and setters, to one trace per second. That hid exactly what needed to be
   seen and the diagnosis went down the wrong path for a good while.
3. **Read the game's code, not just the emulator's.** `codegen.partition.json` maps
   guest addresses to generated files. The audio hang was solved there: the
   emulator's oddity turned out to be a signal the game used on purpose.
4. **Compare against a good run.** The 43 `NtCreateFile FAILED` looked serious
   until they were seen to appear identical in a session that worked.
5. **Suspect configuration before code.** The "broken green screen" was
   two debug switches in the toml.

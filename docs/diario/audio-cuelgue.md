# The audio hang

The project's hardest bug, and the one that best teaches how diagnosis is done here.

**Symptom:** after the prologue, on leaving the garage the audio died. And on returning
to the menu, the game froze completely.

## What it turned out to be

All of Most Wanted's audio is driven by **a single guest thread** (the `0xD`). That same
thread does three things:

1. feeds the XMA decoder with compressed data
2. consumes what comes out
3. mixes the voices

The stall: when that thread goes to mix a voice that has just run out of data, it
starts waiting for audio that only the decoder could produce. And the decoder would
have to be fed by that same thread, as soon as it got out of there. It doesn't get
out. It waits for itself.

## The fix

The game itself has an escape hatch. It can be seen in its recompiled code: when
its read catches up to the write, it asks whether the buffer is still valid
(`XMAIsOutputBufferValid`), and if told no, it considers it finished and moves on.

In the stall it stops **one block** short of catching up, so it never gets to ask.

`parche_desatasco.py` gives it that signal when it has been spinning for more than
250 ms on a voice with no input:

```cpp
const bool atascado = llevo > 250 && context.output_buffer_valid &&
                      !context.input_buffer_0_valid && !context.input_buffer_1_valid &&
                      context.output_buffer_write_offset != context.output_buffer_read_offset;
if (atascado) {
  context.output_buffer_write_offset = context.output_buffer_read_offset;
  context.output_buffer_valid = 0;
  context.Store(context_ptr);
  return context.output_buffer_write_offset;
}
```

It's a deliberate hack: it breaks the stall instead of preventing it. It may cost an
audio glitch on that voice. If many `[desatasco]` lines show up in the log, it means the
audio thread is tight on time on that machine and that's what needs to be worked on,
not the symptom.

## The wrong attempt, which is the useful part

Before this I tried something else: reserving one block in the ring buffer so that
`read == write` could only mean "empty" and never "full". It's the textbook fix
for an ambiguous ring buffer.

**It was wrong, and wrong for a reason you can't see from outside the game.**

Reading the game's own recompiled code shows that `output_buffer_valid = 0` with
the ring full **is not a bug: it's the signal the hardware gives the game** to say
"buffer full". The game reads it and acts accordingly. By "fixing" it I was
removing the only exit it had.

It was reverted entirely.

The lesson: in a recompilation, the emulator's odd behavior may be
exactly what the game expects. Before fixing an oddity, check whether the game
is using it.

## How we got there

The path, because the method is worth more than the result:

1. **Instrument the XMA kernel** (`parche_anillo.py`) to see every call.
2. **A bug in the diagnosis itself:** I limited *all* functions, getters and setters,
   to one trace per second. That hid exactly what needed to be seen —the input
   deliveries— and I spent a while looking in the wrong direction. When the setters
   were left unlimited, the pattern showed up.
3. **Seeing that nothing was produced**: an explicit trace was added for the case "the
   context spun without producing a single sample", with the full state. There it
   became clear that it was a context with no input, spinning.
4. **Read the game's code**, not just the emulator's. `codegen.partition.json` maps
   guest addresses to generated files; with that you find the function that
   asks about the buffer and see what it does with the answer.

Step 4 is the one that solved the case. The first three only narrowed down where to look.

## What to check if it comes back

In the log:

- `[desatasco]` lines: how many and how often. None and the game holds up = it was this.
  Many = the audio thread is tight on time on that machine.
- `XmaContext {}: NO PRODUJO NADA` with the heavy instrumentation enabled
  (`tools/diagnostico/parche_xma.py`): it says which context, with what inputs and at what
  position in the ring.

And the warning `Too few processor cores - scheduling will be wonky` at startup is not
decorative: on machines with few cores the audio thread competes worse and the stall is
more likely.

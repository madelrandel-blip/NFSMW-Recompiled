# Phase 2 — From PowerPC to C++

## Creating the project

From the root of `NFSMW Recomp/`:

```bash
rexglue init --app_name "nfsmw" --app_root ./app --app_desc "NFS Most Wanted (2005) recompiled" --app_author "tu-nombre"
```

This generates in `app/`:
- `CMakeLists.txt` — the app project
- `src/main.cpp` — the host entry point
- `nfsmw_config.toml` — the codegen config
- stub and hook templates

Move or link that config to `config/nfsmw_config.toml` if you prefer to keep it with
the rest (or just edit the one `init` generated; what matters is that there's
**only one**).

## The minimal config

```toml
project_name        = "nfsmw"
file_path           = "../assets/default.xex"
out_directory_path  = "generated"

# If you have a title update:
# patch_file_path    = "../assets/patch.xexp"
# patched_file_path  = "../assets/default_patched.xex"
```

See `config/nfsmw_config.toml` in this repo: it already comes with all the options
commented and explained.

## First codegen pass

```bash
cmake --build --preset win-amd64-debug --target nfsmw_codegen
# or on Linux:
cmake --build --preset linux-amd64-debug --target nfsmw_codegen
```

It can also be invoked directly:
```bash
rexglue codegen config/nfsmw_config.toml --log_level debug --log_file codegen.log
```

What it does: loads the XEX, decrypts/decompresses it, walks the `.text` discovering
function boundaries, resolves jump tables, and writes C++ to `generated/`.

**The first pass almost never comes out clean.** Expect warnings and errors. That's
normal and expected — the phase 2 cycle is exactly about resolving them.

## The three classic problems

### 1. `bctr` with no resolved table

```
warning: unresolved jump table at 0x8210A4C0 (bctr)
```

The analysis didn't figure out where a `bctr` jumps to. You have to declare it by
hand. Open that address in Ghidra/IDA, see which register loads the index and where
the label table is, and add:

```toml
[[switch_tables]]
address  = 0x8210A4C0   # the bctr address
register = 11           # the GPR with the index (rN)
labels   = [0x8210A4D0, 0x8210A520, 0x8210A5A0]  # the destinations, in order
```

The `labels` go in the exact order of the index: `labels[0]` is where it jumps with
index 0. Getting them wrong produces silent, hard crashes, so double-check against
the disassembly.

### 2. Data inside `.text`

```
warning: invalid instruction at 0x8230F118
```

EA's compiler interleaves constants (float tables, strings, vtables) between code.
If there are only a few, adjust the threshold:

```toml
[analysis]
data_region_threshold = 8   # fewer consecutive invalid instructions to cut
```

If there's a specific repeated pattern:
```toml
[[invalid_instructions]]
data = 0x00000000
size = 16
```

### 3. Badly detected function boundaries

When a function has a jump table inside it, the analyzer overshoots or undershoots.
Declare it explicitly:

```toml
[functions]
0x8210A400 = { name = "Physics_Integrate", end = 0x8210B180 }
0x8215C200 = { size = 512 }
# discontinuous fragment that belongs to another function:
0x8215C900 = { parent = 0x8215C200, size = 64 }
```

Giving names to the functions you identify (`name = "..."`) makes the generated C++
readable and makes the debugger's stack traces make sense. It's very much worth
doing from the start.

## `setjmp` / `longjmp`

If the game uses them (very likely — EA used them for streaming error handling), you
have to tell it where they are or the non-local jumps corrupt the state:

```toml
setjmp_address  = 0x82XXXXXX
longjmp_address = 0x82XXXXXX
```

How to find them: in Ghidra, look for the function that saves ~18 non-volatile
registers (r14–r31, LR, CR, SP) into a buffer passed in `r3` and returns 0. That's
`setjmp`. `longjmp` is the one that does the reverse and ends in `mtctr`/`bctr`.

## Generated code quality options

All default to `false`. Enable them **after** you have a build that boots, never
before — they change codegen and can mask bugs:

```toml
cr_as_local           = true   # the biggest win: CR as locals
xer_as_local          = true
ctr_as_local          = true
non_volatile_as_local = true   # r14-r31 as locals
skip_lr               = true   # saves in leaf functions
```

`cr_as_local` has the biggest impact on branch-heavy code — which in a racing game
is almost everything (physics, traffic AI, collisions).

## Rebuild after every change

```bash
cmake --build --preset win-amd64-debug --target nfsmw_codegen
cmake --build --preset win-amd64-debug
```

When codegen finishes without errors and the project builds → `docs/03-runtime.md`.

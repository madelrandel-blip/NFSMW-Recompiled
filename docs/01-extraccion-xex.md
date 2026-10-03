# Phase 1 — Getting the `default.xex` out of your ISO

Goal: go from your dumped ISO to `assets/default.xex` + the game's assets, and note
down the `base address` you'll need in all the following phases.

All of this is about **your own dump**. Nothing that comes out of here gets uploaded
to any repository.

---

## Fast path: the included script

`tools/fase1_extraer.py` does the whole process. It only needs Python 3.8+, no
dependencies and no cloning anything.

### Step 1 — Look at what's inside (extracts nothing)

```powershell
python tools\fase1_extraer.py "D:\ruta\a\NFSMW.iso" --listar
```

Expected output, more or less:

```
Particion de juego en offset 0x0FD90000  (XGD2 (la mayoria de juegos de 360))
Directorio raiz: sector 33, 4,096 bytes

412 archivos, 18 directorios, 6,834,221,056 bytes en total

     6,291,456  default.xex
   [dir]        CARS
   [dir]        FRONTEND
   ...
```

If the detected offset isn't `0x0FD90000`, no problem: the script tries XGD1,
XGD3 and, if needed, scans the image looking for the file system.

**If it fails here** with "No se encontro un sistema de archivos XDVDFS": your image
isn't a flat ISO. `.cci`, `.god`, `.zar` and recompressed `.iso` files have to be
converted first (see below).

### Step 2 — Extract

```powershell
python tools\fase1_extraer.py "D:\ruta\a\NFSMW.iso" -o assets\game_root
```

It takes a few minutes and uses ~7 GB. When it finishes it automatically prints the
header of `default.xex`.

If you only want the executable for now (a few seconds, a few MB):

```powershell
python tools\fase1_extraer.py "D:\ruta\a\NFSMW.iso" -o assets\game_root --solo-xex
```

### Step 3 — Put the XEX where it belongs

**Don't move it out of `game_root`.** ReXGlue requires the `--xex-path` to be
*inside* the `--game-root`, because that's where it derives the guest paths
(`game:\...`) the game will use to look for its files. If you move it out,
`rexglue init` fails with:

```
Failed: --xex_path (...) is not inside --game_root (...)
```

The extraction already leaves it in place: `assets/game_root/default.xex`. There's
nothing to copy.

### Step 4 — Save the XEX info

```powershell
python tools\fase1_extraer.py assets\game_root\default.xex --info > docs\xex_info.txt
type docs\xex_info.txt
```

This is what you should see:

```
== Datos clave ==
  Title ID              : 454107D9
  Media ID              : ........
  Version               : 1.0.0.0
  Disco                 : 1 de 1
  Image base address    : 0x82000000
  Entry point           : 0x82......
  Load address          : 0x82000000
  Tamano de imagen      : ......... bytes
  Cifrado               : normal (AES-128) (1)
  Compresion            : normal (LZX) (2)

  >> Title ID coincide con Need for Speed: Most Wanted (2005). Correcto.
```

**What matters in that output:**

- **Title ID `454107D9`** confirms it's NFSMW 2005 and not the 2012 version or
  another game. If the script warns you with "OJO", stop and check the dump.
- **Image base address** (almost always `0x82000000`) is the reference for all the
  addresses you'll write in the TOML. Everything you declare — functions, jump
  tables, hooks — will be above this value.
- **AES-128 encryption + LZX compression** is normal for retail. **You don't have
  to decrypt it by hand**: ReXGlue does it internally during codegen. If this said
  "ninguna/ninguno" it would be an already-processed XEX, which also works.

On Linux it's identical, swapping `\` for `/`:

```bash
python3 tools/fase1_extraer.py ~/dumps/NFSMW.iso --listar
python3 tools/fase1_extraer.py ~/dumps/NFSMW.iso -o assets/game_root
python3 tools/fase1_extraer.py assets/game_root/default.xex --info > docs/xex_info.txt
```

---

## Special cases

### My dump isn't a flat ISO

| Format | What it is | How to convert it to ISO |
|---|---|---|
| `.cci` / `.cso` | Compressed ISO | `ciso` / `cci-tool` to decompress |
| GOD / `000D0000` folder | Games on Demand (STFS) | see below |
| `.zar` | Xbox Backup Creator archive | extract with XBC |

### I have a GOD / LIVE / CON instead of an ISO

Games on Demand aren't XDVDFS, they're STFS packages. For those you do need the
external tool:

```bash
git clone https://github.com/sp00nznet/360tools.git
python 360tools/tools/extract_stfs.py /ruta/al/archivo-header -o assets/game_root
```

A GOD comes as a file with no extension and a hexadecimal name, plus a `000D0000/`
folder with the parts. Point the script at the **header file**, not the folder.
Then continue from Step 3.

### I want to open it in Ghidra / IDA

You'll need this in phase 2 to locate `setjmp`/`longjmp` and resolve jump tables.

- **IDA Pro**: open the `.xex` directly with the Xbox 360 loader.
- **Ghidra**: it needs the raw PE (decrypted and decompressed). Use
  `360tools/tools/extract_pe.py`, or let ReXGlue do the codegen and work from the
  `.map` it generates. When loading it: **PowerPC 32-bit big-endian** processor.

---

## Where everything ends up

```
NFSMW Recomp/
└── assets/
    └── game_root/         ← --game-root: the XEX AND the assets, together
        ├── default.xex    ← --xex-path (it has to be in here)
        ├── Movies/
        └── NFS/
└── docs/
    └── xex_info.txt       ← the output from step 4
```

`assets/` is in the `.gitignore`. Keep it that way.

---

## Next

With `assets/game_root/default.xex` in place and `docs/xex_info.txt` saved →
`docs/02-codegen.md`.

#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
NFSMW Recomp - Phase 1: extract the Xbox 360 ISO and dump the XEX info.

No dependencies: just the Python 3.8+ standard library.

Typical usage:
    python tools/fase1_extraer.py "D:\\dumps\\NFSMW.iso" --listar
    python tools/fase1_extraer.py "D:\\dumps\\NFSMW.iso" -o assets/game_root
    python tools/fase1_extraer.py assets/game_root/default.xex --info

What it does:
  1. Detects the game partition offset (XGD1 / XGD2 / XGD3 / raw).
  2. Walks the XDVDFS tree and lists or extracts the files.
  3. Parses the XEX2 header of default.xex and pulls the title id, base
     address, entry point and the compression/encryption type.

It does NOT decrypt or decompress the PE: ReXGlue handles that internally
during codegen. Here we only need the files and the header data.
"""

import argparse
import os
import struct
import sys

SECTOR = 2048
XDVDFS_MAGIC = b"MICROSOFT*XBOX*MEDIA"

# Known offsets where the game partition starts, depending on the disc type.
KNOWN_BASES = [
    (0x00000000, "raw partition / already trimmed image"),
    (0x0FD90000, "XGD2 (most Xbox 360 games)"),
    (0x02080000, "XGD3 (late titles)"),
    (0x18300000, "XGD1 (original Xbox)"),
]

ATTR_DIRECTORY = 0x10


# ---------------------------------------------------------------------------
# XDVDFS
# ---------------------------------------------------------------------------

def _magic_at(fh, offset):
    """True if the volume descriptor is at base=offset."""
    try:
        fh.seek(offset + 32 * SECTOR)
    except OSError:
        return False
    head = fh.read(len(XDVDFS_MAGIC))
    return head == XDVDFS_MAGIC


def detectar_base(fh, limite_scan=1 << 30):
    """Returns (base, description) of the game partition."""
    for base, desc in KNOWN_BASES:
        if _magic_at(fh, base):
            return base, desc

    # None of the known ones: brute-force scan in large chunks.
    fh.seek(0, os.SEEK_END)
    tam = fh.tell()
    tope = min(tam, limite_scan)
    CHUNK = 16 << 20
    solapa = len(XDVDFS_MAGIC)
    pos = 0
    while pos < tope:
        fh.seek(pos)
        buf = fh.read(CHUNK + solapa)
        if not buf:
            break
        idx = buf.find(XDVDFS_MAGIC)
        while idx != -1:
            abs_off = pos + idx
            if abs_off % SECTOR == 0 and abs_off >= 32 * SECTOR:
                base = abs_off - 32 * SECTOR
                if _magic_at(fh, base):
                    return base, "detectado por barrido (offset no estandar)"
            idx = buf.find(XDVDFS_MAGIC, idx + 1)
        pos += CHUNK

    raise SystemExit(
        "No XDVDFS filesystem was found in the image.\n"
        "Check that it is an Xbox 360 ISO and not a compressed CCI/GOD/ZAR."
    )


def leer_descriptor(fh, base):
    """Returns (root_sector, root_size) by reading the volume descriptor."""
    fh.seek(base + 32 * SECTOR)
    vd = fh.read(SECTOR)
    if len(vd) < SECTOR or vd[:20] != XDVDFS_MAGIC:
        raise SystemExit("Invalid volume descriptor.")
    if vd[0x7EC:0x7EC + 20] != XDVDFS_MAGIC:
        print("  warning: missing closing magic at 0x7EC (truncated image?)",
              file=sys.stderr)
    sector_raiz, tam_raiz = struct.unpack_from("<II", vd, 0x14)
    return sector_raiz, tam_raiz


def _entradas(tabla, offset, vistos):
    """Walks the binary tree of a directory. Yields dicts per entry."""
    pila = [offset]
    while pila:
        off = pila.pop()
        if off in vistos:
            continue
        # A zero offset is only valid for the root of the tree.
        if off != 0 and off == 0:
            continue
        if off + 14 > len(tabla):
            continue
        vistos.add(off)

        izq, der, sector, tam, attrs, largo = struct.unpack_from(
            "<HHIIBB", tabla, off)

        # 0xFFFF and 0 mark "no child".
        for hijo in (izq, der):
            if hijo not in (0, 0xFFFF):
                pila.append(hijo * 4)

        fin_nombre = off + 14 + largo
        if largo == 0 or fin_nombre > len(tabla):
            continue
        nombre = tabla[off + 14:fin_nombre].decode("latin-1")

        yield {
            "nombre": nombre,
            "sector": sector,
            "tam": tam,
            "dir": bool(attrs & ATTR_DIRECTORY),
        }


def recorrer(fh, base, sector, tam, prefijo=""):
    """Recursively walks the directory tree. Yields (path, entry)."""
    if tam == 0 or tam > (256 << 20):
        return
    fh.seek(base + sector * SECTOR)
    tabla = fh.read(tam)
    if len(tabla) < tam:
        print("  warning: truncated directory table at %s" % (prefijo or "/"),
              file=sys.stderr)

    vistos = set()
    hijos = list(_entradas(tabla, 0, vistos))
    hijos.sort(key=lambda e: e["nombre"].lower())

    for e in hijos:
        ruta = prefijo + "/" + e["nombre"] if prefijo else e["nombre"]
        yield ruta, e
        if e["dir"]:
            for sub in recorrer(fh, base, e["sector"], e["tam"], ruta):
                yield sub


def extraer(fh, base, entrada, destino):
    os.makedirs(os.path.dirname(destino) or ".", exist_ok=True)
    fh.seek(base + entrada["sector"] * SECTOR)
    restante = entrada["tam"]
    with open(destino, "wb") as out:
        while restante > 0:
            trozo = fh.read(min(1 << 20, restante))
            if not trozo:
                raise SystemExit(
                    "Unexpected end of file while reading %s. Incomplete image?"
                    % entrada["nombre"])
            out.write(trozo)
            restante -= len(trozo)


# ---------------------------------------------------------------------------
# XEX2
# ---------------------------------------------------------------------------

CLAVES_XEX = {
    0x000002FF: "Resource info",
    0x000003FF: "File format info",
    0x000005FF: "Delta patch descriptor",
    0x000080FF: "Bounding path",
    0x00008105: "Device ID",
    0x00010001: "Original base address",
    0x00010100: "Entry point",
    0x00010201: "Image base address",
    0x000103FF: "Import libraries",
    0x00018002: "Checksum / timestamp",
    0x00018102: "Enabled for callcap",
    0x00018200: "Enabled for fastcap",
    0x000183FF: "Original PE name",
    0x000200FF: "Static libraries",
    0x00020104: "TLS info",
    0x00020200: "Default stack size",
    0x00020301: "Default filesystem cache size",
    0x00020401: "Default heap size",
    0x00028002: "Page heap size and flags",
    0x00030000: "System flags",
    0x00040006: "Execution info",
    0x00040201: "Title workspace size",
    0x00040310: "Game ratings",
    0x00040404: "LAN key",
    0x000405FF: "Xbox 360 logo",
    0x000406FF: "Multidisc media IDs",
    0x000407FF: "Alternate title IDs",
    0x00040801: "Additional title memory",
    0x00E10402: "Exports by name",
}

COMPRESION = {0: "none", 1: "basic", 2: "normal (LZX)", 3: "delta"}
CIFRADO = {0: "none", 1: "normal (AES-128)"}


def _u32(buf, off):
    return struct.unpack_from(">I", buf, off)[0]


def info_xex(ruta):
    with open(ruta, "rb") as fh:
        cab = fh.read(0x1000)
        if cab[:4] != b"XEX2":
            raise SystemExit(
                "%s does not start with the 'XEX2' magic. It is not an Xbox "
                "360 executable (or it is encrypted in another format)." % ruta)

        flags_modulo = _u32(cab, 0x04)
        off_pe = _u32(cab, 0x08)
        off_seguridad = _u32(cab, 0x10)
        n_opt = _u32(cab, 0x14)

        print("== XEX2 Header ==")
        print("  file                  : %s (%s bytes)"
              % (ruta, f"{os.path.getsize(ruta):,}"))
        print("  module flags          : 0x%08X" % flags_modulo)
        print("  PE data offset        : 0x%08X" % off_pe)
        print("  security info offset  : 0x%08X" % off_seguridad)
        print("  optional headers      : %d" % n_opt)

        # The optional headers may go beyond the 0x1000 bytes read.
        fh.seek(0x18)
        raw_opt = fh.read(n_opt * 8)
        opcionales = {}
        for i in range(n_opt):
            clave, valor = struct.unpack_from(">II", raw_opt, i * 8)
            opcionales[clave] = valor

        entry = opcionales.get(0x00010100)
        base_img = opcionales.get(0x00010201)

        print()
        print("== Datos clave ==")

        # Execution info -> title id, version, disc
        title_id = None
        if 0x00040006 in opcionales:
            fh.seek(opcionales[0x00040006])
            ei = fh.read(24)
            if len(ei) == 24:
                media_id, version, base_version, title_id = struct.unpack_from(
                    ">IIII", ei, 0)
                plataforma, tabla_exe, disco_n, disco_tot = struct.unpack_from(
                    ">BBBB", ei, 0x10)
                print("  Title ID              : %08X" % title_id)
                print("  Media ID              : %08X" % media_id)
                print("  Version               : %d.%d.%d.%d"
                      % ((version >> 28) & 0xF, (version >> 16) & 0xFFF,
                         (version >> 8) & 0xFF, version & 0xFF))
                print("  Disc                  : %d of %d" % (disco_n, disco_tot))
        else:
            print("  Title ID              : (no execution info)")

        if base_img is not None:
            print("  Image base address    : 0x%08X" % base_img)
        if entry is not None:
            print("  Entry point           : 0x%08X" % entry)

        # Security info -> load address, image size
        fh.seek(off_seguridad)
        si = fh.read(0x184)
        if len(si) >= 0x114:
            tam_imagen = _u32(si, 0x004)
            load_addr = _u32(si, 0x110)
            print("  Load address          : 0x%08X" % load_addr)
            print("  Image size            : %s bytes" % f"{tam_imagen:,}")

        # File format info -> compression / encryption
        if 0x000003FF in opcionales:
            fh.seek(opcionales[0x000003FF])
            ffi = fh.read(8)
            if len(ffi) == 8:
                _tam, cif, comp = struct.unpack(">IHH", ffi)
                print("  Encryption            : %s (%d)"
                      % (CIFRADO.get(cif, "unknown"), cif))
                print("  Compression           : %s (%d)"
                      % (COMPRESION.get(comp, "unknown"), comp))

        # Other useful fields that are inline
        if 0x00010001 in opcionales:
            print("  Original base address : 0x%08X" % opcionales[0x00010001])
        if 0x00020200 in opcionales:
            print("  Default stack size    : %s bytes" % f"{opcionales[0x00020200]:,}")
        if 0x00030000 in opcionales:
            print("  System flags          : 0x%08X" % opcionales[0x00030000])

        # Import libraries: which kernel modules the game uses.
        # This sets the work for phase 3: each module brings imports that
        # must be implemented or stubbed in the runtime.
        if 0x000103FF in opcionales:
            try:
                fh.seek(opcionales[0x000103FF])
                cab_imp = fh.read(12)
                if len(cab_imp) == 12:
                    _total, st_tam, st_num = struct.unpack(">III", cab_imp)
                    if 0 < st_tam <= (1 << 16):
                        tabla = fh.read(st_tam)
                        nombres = [n.decode("latin-1") for n in tabla.split(b"\x00")
                                   if n and all(32 <= c < 127 for c in n)]
                        if nombres:
                            print()
                            print("== Imported modules (%d) ==" % st_num)
                            for n in nombres:
                                print("  " + n)
            except (OSError, struct.error):
                pass

        print()
        print("== Optional headers present ==")
        for clave in sorted(opcionales):
            nombre = CLAVES_XEX.get(clave, "")
            print("  0x%08X  %-24s value/offset 0x%08X"
                  % (clave, nombre, opcionales[clave]))

        print()
        if title_id == 0x454107D9:
            print("  >> Title ID matches Need for Speed: Most Wanted (2005). Correct.")
        elif title_id is not None:
            print("  >> WARNING: the expected Title ID for NFSMW 2005 is 454107D9.")
            print("     This XEX is 0x%08X. Check that you dumped the right game." % title_id)

        print()
        print("Copy this output to docs/xex_info.txt: you will need the base")
        print("address every time you declare an address in the TOML.")


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def buscar_iso():
    """Looks for a single .iso in the project folder (or in the current one)."""
    raiz = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    candidatos = []
    for carpeta in (raiz, os.getcwd()):
        try:
            for n in os.listdir(carpeta):
                if n.lower().endswith(".iso"):
                    ruta = os.path.join(carpeta, n)
                    if ruta not in candidatos:
                        candidatos.append(ruta)
        except OSError:
            pass

    if not candidatos:
        raise SystemExit(
            "No file was given and there is no .iso in:\n"
            "  %s\n"
            "Pass the path as an argument:\n"
            "  python tools/fase1_extraer.py \"D:/ruta/al/juego.iso\" --listar" % raiz)

    if len(candidatos) > 1:
        msg = "There are several .iso files; say which one you want:\n"
        for c in candidatos:
            msg += "  %s\n" % c
        raise SystemExit(msg)

    return candidatos[0]


def main():
    p = argparse.ArgumentParser(
        description="Extracts an Xbox 360 ISO and dumps the XEX info.")
    p.add_argument("entrada", nargs="?", default=None,
                   help="path to the .iso, or to a .xex if you use --info. If omitted, looks for a single .iso in the project folder.")
    p.add_argument("-o", "--salida", default="assets/game_root",
                   help="destination folder for the extraction (default: assets/game_root)")
    p.add_argument("--listar", action="store_true",
                   help="only list the contents, without extracting anything")
    p.add_argument("--solo-xex", action="store_true",
                   help="extract only default.xex (and the .xexp if it exists)")
    p.add_argument("--info", action="store_true",
                   help="the input is a .xex: dump its header and exit")
    args = p.parse_args()

    if args.entrada is None:
        args.entrada = buscar_iso()
        print("ISO found automatically: %s\n" % args.entrada)

    if not os.path.exists(args.entrada):
        raise SystemExit("File does not exist: %s" % args.entrada)

    if args.info or args.entrada.lower().endswith(".xex"):
        info_xex(args.entrada)
        return

    with open(args.entrada, "rb") as fh:
        base, desc = detectar_base(fh)
        print("Game partition at offset 0x%08X  (%s)" % (base, desc))

        sector_raiz, tam_raiz = leer_descriptor(fh, base)
        print("Root directory: sector %d, %s bytes\n" % (sector_raiz, f"{tam_raiz:,}"))

        entradas = list(recorrer(fh, base, sector_raiz, tam_raiz))
        if not entradas:
            raise SystemExit("The filesystem is empty. Corrupt image?")

        n_arch = sum(1 for _, e in entradas if not e["dir"])
        total = sum(e["tam"] for _, e in entradas if not e["dir"])
        print("%d files, %d directories, %s bytes in total\n"
              % (n_arch, len(entradas) - n_arch, f"{total:,}"))

        if args.listar:
            for ruta, e in entradas:
                if e["dir"]:
                    print("  [dir]  %s" % ruta)
                else:
                    print("  %12s  %s" % (f"{e['tam']:,}", ruta))
            return

        objetivos = entradas
        if args.solo_xex:
            objetivos = [(r, e) for r, e in entradas
                         if not e["dir"] and
                         (r.lower().endswith(".xex") or r.lower().endswith(".xexp"))]
            if not objetivos:
                raise SystemExit("There is no .xex in the image.")

        hechos = 0
        for ruta, e in objetivos:
            if e["dir"]:
                continue
            destino = os.path.join(args.salida, ruta.replace("/", os.sep))
            extraer(fh, base, e, destino)
            hechos += 1
            if hechos % 50 == 0:
                print("  ... %d files" % hechos)
        print("\n%d files extracted to %s" % (hechos, args.salida))

    xex = os.path.join(args.salida, "default.xex")
    if os.path.exists(xex):
        print()
        info_xex(xex)
    else:
        print("\nWarning: no default.xex appeared in the root. Check the listing "
              "with --listar to see where the executable is.")


if __name__ == "__main__":
    main()

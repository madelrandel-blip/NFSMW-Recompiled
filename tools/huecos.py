#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Finds code gaps with no function assigned in the codegen output.

THE PROBLEM
-----------
The binary dies with:
    [FATAL] Call to invalid or unregistered function at guest address 0xXXXXXXXX

That happens when something calls INDIRECTLY (through a pointer or vtable) an
address that the analysis did not mark as a function start. The Validate phase
does not detect them: it only checks DIRECT jumps (b / bl). The indirect ones
only show up at runtime, one by one.

HOW IT FINDS THEM
-----------------
Each PowerPC instruction takes 4 bytes and the codegen emits exactly one
comment line "\\t// <asm>" per instruction. So:

    end_of_function = start + 4 * number_of_comments

With the start of each function (codegen.partition.json) and its computed end,
every stretch between the end of one and the start of the next is a gap with no
owner. The gaps that contain real code are the candidates to blow up startup.

Usage:
    python tools\\huecos.py
    python tools\\huecos.py --min 8
    python tools\\huecos.py --comprobar 0x8215FEA8 0x826BE258
"""


# ============================================================================
#  KNOWN LIMITATION - a gap is NOT always a function
#
#  This tool emits one declaration per gap, at its start address. That assumes
#  each gap contains exactly one function, and that is FALSE.
#
#  A gap is just space that automatic discovery did not claim. It can contain
#  several small functions in a row, typically tables of 8- and 16-byte thunks.
#  By declaring only the start, the Discover phase swallows the whole gap as
#  ONE single function and the other entry points become unreachable. The
#  symptom is:
#
#     [FATAL] Call to invalid or unregistered function at guest address 0x...
#
#  with an address that falls INSIDE an already declared gap.
#
#  Verified on 2026-09-04: four different crashes fell inside declared gaps,
#  at +8, +24, +64 and +96 bytes from their start. That is why the crash
#  seemed to "move": each fix uncovered the next entry of the same gap.
#
#  Subdividing all gaps every 8 bytes is NOT the solution: that would be more
#  than 3000 declarations and most gaps do contain a single function, so entry
#  points would be declared in the middle of a function.
#
#  What does work:
#    1. Subdivide only the gaps DEMONSTRATED to be multi-entry (where a crash
#       already happened inside). See the block at the end of app/huecos.toml.
#    2. SONDEO_RELEASE.bat, which discovers them empirically all at once.
# ============================================================================

import argparse
import bisect
import json
import os
import re
import sys

RE_FUNC = re.compile(r'^DEFINE_REX_FUNC\((?:sub_)?([0-9A-Fa-f]{8})\)')


def medir_funciones(gen_dir):
    """Returns {start: n_instructions} by walking the generated C++."""
    tamanos = {}
    ficheros = sorted(f for f in os.listdir(gen_dir) if f.endswith('.cpp'))
    for idx, nombre in enumerate(ficheros, 1):
        ruta = os.path.join(gen_dir, nombre)
        actual = None
        n = 0
        with open(ruta, 'r', encoding='utf-8', errors='replace') as fh:
            for linea in fh:
                if linea.startswith('DEFINE_REX_FUNC('):
                    m = RE_FUNC.match(linea)
                    if m:
                        actual = int(m.group(1), 16)
                        n = 0
                elif actual is not None:
                    if linea.startswith('\t// '):
                        n += 1
                    elif linea.startswith('}'):
                        tamanos[actual] = n
                        actual = None
        sys.stdout.write("\r  leidos %d/%d archivos" % (idx, len(ficheros)))
        sys.stdout.flush()
    print()
    return tamanos


def main():
    p = argparse.ArgumentParser(description="Finds code gaps with no function.")
    p.add_argument("--gen", default="app/generated/default",
                   help="folder with the generated code")
    p.add_argument("--min", type=int, default=4,
                   help="minimum gap size to list (default 4)")
    p.add_argument("--comprobar", nargs="*", default=[],
                   help="specific addresses to locate, e.g. 0x8215FEA8")
    p.add_argument("--salida", default="docs/huecos.txt")
    p.add_argument("--toml", default="tools/huecos_functions.toml")
    args = p.parse_args()

    part = os.path.join(args.gen, "codegen.partition.json")
    if not os.path.exists(part):
        raise SystemExit("%s does not exist. Run the codegen first." % part)

    print("Reading function assignment...")
    asignaciones = json.load(open(part))["assignments"]
    inicios = sorted(int(k, 16) for k in asignaciones)
    print("  %d functions" % len(inicios))

    print("Measuring each function in the generated C++...")
    tamanos = medir_funciones(args.gen)
    print("  %d measured" % len(tamanos))

    faltan = [a for a in inicios if a not in tamanos]
    if faltan:
        print("  warning: %d functions not measured (ignored)" % len(faltan))

    # Compute gaps
    huecos = []
    for i, a in enumerate(inicios[:-1]):
        n = tamanos.get(a)
        if n is None:
            continue
        fin = a + 4 * n
        siguiente = inicios[i + 1]
        if fin < siguiente:
            huecos.append((fin, siguiente - fin, a, siguiente))

    grandes = [h for h in huecos if h[1] >= args.min]

    # Size distribution
    dist = {}
    for _, tam, _, _ in huecos:
        dist[tam] = dist.get(tam, 0) + 1

    lineas = []
    def w(s=""):
        print(s)
        lineas.append(s)

    w()
    w("=" * 60)
    w("  CODE GAPS WITH NO FUNCTION ASSIGNED")
    w("=" * 60)
    w("Functions            : %d" % len(inicios))
    w("Total gaps           : %d" % len(huecos))
    w("Gaps >= %d bytes      : %d" % (args.min, len(grandes)))
    w("Bytes in gaps        : %d" % sum(h[1] for h in huecos))
    w()
    w("Distribution by gap size:")
    for tam in sorted(dist):
        w("   %5d bytes  x %d" % (tam, dist[tam]))
    w()

    if args.comprobar:
        w("-" * 60)
        w("Queried addresses:")
        finales = sorted(h[0] for h in huecos)
        for s in args.comprobar:
            t = int(s, 16)
            es_inicio = bisect.bisect_left(inicios, t) < len(inicios) and \
                        inicios[bisect.bisect_left(inicios, t)] == t
            dentro = None
            for fin, tam, ini, sig in huecos:
                if fin <= t < fin + tam:
                    dentro = (fin, tam, ini, sig)
                    break
            w("  0x%08X  function start: %s" % (t, "YES" if es_inicio else "NO"))
            if dentro:
                w("      falls in a %d-byte gap: 0x%08X - 0x%08X"
                  % (dentro[1], dentro[0], dentro[0] + dentro[1]))
                w("      (after function 0x%08X, before 0x%08X)"
                  % (dentro[2], dentro[3]))
            elif not es_inicio:
                w("      does not fall in any known gap")
        w()

    w("-" * 60)
    w("First 60 gaps of >= %d bytes:" % args.min)
    for fin, tam, ini, sig in grandes[:60]:
        w("  0x%08X  %5d bytes   (after 0x%08X, before 0x%08X)"
          % (fin, tam, ini, sig))
    if len(grandes) > 60:
        w("  ... and %d more" % (len(grandes) - 60))

    os.makedirs(os.path.dirname(args.salida) or ".", exist_ok=True)
    with open(args.salida, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lineas) + "\n")
        fh.write("\n" + "-" * 60 + "\nALL gaps >= %d bytes:\n" % args.min)
        for fin, tam, ini, sig in grandes:
            fh.write("  0x%08X  %5d bytes   (after 0x%08X, before 0x%08X)\n"
                     % (fin, tam, ini, sig))
    print()
    print("Full report in %s" % args.salida)

    os.makedirs(os.path.dirname(args.toml) or ".", exist_ok=True)
    with open(args.toml, "w", encoding="utf-8") as fh:
        fh.write("# Generated by tools/huecos.py - do NOT include as is.\n")
        fh.write("# Declaring a function in a gap that is padding or data may\n")
        fh.write("# break the codegen. Copy only the entries you need.\n")
        fh.write("[functions]\n")
        for fin, tam, ini, sig in grandes:
            fh.write('"0x%08X" = { }   # %d bytes, after 0x%08X\n' % (fin, tam, ini))
    print("Reference TOML block in %s" % args.toml)


if __name__ == "__main__":
    main()

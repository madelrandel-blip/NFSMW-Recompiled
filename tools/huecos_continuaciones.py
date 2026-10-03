#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Computes the "continuation" entries of app/huecos_*.toml from the codegen log
and the function assignment.

The codegen warns in the Write phase:

    Unresolved conditional branch to 0xT from 0xS

That means the jump comes out of a gap (a manually declared function) and its
destination did not land inside any function. It is fixed in two ways, same as
the CONTINUACION section of the PAL project's app/huecos.toml:

  * BACKWARD jump (T < start of the gap): the gap is the parent's tail, cut too
    soon. The PARENT IS EXTENDED to the end of the gap and the gap is no longer
    declared separately.
  * FORWARD jump inside the gap: the gap is declared with
    end = farthest_target + 4, without invading the start of the next function.

Usage:
    python tools/huecos_continuaciones.py --gen app/generated/default ^
        --huecos app/huecos_usa.toml --log logs/codegen_usa.log            (view)
    python tools/huecos_continuaciones.py ... --aplicar                    (write)
"""

import argparse
import bisect
import collections
import json
import os
import re
import sys

RE_SIMPLE = re.compile(r'^"(0x[0-9A-Fa-f]+)"\s*=\s*\{\s*\}\s*#\s*(\d+) bytes')
RE_END = re.compile(r'^"(0x[0-9A-Fa-f]+)"\s*=\s*\{\s*end\s*=\s*(0x[0-9A-Fa-f]+)\s*\}')
RE_BRANCH = re.compile(
    r'Unresolved conditional branch to (0x[0-9A-Fa-f]+) from (0x[0-9A-Fa-f]+)')
MARCA = "# --- Funciones alargadas para cubrir sus saltos internos ---"


def cargar_funciones(gen_dir):
    part = os.path.join(gen_dir, "codegen.partition.json")
    if not os.path.exists(part):
        raise SystemExit("%s does not exist. Run the codegen first." % part)
    data = json.load(open(part, encoding="utf-8"))
    return sorted(int(k, 16) for k in data["assignments"])


def cargar_huecos(ruta):
    huecos = {}
    with open(ruta, encoding="utf-8") as fh:
        for linea in fh:
            linea = linea.strip()
            m = RE_SIMPLE.match(linea)
            if m:
                huecos[int(m.group(1), 16)] = int(m.group(2))
                continue
            m = RE_END.match(linea)
            if m:
                inicio = int(m.group(1), 16)
                fin = int(m.group(2), 16)
                huecos[inicio] = fin - inicio
    return huecos


def cargar_ramas(log):
    ramas = set()
    with open(log, encoding="utf-8", errors="replace") as fh:
        for linea in fh:
            m = RE_BRANCH.search(linea)
            if m:
                ramas.add((int(m.group(2), 16), int(m.group(1), 16)))
    return ramas


def calcular(starts, huecos, ramas):
    """Returns (parents, gaps_with_end, absorbed)."""
    por_hueco = collections.defaultdict(list)
    for src, tgt in ramas:
        i = bisect.bisect_right(starts, src) - 1
        f = starts[i] if i >= 0 else None
        if f in huecos:
            por_hueco[f].append(tgt)

    padres = {}
    con_end = {}
    absorbidos = set()
    for g, destinos in sorted(por_hueco.items()):
        fin_hueco = g + huecos[g]
        i = starts.index(g)
        siguiente = starts[i + 1] if i + 1 < len(starts) else None
        if any(t < g for t in destinos):
            # Parent continuation: extend it to the end of the gap.
            if i == 0:
                print("  warning: gap 0x%08X has no previous parent" % g)
                continue
            padre = starts[i - 1]
            padres[padre] = max(padres.get(padre, 0), fin_hueco)
            absorbidos.add(g)
        else:
            fin = max(destinos) + 4
            if siguiente is not None and fin > siguiente:
                print("  warning: 0x%08X end 0x%08X overruns 0x%08X; clipping"
                      % (g, fin, siguiente))
                fin = siguiente
            con_end[g] = max(con_end.get(g, 0), fin)

    for p in padres:
        con_end[p] = max(con_end.get(p, 0), padres[p])
    return con_end, absorbidos


def aplicar(ruta, con_end, absorbidos):
    with open(ruta, encoding="utf-8") as fh:
        lineas = fh.read().splitlines()

    # Remove the previous continuations section, if any.
    for i, linea in enumerate(lineas):
        if linea.strip() == MARCA:
            lineas = lineas[:i]
            break
    while lineas and not lineas[-1].strip():
        lineas.pop()

    salida = []
    quitadas = 0
    for linea in lineas:
        m = RE_SIMPLE.match(linea.strip())
        if m and int(m.group(1), 16) in absorbidos:
            quitadas += 1
            continue
        if m and int(m.group(1), 16) in con_end:
            quitadas += 1
            continue
        salida.append(linea)

    salida.append("")
    salida.append(MARCA)
    for inicio in sorted(con_end):
        salida.append('"0x%08X" = { end = 0x%08X }' % (inicio, con_end[inicio]))
    salida.append("")

    with open(ruta, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(salida))
    return quitadas


def main():
    p = argparse.ArgumentParser(description="Computes gap continuations.")
    p.add_argument("--gen", default="app/generated/default")
    p.add_argument("--huecos", default="app/huecos_usa.toml")
    p.add_argument("--log", required=True, help="codegen log to analyze")
    p.add_argument("--aplicar", action="store_true",
                   help="write the TOML (by default it only reports)")
    args = p.parse_args()

    starts = cargar_funciones(args.gen)
    huecos = cargar_huecos(args.huecos)
    ramas = cargar_ramas(args.log)
    print("functions: %d | simple gaps: %d | unresolved branches: %d"
          % (len(starts), len(huecos), len(ramas)))

    con_end, absorbidos = calcular(starts, huecos, ramas)
    print("continuations: %d entries | absorbed gaps: %d"
          % (len(con_end), len(absorbidos)))
    for inicio in sorted(con_end):
        print('  "0x%08X" = { end = 0x%08X }' % (inicio, con_end[inicio]))

    if args.aplicar:
        quitadas = aplicar(args.huecos, con_end, absorbidos)
        print("wrote %s (%d simple entries removed)" % (args.huecos, quitadas))
    else:
        print("(dry run; use --aplicar to write)")


if __name__ == "__main__":
    main()

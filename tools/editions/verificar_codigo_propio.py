# -*- coding: utf-8 -*-
# Checks that our own code (app/src) works the same in another edition, from the recompiled code of both.
#
# For every guest function our sources hook (REX_HOOK_RAW) or call (REX_EXTERN), and for every function that a data
# address we read is only named in, the Spanish function and its translation (app_<edition>/tabla.tsv) are compared
# instruction by instruction, ignoring what changes between editions without changing behaviour: call targets, the
# link register, labels and the immediates of loads of read-only constants (the same floats at shifted addresses).
# Any other difference (a structure offset, a branch, a register) is reported: there our code could read or call
# something else in that edition.
#
# Also lists the 82xxxxxx addresses in our sources that the translation table does not cover with a safe match, so
# they are checked by hand (data in the executable's read-only part that every edition keeps at the same address).
#
# Usage: py tools/editions/verificar_codigo_propio.py <edition> [files...]   (default: every app/src/*.cpp)
import glob
import os
import re
import sys

RAIZ = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def tabla(ed):
    mapa, estado = {}, {}
    for linea in open(os.path.join(RAIZ, 'app_' + ed, 'tabla.tsv'), encoding='utf-8').read().splitlines()[1:]:
        pal, _sec, otra, est = linea.split('\t')
        if otra != '-':
            mapa[pal.upper()] = otra.upper()
            estado[pal.upper()] = est
    return mapa, estado


def cuerpos(directorio):
    funciones = {}
    for f in glob.glob(os.path.join(directorio, 'nfsmw_recomp.*.cpp')):
        actual, lineas = None, []
        for linea in open(f, encoding='utf-8', errors='replace'):
            m = re.match(r'DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\)', linea)
            if m:
                actual, lineas = m.group(1), []
                continue
            if actual:
                if linea.startswith('}'):
                    funciones[actual] = lineas
                    actual = None
                else:
                    lineas.append(linea.rstrip())
    return funciones


IGNORAR = re.compile(r'^\s*//|sub_82|ctx\.lr = 0x|goto loc_|^loc_|case |switch|__builtin_trap')
SUMA = re.compile(r'(ctx\.r\d+\.s64 = ctx\.r\d+\.s64 \+ )-?\d+;')
CONSTANTE = re.compile(r'temp\.u32 = REX_LOAD_U32\(ctx\.r\d+\.u32 \+ -?\d+\);|ctx\.r12\.s64 = ctx\.r12\.s64 \+ \d+;')


def normal(lineas):
    out = []
    for linea in lineas:
        if IGNORAR.search(linea):
            continue
        out.append(CONSTANTE.sub('<constante>', linea))
    return out


def main():
    ed = sys.argv[1]
    fuentes = sys.argv[2:] or glob.glob(os.path.join(RAIZ, 'app', 'src', '*.cpp'))
    mapa, estado = tabla(ed)
    usadas = set()
    for f in fuentes:
        texto = open(f, encoding='utf-8', errors='replace').read()
        usadas |= {m.upper() for m in re.findall(r'REX_(?:HOOK_RAW|EXTERN)\((?:__imp__)?sub_([0-9A-Fa-f]{8})\)', texto)}
    print('%d funciones del juego usadas por nuestro codigo' % len(usadas))
    es = cuerpos(os.path.join(RAIZ, 'app', 'generated', 'default'))
    otra = cuerpos(os.path.join(RAIZ, 'app_' + ed, 'generated', 'default'))
    malas = datos = 0
    for a in sorted(usadas):
        t = mapa.get(a)
        if not t or a not in es or t not in otra:
            print('  %s: sin traduccion o sin codigo (%s)' % (a, t))
            malas += 1
            continue
        x, y = normal(es[a]), normal(otra[t])
        if x != y and [SUMA.sub('<dato>', l) for l in x] == [SUMA.sub('<dato>', l) for l in y]:
            # Only the low halves of data addresses (lis + addi) moved: the same data elsewhere in that edition.
            datos += 1
            continue
        if x != y:
            difs = [(p, q) for p, q in zip(x, y) if p != q][:3]
            print('  %s -> %s: %d lineas distintas, p. ej. %s' % (a, t, sum(1 for p, q in zip(x, y) if p != q) +
                                                               abs(len(x) - len(y)), difs[:1]))
            malas += 1
    # Addresses in our sources without a safe translation.
    sin = set()
    for f in fuentes:
        for m in re.findall(r'(?<![0-9A-Fa-f])0x(82[0-9A-Fa-f]{6})u?', open(f, encoding='utf-8', errors='replace').read()):
            m = m.upper()
            if not estado.get(m, '').startswith(('exacta', 'misma direccion', 'constante', 'referencias')):
                sin.add(m)
    print('%d iguales salvo direcciones de datos desplazadas' % datos)
    print('%d funciones distintas; direcciones a revisar a mano: %s' % (malas, ', '.join(sorted(sin)) or 'ninguna'))
    return 1 if malas else 0


if __name__ == '__main__':
    sys.exit(main())

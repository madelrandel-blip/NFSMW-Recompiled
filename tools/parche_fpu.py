#!/usr/bin/env python3
"""
FP exception mask fix for the ReXGlue SDK.

    python tools/parche_fpu.py            apply
    python tools/parche_fpu.py --estado
    python tools/parche_fpu.py --revertir

Touches two SDK headers:
    include/rex/platform/fpscr.h   setcsr() must keep FP exceptions masked
    include/rex/ppc/context.h      flush-mode helpers must always rewrite MXCSR

WHY
---
The recompiled guest runs host floating-point code. If anything on the guest
thread (a host library, FFmpeg/XMA, ...) leaves the host MXCSR with the FP
exception masks cleared, the next FP operation raises a hardware exception.
The SEH filter does not cover 0xC000008F (float inexact), so the process dies
with no [FATAL] line in the log.

That is exactly how the first USA builds died: Windows Error Reporting showed
exception 0xc000008f inside a generated function, and the log just stopped.

The fix keeps the exception masks set on every write to MXCSR, and rewrites
MXCSR from the cached control word on every flush-mode change, so a dirty
control word left by host code is repaired before the FP code runs.
"""

import argparse
import pathlib
import shutil
import sys

MARCA = "FP EXCEPTION MASKS - LOCAL PATCH"

# ---------------------------------------------------------------------------
#  1. include/rex/platform/fpscr.h
# ---------------------------------------------------------------------------

X86_ANCLA = """  static inline void setcsr(u32 csr) noexcept { simde_mm_setcsr(csr); }"""

X86_NUEVO = """  // FP exceptions stay masked: the guest must not be able to unmask them,
  // and a dirty MXCSR left by host code is repaired on the next write.
  // FP EXCEPTION MASKS - LOCAL PATCH
  static inline void setcsr(u32 csr) noexcept { simde_mm_setcsr(csr | ExceptionMask); }"""

ARM_ANCLA = """  static inline void setcsr(u32 csr) noexcept { __asm__ __volatile__("msr fpcr, %0" : : "r"(csr)); }"""

ARM_NUEVO = """  // FP EXCEPTION MASKS - LOCAL PATCH
  static inline void setcsr(u32 csr) noexcept {
    csr &= ~ExceptionMask;
    __asm__ __volatile__("msr fpcr, %0" : : "r"(csr));
  }"""

# ---------------------------------------------------------------------------
#  2. include/rex/ppc/context.h
# ---------------------------------------------------------------------------

CTX_ANCLA = """  inline void enableFlushMode() noexcept {
    if ((csr & FlushMask) != FlushMask) [[unlikely]] {
      csr |= FlushMask;
      setcsr(csr);
    }
  }

  inline void disableFlushMode() noexcept {
    if ((csr & FlushMask) != 0) [[unlikely]] {
      csr &= ~FlushMask;
      setcsr(csr);
    }
  }"""

CTX_NUEVO = """  // Always write, even when the bit already has the requested value: setcsr
  // re-asserts the FP exception masks and repairs a dirty MXCSR left by host
  // code mid-execution.
  // FP EXCEPTION MASKS - LOCAL PATCH
  inline void enableFlushMode() noexcept {
    csr |= FlushMask;
    setcsr(csr);
  }

  inline void disableFlushMode() noexcept {
    csr &= ~FlushMask;
    setcsr(csr);
  }"""


def localizar_sdk():
    raiz = pathlib.Path(__file__).resolve().parent.parent
    for cand in [raiz.parent / "rexglue-sdk", raiz / "sdk"]:
        if (cand / "include" / "rex" / "platform" / "fpscr.h").exists():
            return cand
    sys.exit("[ERROR] Cannot find the SDK (include/rex/platform/fpscr.h).\n"
             "        Looked in ..\\rexglue-sdk and .\\sdk")


def ya_aplicado(txt, ancla, nuevo, marca=MARCA):
    return marca in txt or ancla not in txt and nuevo.splitlines()[-1] in txt


def main():
    p = argparse.ArgumentParser(add_help=True)
    p.add_argument("--estado", action="store_true")
    p.add_argument("--revertir", action="store_true")
    args = p.parse_args()

    sdk = localizar_sdk()
    f_fpscr = sdk / "include" / "rex" / "platform" / "fpscr.h"
    f_ctx = sdk / "include" / "rex" / "ppc" / "context.h"

    trabajos = [
        (f_fpscr, [("x86 setcsr", X86_ANCLA, X86_NUEVO),
                   ("arm64 setcsr", ARM_ANCLA, ARM_NUEVO)]),
        (f_ctx, [("flush-mode helpers", CTX_ANCLA, CTX_NUEVO)]),
    ]

    if args.estado:
        for f, anclas in trabajos:
            t = f.read_text(encoding="utf-8")
            aplicado = all(a in t or n.splitlines()[-1] in t for _, a, n in anclas)
            print(f"  {f.name:16s}  {'APPLIED' if aplicado else 'not applied'}")
        return 0

    if args.revertir:
        for f, _ in trabajos:
            original = f.with_suffix(".h.original")
            if original.exists():
                shutil.copy2(original, f)
                print(f"[ok] Restored {f.name} from .original")
            else:
                print(f"[warning] No .original for {f.name}.")
        return 0

    planes = []
    for f, anclas in trabajos:
        txt = f.read_text(encoding="utf-8")
        bloques = []
        for nombre, ancla, nuevo in anclas:
            if MARCA in txt:
                continue
            if ancla not in txt:
                if nuevo.splitlines()[-1] in txt:
                    continue  # already fixed by hand, in another wording
                sys.exit(f"[ERROR] In {f.name}, anchor '{nombre}' not found and the "
                         f"fixed line is not there either. The SDK changed. Nothing touched.")
            bloques.append((nombre, ancla, nuevo))
        planes.append((f, txt, bloques))

    if all(not b for _, _, b in planes):
        print("[ok] Everything was already applied. Nothing touched.")
        return 0

    for f, txt, bloques in planes:
        if not bloques:
            continue
        original = f.with_suffix(".h.original")
        if not original.exists():
            shutil.copy2(f, original)
            print(f"[ok] Backup: {original.name}")
        for nombre, ancla, nuevo in bloques:
            txt = txt.replace(ancla, nuevo)
            print(f"[ok] Applied: {nombre}")
        f.write_text(txt, encoding="utf-8")

    print()
    print("  THE SDK MUST BE REBUILT for this to matter:")
    print("    cmake --build out/build/win-amd64 --config Release --target install")
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())

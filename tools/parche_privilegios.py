#!/usr/bin/env python3
"""
Lets you grant the Xbox Live privileges, so you can get into multiplayer.

    python tools/parche_privilegios.py            apply
    python tools/parche_privilegios.py --estado
    python tools/parche_privilegios.py --revertir

It touches one SDK file:  src/kernel/xam/xam_user.cpp

It does not keep a .original: it applies and undoes by exact text replacement,
block by block, like the other patches in this project.


WHERE THIS COMES FROM
=====================

When entering multiplayer, the game shows this sign:

    ATENCION
    Los privilegios que tienes en Xbox Live no te permiten acceder a esta
    funcion.

It is not a failure or a hang: it is a clean NO, and it arrives long before the
network is touched. The game asks about its privileges and is told it has none.

The answer is in xam_user.cpp, and the original comment leaves no room for
doubt:

    u32 XamUserCheckPrivilege_entry(u32 user_index, u32 mask, mapped_u32 out_value) {
      ...
      // If we deny everything, games should hopefully not try to do stuff.
      *out_value = 0;
      return X_ERROR_SUCCESS;
    }

It denies ALL privileges, always, whichever one is asked about. It comes from
Xenia and for an emulator without Xbox Live it has its logic: if the game
believes it has no permissions, it does not even try, and you are spared a hang
against servers that have been off for years.

The odd thing is that the rest of the SDK says just the opposite:

    XamUserIsOnlineEnabled   -> 1        (there is a connection)
    XamUserGetMembershipTier -> 6        (which is Gold)
    user_profile.signin_state -> 1       (there is a signed-in session)
    user_profile.type         -> 1 | 2   (local and online profile)

So the only piece that says no is this one. The profile is set up, the session
signed in and the membership is Gold; only the permissions are missing.


WHAT THIS DOES NOT FIX, WHICH IS THE IMPORTANT PART
===================================================

This opens the menu's DOOR. It does not make multiplayer work. Behind it, half
the network layer is still missing, and it is worth knowing before trying so as
not to be disappointed:

  - Of the 158 network functions declared by the SDK's ordinal table, 114 have
    no implementation. Among them are precisely the System Link ones:

        0x36  XNetCreateKey          0x41  XNetConnect
        0x37  XNetRegisterKey        0x42  XNetGetConnectStatus
        0x38  XNetUnregisterKey      0x53  XNetGetSystemLinkPort
        0x3F  XNetUnregisterInAddr   0x09  getsockname

    That CreateKey/RegisterKey/UnregisterKey trio is the one that associates
    the match's XNKID and XNKEY; XNetConnect and XNetGetConnectStatus are the
    ones that bring up the link with the other machine.

  - The session handlers in xam/apps/xgi_app.cpp are decorative: they read the
    parameters, write them to the log and return X_E_SUCCESS without doing
    anything. XSessionSearch does not even touch the results buffer, so a
    client searching for matches will always find zero.

So the usefulness of this patch is FINDING WHERE THE NEXT WALL IS. With it in
place, the menu should let you through, and whatever shows up in the log from
there on says what this particular game needs, which may be considerably less
than what is missing overall.


IT COMES DISABLED
=================

The new setting is  grant_user_privileges  and by default it is false, so the
behavior does not change until you turn it on. It is read on EVERY call, so it
can be enabled from the F4 menu without restarting the game: you enable it,
leave the multiplayer menu and enter again.

If granting them makes the game start trying Xbox Live things and hang, turn it
off and you are back to how it was. That is why it is a switch and not a fixed
change.
"""

import argparse
import pathlib
import sys


# ---------------------------------------------------------------------------
#  Block 1: the setting
# ---------------------------------------------------------------------------

CVAR_ANCLA = '''REXCVAR_DEFINE_UINT32(user_language, 1, "Kernel", "User's language ID");
'''

CVAR_NUEVO = '''REXCVAR_DEFINE_UINT32(user_language, 1, "Kernel", "User's language ID");

// PARCHE LOCAL - privilegios de Xbox Live
//
// Apagado por defecto: encendido cambia lo que el juego cree poder hacer, y eso
// merece ser una decision y no una sorpresa.
//
// kHotReload y no kRequiresRestart porque XamUserCheckPrivilege lo lee en cada
// llamada. Se puede encender desde F4 con el juego abierto; basta con salir del
// menu que dio el aviso y volver a entrar.
//
// El texto va en ingles porque es lo que sale en la ventana de F4, que es del
// SDK y esta entera en ingles.
REXCVAR_DEFINE_BOOL(grant_user_privileges, false, "Kernel",
                    "Tell the game it has every Xbox Live privilege. Off by default, which "
                    "makes the game refuse to open its multiplayer menus. Turning it on only "
                    "opens the door: system link also needs the XNet layer, which is only "
                    "half implemented here.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
'''


# ---------------------------------------------------------------------------
#  Block 2: the response
# ---------------------------------------------------------------------------

CHEQUEO_ANCLA = '''  // If we deny everything, games should hopefully not try to do stuff.
  *out_value = 0;
  return X_ERROR_SUCCESS;
}
'''

CHEQUEO_NUEVO = '''  // PARCHE LOCAL - privilegios de Xbox Live
  //
  // Aqui decia esto, y hacia exactamente lo que dice:
  //
  //     // If we deny everything, games should hopefully not try to do stuff.
  //     *out_value = 0;
  //
  // Deniega todos los privilegios, siempre, sea cual sea el que pregunten. Es
  // de Xenia y para un emulador sin Xbox Live se entiende: si el juego se cree
  // sin permisos ni lo intenta, y no se cuelga contra servidores apagados.
  //
  // En Most Wanted el efecto es el cartel "Los privilegios que tienes en Xbox
  // Live no te permiten acceder a esta funcion" nada mas tocar el multijugador.
  // Que ademas se contradice con el resto del SDK: XamUserIsOnlineEnabled
  // devuelve 1, XamUserGetMembershipTier devuelve 6 -que es Gold- y el perfil
  // dice signin_state 1 y type local|online. El unico que decia que no era este.
  //
  // OJO CON LO QUE ESTO NO HACE. Abre la puerta del menu y nada mas. El System
  // Link de detras necesita la capa XNet, y en este SDK faltan XNetCreateKey,
  // XNetRegisterKey, XNetUnregisterKey, XNetConnect y XNetGetConnectStatus,
  // ademas de que los manejadores de sesion de xgi_app.cpp devuelven exito sin
  // hacer nada. Esto sirve para ver donde esta el siguiente muro, no para
  // jugar en red.
  *out_value = REXCVAR_GET(grant_user_privileges) ? 1 : 0;
  return X_ERROR_SUCCESS;
}
'''


BLOQUES = [
    ("the grant_user_privileges setting", CVAR_ANCLA, CVAR_NUEVO),
    ("the XamUserCheckPrivilege response", CHEQUEO_ANCLA, CHEQUEO_NUEVO),
]


# There has not yet been any previous version of this patch. The list exists
# so the migration machinery is the same as in the other scripts: the day
# there is a v2, it is added here and it just works.
VIEJOS = []


def localizar_sdk():
    raiz = pathlib.Path(__file__).resolve().parent.parent
    for cand in [raiz.parent / "rexglue-sdk", raiz / "sdk"]:
        if (cand / "src" / "kernel" / "xam" / "xam_user.cpp").exists():
            return cand
    sys.exit("[ERROR] Cannot find the SDK's src/kernel/xam/xam_user.cpp.\n"
             "        Looked in ..\\rexglue-sdk and .\\sdk")


def quitar_version_vieja(txt):
    """Removes the remains of a previous version of this same patch.

    Same rule as in the other patches of the project: finding the old block
    only counts if it CANNOT be the good one seen halfway.

        es_de_verdad_vieja = (viejo in txt) and
                             (viejo not in nuevo or nuevo not in txt)

    See parche_backend.py, where the whole story is told and where it took
    three attempts to hit on it.
    """
    ahora = {ancla: nuevo for _, ancla, nuevo in BLOQUES}
    quitados = 0
    anclajes_hechos = set()
    for nombre, huella, viejo, ancla in VIEJOS:
        if ancla in anclajes_hechos:
            continue
        nuevo = ahora[ancla]
        if viejo not in txt:
            if huella in txt and nuevo not in txt:
                print(f"[aviso] I see remains of '{nombre}' but not in the form I expected.")
                print(f"        Leaving it alone; look at it by hand if something seems off.")
            continue
        if viejo in nuevo and nuevo in txt:
            anclajes_hechos.add(ancla)
            continue
        txt = txt.replace(viejo, ancla)
        anclajes_hechos.add(ancla)
        print(f"[ok] Removed previous version: {nombre}")
        quitados += 1
    return txt, quitados


def main():
    p = argparse.ArgumentParser(add_help=True)
    p.add_argument("--estado", action="store_true")
    p.add_argument("--revertir", action="store_true")
    args = p.parse_args()

    f = localizar_sdk() / "src" / "kernel" / "xam" / "xam_user.cpp"
    txt = f.read_text(encoding="utf-8")

    if args.estado:
        puestos = sum(1 for _, _, nuevo in BLOQUES if nuevo in txt)
        print(f"  {f.name:26s} {puestos} of {len(BLOQUES)} blocks applied")
        for nombre, _, nuevo in BLOQUES:
            print(f"      {'yes' if nuevo in txt else 'NO':>2}  {nombre}")
        return 0

    if args.revertir:
        quitados = 0
        for nombre, ancla, nuevo in BLOQUES:
            if nuevo not in txt:
                continue
            if txt.count(nuevo) != 1:
                sys.exit(f"[ERROR] The block '{nombre}' appears {txt.count(nuevo)} times.\n"
                         f"        I am not touching it, remove it yourself.")
            txt = txt.replace(nuevo, ancla)
            quitados += 1
        txt, viejos = quitar_version_vieja(txt)
        quitados += viejos
        if not quitados:
            print(f"[ok] {f.name}: there was nothing applied")
            return 0
        f.write_text(txt, encoding="utf-8")
        print(f"[ok] Removed {quitados} blocks from {f.name}")
        print()
        print("  THE SDK MUST BE RECOMPILED.")
        return 0

    txt, _ = quitar_version_vieja(txt)

    faltan = [(n, a, v) for n, a, v in BLOQUES if v not in txt]
    if not faltan:
        print(f"[ok] {f.name}: all {len(BLOQUES)} blocks were already there")
        return 0

    for nombre, ancla, _ in faltan:
        n = txt.count(ancla)
        if n != 1:
            sys.exit(f"[ERROR] The anchor for '{nombre}' appears {n} times, expected 1.\n"
                     f"        The SDK must have changed. I have not touched anything.")

    for nombre, ancla, nuevo in faltan:
        txt = txt.replace(ancla, nuevo)
        print(f"[ok] Applied: {nombre}")

    f.write_text(txt, encoding="utf-8")
    print()
    print("  In F4, Kernel category, setting  grant_user_privileges")
    print("  It comes OFF. When on, the game believes it has every permission.")
    print()
    print("  THE SDK MUST BE RECOMPILED:")
    print("    cmake --build out/build/win-amd64 --config Release --target install")
    return 0


if __name__ == "__main__":
    sys.exit(main())

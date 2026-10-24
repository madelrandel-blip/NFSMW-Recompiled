// nfsmw - textos de la interfaz en espanol, ingles y portugues.
//
// Todo texto visible pasa por T("clave"). Las tres traducciones de cada clave
// van juntas en idioma.cpp: anadir un texto obliga a escribirlo en los tres.

#pragma once

namespace nfsmw::idioma {

constexpr int kNumIdiomas = 3;

// 0 = espanol, 1 = ingles, 2 = portugues. Sale del cvar nfsmw_idioma o, si
// esta vacio, del idioma del sistema (ingles si no es ninguno de los tres).
int Actual();

// Cambia el idioma (lo guarda en el cvar nfsmw_idioma).
void Elegir(int idioma);

// Texto de la clave en el idioma actual. Si la clave no existe devuelve la
// propia clave, para que un olvido se vea en pantalla y no pase desapercibido.
const char* T(const char* clave);

// Nombre del idioma en su propia lengua ("Español", "English", "Português").
const char* Nombre(int idioma);

}  // namespace nfsmw::idioma

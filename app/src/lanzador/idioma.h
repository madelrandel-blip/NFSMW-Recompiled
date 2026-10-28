// nfsmw - textos de la interfaz en espanol, ingles, portugues, aleman, italiano y frances.
//
// Todo texto visible pasa por T("clave"). Las seis traducciones de cada clave
// van juntas en idioma.cpp: anadir un texto obliga a escribirlo en los seis.

#pragma once

namespace nfsmw::idioma {

constexpr int kNumIdiomas = 6;

// 0 = espanol, 1 = ingles, 2 = portugues, 3 = aleman, 4 = italiano, 5 = frances. Sale del cvar nfsmw_idioma o,
// si esta vacio, del idioma del sistema (ingles si no es ninguno de estos).
int Actual();

// Cambia el idioma (lo guarda en el cvar nfsmw_idioma).
void Elegir(int idioma);

// Texto de la clave en el idioma actual. Si la clave no existe devuelve la
// propia clave, para que un olvido se vea en pantalla y no pase desapercibido.
const char* T(const char* clave);

// Nombre del idioma en su propia lengua ("Español", "English", "Deutsch"...).
const char* Nombre(int idioma);

}  // namespace nfsmw::idioma

// nfsmw - gestor de trucos.
//
// Los trucos son escrituras en la memoria del juego, leidas de
// nfsmw_trucos.txt junto al ejecutable. El lanzador los lista y deja activar
// cada uno (cvar nfsmw_trucos_activos); con el juego en marcha, un hilo
// vuelve a escribir los activos cada 100 ms, para que el juego no los pise.
//
// Formato del fichero, un bloque por truco:
//
//   [Dinero infinito]
//   nota = Lo que hace, se muestra en el lanzador
//   u32 0x82A01234 1000000
//   u32 *0x82A05678+0x1C 5      <- puntero: lee la direccion y suma el desplazamiento
//
// Tipos: u8, u16, u32, f32. Los valores van en decimal o en 0x hexadecimal.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rex::memory {
class Memory;
}

namespace nfsmw::trucos {

struct Escritura {
  enum class Tipo { kU8, kU16, kU32, kF32 } tipo = Tipo::kU32;
  uint32_t direccion = 0;
  bool puntero = false;   // `direccion` guarda un puntero: escribir en *direccion + desplazamiento
  uint32_t desplazamiento = 0;
  uint32_t valor = 0;     // f32 guardado con sus bits
};

struct Truco {
  std::string nombre;
  std::string nota;
  std::vector<Escritura> escrituras;
};

std::filesystem::path RutaFichero();

// Lee el fichero. Si no existe, lo crea con la explicacion del formato.
// Las lineas que no se entienden se saltan y se cuentan en `errores`.
std::vector<Truco> Cargar(int* errores = nullptr);

bool Activo(const std::string& nombre);
void Activar(const std::string& nombre, bool activo);

// Con el juego en marcha: aplica los trucos activos en segundo plano.
void Arrancar(rex::memory::Memory* memoria);
void Parar();

}  // namespace nfsmw::trucos

// Byte order conversion of guest data, with the endianness chosen once per buffer so the loops stay tight (and
// vectorize) instead of deciding it per element.

#pragma once

#include <rex/graphics/xenos.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace nfsmw::renderer {

namespace detail {

template <typename Swap>
inline void CopyWords32(uint8_t* dest, const uint8_t* source, size_t words, Swap swap) {
  for (size_t i = 0; i < words; ++i) {
    uint32_t value;
    std::memcpy(&value, source + i * 4, 4);
    value = swap(value);
    std::memcpy(dest + i * 4, &value, 4);
  }
}

}  // namespace detail

// Copies `bytes / 4` 32-bit words from guest memory, swapping each as `endian` says.
inline void CopySwapped32(uint8_t* dest, const uint8_t* source, size_t bytes, rex::graphics::xenos::Endian endian) {
  using rex::graphics::xenos::Endian;
  const size_t words = bytes / 4;
  switch (endian) {
    case Endian::k8in16:
      detail::CopyWords32(dest, source, words,
                          [](uint32_t v) { return ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu); });
      break;
    case Endian::k8in32:
      detail::CopyWords32(dest, source, words, [](uint32_t v) { return __builtin_bswap32(v); });
      break;
    case Endian::k16in32:
      detail::CopyWords32(dest, source, words, [](uint32_t v) { return (v << 16) | (v >> 16); });
      break;
    default:
      std::memcpy(dest, source, words * 4);
      break;
  }
}

}  // namespace nfsmw::renderer

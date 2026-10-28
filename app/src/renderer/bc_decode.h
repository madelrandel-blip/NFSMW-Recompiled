// nfsmw - BC1-BC5 block decoding on the CPU: for devices without BC textures (textures.cpp) and for the touch
// controls' icons, which are DDS files of the game (nfsmw_tactil.cpp).
//
// Blocks of 4x4 texels, little-endian.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace nfsmw::renderer::bc {

inline uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }

// The color part of BC1-BC3: two RGB565 endpoints and 2-bit indices. Writes RGBA8 texels; for BC1 (`bc1`)
// with c0 <= c1, index 3 is transparent black.
inline void DecodeColorBlock(const uint8_t* block, bool bc1, uint8_t* out, size_t out_pitch) {
  const uint16_t c0 = Le16(block), c1 = Le16(block + 2);
  uint8_t palette[4][4];
  auto expand = [](uint16_t c, uint8_t* rgba) {
    rgba[0] = uint8_t(((c >> 11) & 31) * 255 / 31);
    rgba[1] = uint8_t(((c >> 5) & 63) * 255 / 63);
    rgba[2] = uint8_t((c & 31) * 255 / 31);
    rgba[3] = 255;
  };
  expand(c0, palette[0]);
  expand(c1, palette[1]);
  const bool four_colors = !bc1 || c0 > c1;
  for (int i = 0; i < 3; ++i) {
    if (four_colors) {
      palette[2][i] = uint8_t((2 * palette[0][i] + palette[1][i]) / 3);
      palette[3][i] = uint8_t((palette[0][i] + 2 * palette[1][i]) / 3);
    } else {
      palette[2][i] = uint8_t((palette[0][i] + palette[1][i]) / 2);
      palette[3][i] = 0;
    }
  }
  palette[2][3] = 255;
  palette[3][3] = four_colors ? 255 : 0;
  const uint32_t indices = uint32_t(block[4]) | (uint32_t(block[5]) << 8) | (uint32_t(block[6]) << 16) |
                           (uint32_t(block[7]) << 24);
  for (int y = 0; y < 4; ++y) {
    for (int x = 0; x < 4; ++x) {
      std::memcpy(out + y * out_pitch + x * 4, palette[(indices >> (2 * (y * 4 + x))) & 3], 4);
    }
  }
}

// One BC4 channel (also BC3's alpha and each BC5 channel): two 8-bit endpoints and 3-bit indices. Writes one byte
// per texel, `stride` bytes apart.
inline void DecodeChannelBlock(const uint8_t* block, uint8_t* out, size_t out_pitch, size_t stride) {
  const uint32_t a0 = block[0], a1 = block[1];
  uint8_t palette[8] = {uint8_t(a0), uint8_t(a1)};
  if (a0 > a1) {
    for (uint32_t i = 1; i < 7; ++i) palette[i + 1] = uint8_t(((7 - i) * a0 + i * a1) / 7);
  } else {
    for (uint32_t i = 1; i < 5; ++i) palette[i + 1] = uint8_t(((5 - i) * a0 + i * a1) / 5);
    palette[6] = 0;
    palette[7] = 255;
  }
  uint64_t indices = 0;
  for (int i = 0; i < 6; ++i) indices |= uint64_t(block[2 + i]) << (8 * i);
  for (int y = 0; y < 4; ++y) {
    for (int x = 0; x < 4; ++x) {
      out[y * out_pitch + x * stride] = palette[(indices >> (3 * (y * 4 + x))) & 7];
    }
  }
}

// BC2's explicit alpha: 4 bits per texel, into the alpha bytes of RGBA8 texels.
inline void DecodeExplicitAlpha(const uint8_t* block, uint8_t* out, size_t out_pitch) {
  for (int y = 0; y < 4; ++y) {
    const uint16_t row = Le16(block + y * 2);
    for (int x = 0; x < 4; ++x) {
      out[y * out_pitch + x * 4 + 3] = uint8_t(((row >> (4 * x)) & 15) * 17);
    }
  }
}

}  // namespace nfsmw::renderer::bc

// Writes the PE image inside a default.xex (decrypted and decompressed), the input of emparejar.py.
//
//   imagen_xex <default.xex> <output image>
//
// Retail discs are AES-128 encrypted and LZX compressed; fan translations are often plain or use basic
// compression. All three are accepted, the same as the launcher's shader generator
// (app/src/lanzador/shaders.cpp).
//
// Build: clang++ -std=c++20 imagen_xex.cpp lzxd.c system.c -I<libmspack/mspack> -I<sdk/thirdparty>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" {
#include <lzx.h>
#include <mspack.h>
}

#include "crypto/rijndael-alg-fst.c"

namespace {

using Bytes = std::vector<uint8_t>;

uint32_t Be32(const Bytes& b, size_t o) {
  if (o + 4 > b.size()) throw std::runtime_error("truncated XEX header");
  return uint32_t(b[o]) << 24 | uint32_t(b[o + 1]) << 16 | uint32_t(b[o + 2]) << 8 | b[o + 3];
}

constexpr uint8_t kRetailKey[16] = {0x20, 0xB1, 0x85, 0xA5, 0x9D, 0x28, 0xFD, 0xC3,
                                    0x40, 0x58, 0x3F, 0xBB, 0x08, 0x96, 0xBF, 0x91};

// AES-128-CBC, zero IV, no padding.
Bytes Decrypt(const uint8_t* data, size_t n, const uint8_t key[16]) {
  uint32_t rk[4 * (14 + 1)];
  const int rounds = rijndaelKeySetupDec(rk, key, 128);
  Bytes out(n - n % 16);
  uint8_t prev[16] = {};
  for (size_t i = 0; i < out.size(); i += 16) {
    uint8_t block[16];
    rijndaelDecrypt(rk, rounds, data + i, block);
    for (int j = 0; j < 16; ++j) out[i + j] = block[j] ^ prev[j];
    std::memcpy(prev, data + i, 16);
  }
  return out;
}

struct MemoryFile {
  mspack_system sys;
  uint8_t* data;
  size_t size;
  size_t pos;
};
int ReadMemory(mspack_file* f, void* buf, int n) {
  auto* m = reinterpret_cast<MemoryFile*>(f);
  const size_t k = std::min<size_t>(size_t(n), m->size - m->pos);
  std::memcpy(buf, m->data + m->pos, k);
  m->pos += k;
  return int(k);
}
int WriteMemory(mspack_file* f, void* buf, int n) {
  auto* m = reinterpret_cast<MemoryFile*>(f);
  const size_t k = std::min<size_t>(size_t(n), m->size - m->pos);
  std::memcpy(m->data + m->pos, buf, k);
  m->pos += k;
  return int(k);
}
void* Alloc(mspack_system*, size_t n) { return std::calloc(n, 1); }
void Free(void* p) { std::free(p); }
void Copy(void* src, void* dst, size_t n) { std::memcpy(dst, src, n); }

Bytes DecompressLzx(Bytes packed, uint32_t bits, size_t size) {
  mspack_system sys{};
  sys.read = ReadMemory;
  sys.write = WriteMemory;
  sys.alloc = Alloc;
  sys.free = Free;
  sys.copy = Copy;
  Bytes image(size);
  MemoryFile in{sys, packed.data(), packed.size(), 0};
  MemoryFile out{sys, image.data(), image.size(), 0};
  lzxd_stream* lzx = lzxd_init(&sys, reinterpret_cast<mspack_file*>(&in), reinterpret_cast<mspack_file*>(&out),
                               int(bits), 0, 0x8000, off_t(size), 0);
  if (!lzx) throw std::runtime_error("could not start LZX");
  const int r = lzxd_decompress(lzx, off_t(size));
  lzxd_free(lzx);
  if (r != 0) throw std::runtime_error("LZX failed (" + std::to_string(r) + ")");
  return image;
}

Bytes XexImage(const Bytes& xex) {
  if (Be32(xex, 0) != 0x58455832) throw std::runtime_error("not an XEX2");
  const uint32_t header = Be32(xex, 8), security = Be32(xex, 16), optional = Be32(xex, 20);
  if (header > xex.size() || security + 0x160 > header || 24 + 8ull * optional > header)
    throw std::runtime_error("XEX header out of bounds");
  uint32_t format = 0;
  for (uint32_t i = 0; i < optional; ++i)
    if (Be32(xex, 24 + 8 * i) == 0x000003FF) format = Be32(xex, 28 + 8 * i);
  if (!format || format + 8 > header) throw std::runtime_error("the XEX does not describe its format");
  const uint32_t info = Be32(xex, format), types = Be32(xex, format + 4);
  const uint32_t compression = types & 0xFFFF, encryption = types >> 16;
  const uint32_t image_size = Be32(xex, security + 4);
  if (info < 8 || format + info > header || image_size > 512u * 1024 * 1024)
    throw std::runtime_error("invalid XEX format");

  Bytes body(xex.begin() + header, xex.end());
  if (encryption == 1) {
    const Bytes session = Decrypt(xex.data() + security + 0x150, 16, kRetailKey);
    body = Decrypt(body.data(), body.size(), session.data());
  } else if (encryption != 0) {
    throw std::runtime_error("unsupported XEX encryption");
  }

  Bytes image;
  if (compression == 0) {
    image.assign(body.begin(), body.begin() + std::min<size_t>(body.size(), image_size));
    image.resize(image_size);
  } else if (compression == 1) {
    image.assign(image_size, 0);
    size_t src = 0, dst = 0;
    for (uint32_t at = format + 8; at + 8 <= format + info; at += 8) {
      const uint32_t data = Be32(xex, at), zeros = Be32(xex, at + 4);
      if (src + data > body.size() || dst + data + zeros > image_size) throw std::runtime_error("block outside the XEX");
      std::memcpy(image.data() + dst, body.data() + src, data);
      src += data;
      dst += data + zeros;
    }
  } else if (compression == 2) {
    if (info < 36) throw std::runtime_error("missing LZX information");
    const uint32_t window = Be32(xex, format + 8);
    if (!window || (window & (window - 1))) throw std::runtime_error("invalid LZX window");
    uint32_t bits = 0;
    while ((1u << bits) < window) ++bits;
    // Block chain: each block starts with the next block's size and its SHA-1 (20 bytes), then
    // [2-byte size][data] chunks up to a 0.
    uint32_t block_size = Be32(xex, format + 12);
    size_t pos = 0;
    Bytes packed;
    while (block_size) {
      const size_t end = pos + block_size;
      if (block_size < 26 || end > body.size()) throw std::runtime_error("block outside the XEX");
      const uint32_t next = Be32(body, pos);
      size_t p = pos + 24;
      for (;;) {
        if (p + 2 > end) throw std::runtime_error("block without an end");
        const size_t n = size_t(body[p]) << 8 | body[p + 1];
        p += 2;
        if (!n) break;
        if (p + n > end) throw std::runtime_error("chunk outside its block");
        packed.insert(packed.end(), body.begin() + p, body.begin() + p + n);
        p += n;
      }
      pos = end;
      block_size = next;
    }
    image = DecompressLzx(std::move(packed), bits, image_size);
  } else {
    throw std::runtime_error("unsupported XEX compression");
  }
  if (image.size() < 2 || image[0] != 'M' || image[1] != 'Z') throw std::runtime_error("the image has no MZ signature");
  return image;
}

}  // namespace

int main(int argc, char** argv) try {
  if (argc != 3) {
    std::fprintf(stderr, "usage: imagen_xex <default.xex> <output image>\n");
    return 1;
  }
  std::ifstream in(argv[1], std::ios::binary);
  if (!in) throw std::runtime_error("cannot open the XEX");
  const Bytes xex((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const Bytes image = XexImage(xex);
  std::ofstream(argv[2], std::ios::binary).write(reinterpret_cast<const char*>(image.data()), std::streamsize(image.size()));
  std::printf("%zu bytes\n", image.size());
  return 0;
} catch (const std::exception& e) {
  std::fprintf(stderr, "imagen_xex: %s\n", e.what());
  return 2;
}

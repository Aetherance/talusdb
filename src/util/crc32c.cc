#include "util/crc32c.h"

#include <cstddef>
#include <cstdint>

#include "port.h"

#if HAVE_CRC32C
#include <crc32c/crc32c.h>
#endif  // HAVE_CRC32C

namespace db {
namespace crc32c {
namespace {

constexpr uint32_t kCrc32cPolynomial = 0x82f63b78U;

uint32_t ExtendByte(uint32_t crc, uint8_t byte) {
  crc ^= static_cast<uint32_t>(byte);
  for (int bit = 0; bit < 8; ++bit) {
    if ((crc & 1U) != 0U) {
      crc = (crc >> 1) ^ kCrc32cPolynomial;
    } else {
      crc >>= 1;
    }
  }
  return crc;
}

}  // namespace

uint32_t Extend(uint32_t init_crc, const char* data, size_t n) {
#if HAVE_CRC32C
  return ::crc32c::Extend(init_crc, reinterpret_cast<const uint8_t*>(data), n);
#else
  uint32_t crc = ~init_crc;
  const auto* bytes = reinterpret_cast<const uint8_t*>(data);

  for (size_t index = 0; index < n; ++index) {
    crc = ExtendByte(crc, bytes[index]);
  }

  return ~crc;
#endif  // HAVE_CRC32C
}

}  // namespace crc32c
}  // namespace db

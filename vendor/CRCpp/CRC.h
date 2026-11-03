// Small CRCpp-compatible shim for the two JS8 message checksums.
// Check values: CRC-16/KERMIT("123456789") = 0x2189,
//               CRC-32/BZIP2("123456789") = 0xFC891918.
#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace CRC {

struct Crc16KermitPolicy { using value_type = std::uint16_t; };
struct Crc32Bzip2Policy { using value_type = std::uint32_t; };

inline Crc16KermitPolicy CRC_16_KERMIT() { return {}; }
inline Crc32Bzip2Policy CRC_32_BZIP2() { return {}; }

template <typename Policy>
inline typename Policy::value_type Calculate(const void* data, std::size_t size,
                                              const Policy& = Policy()) {
  using T = typename Policy::value_type;
  const auto* bytes = static_cast<const std::uint8_t*>(data);

  if constexpr (std::is_same_v<Policy, Crc16KermitPolicy>) {
    // RefIn/RefOut=true, poly=0x1021 (reflected 0x8408), init=0, xorout=0.
    std::uint16_t crc = 0;
    for (std::size_t i = 0; i < size; ++i) {
      crc ^= bytes[i];
      for (int bit = 0; bit < 8; ++bit)
        crc = static_cast<std::uint16_t>(
            (crc >> 1) ^ ((crc & 1u) ? 0x8408u : 0u));
    }
    return static_cast<T>(crc);
  } else {
    static_assert(std::is_same_v<Policy, Crc32Bzip2Policy>,
                  "Unsupported CRC policy");
    // RefIn/RefOut=false, poly=0x04C11DB7, init/xorout=0xFFFFFFFF.
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < size; ++i) {
      crc ^= static_cast<std::uint32_t>(bytes[i]) << 24;
      for (int bit = 0; bit < 8; ++bit)
        crc = (crc << 1) ^ ((crc & 0x80000000u) ? 0x04C11DB7u : 0u);
    }
    return static_cast<T>(crc ^ 0xFFFFFFFFu);
  }
}

}  // namespace CRC

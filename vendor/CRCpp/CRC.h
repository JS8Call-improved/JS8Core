
/**
    @file CRC.h
    @author Daniel Bahr
    @version 0.2.0.6
    @copyright
    @parblock
        CRC++
        Copyright (c) 2016, Daniel Bahr
        All rights reserved.

        Redistribution and use in source and binary forms, with or without
        modification, are permitted provided that the following conditions are met:

        * Redistributions of source code must retain the above copyright notice, this
          list of conditions and the following disclaimer.

        * Redistributions in binary form must reproduce the above copyright notice,
          this list of conditions and the following disclaimer in the documentation
          and/or other materials provided with the distribution.

        * Neither the name of CRC++ nor the names of its
          contributors may be used to endorse or promote products derived from
          this software without specific prior written permission.

        THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
        AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
        IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
        DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
        FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
        DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
        SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
        CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
        OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
        OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
    @endparblock
*/

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

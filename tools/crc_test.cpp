#include <cstdint>
#include <iostream>
#include <string_view>
#include "vendor/CRCpp/CRC.h"

int main() {
  struct Vector {
    std::string_view input;
    std::uint16_t kermit;
    std::uint32_t bzip2;
  };
  constexpr Vector vectors[] = {
    {"", 0x0000u, 0x00000000u},
    {"123456789", 0x2189u, 0xFC891918u},
  };
  for (auto const& v : vectors) {
    auto a = CRC::Calculate(v.input.data(), v.input.size(), CRC::CRC_16_KERMIT());
    auto b = CRC::Calculate(v.input.data(), v.input.size(), CRC::CRC_32_BZIP2());
    if (a != v.kermit || b != v.bzip2) {
      std::cerr << "CRC known-answer failure for input length " << v.input.size()
                << ": KERMIT=" << std::hex << a << " BZIP2=" << b << '\n';
      return 1;
    }
  }
  std::cout << "CRC known-answer vectors PASS\n";
  return 0;
}

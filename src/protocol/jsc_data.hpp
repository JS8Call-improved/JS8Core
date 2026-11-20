#pragma once

#include <cstdint>
#include <string_view>

// Index-ordered JSC dictionary (jsc_map.cpp.inc): entry i is the byte slice
// JSC_TEXT[JSC_OFFSETS[i] .. JSC_OFFSETS[i + 1]). No pointers, so a shared
// library needs no relocations for it.
extern unsigned char const JSC_TEXT[];
extern std::uint32_t const JSC_OFFSETS[262145];
extern std::uint32_t const JSC_SIZE;

// Text of dictionary entry `index` (index < JSC_SIZE).
inline std::string_view jsc_word(std::uint32_t index) {
  return std::string_view(reinterpret_cast<char const*>(JSC_TEXT) + JSC_OFFSETS[index],
                          JSC_OFFSETS[index + 1] - JSC_OFFSETS[index]);
}

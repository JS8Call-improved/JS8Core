#pragma once

#include <cstdint>

struct JscTuple {
  char const* str;
  int size;
  int index;
};

extern JscTuple const JSC_MAP[262144];
extern std::uint32_t const JSC_SIZE;

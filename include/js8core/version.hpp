#pragma once

#include <string_view>

// Authoritative library version. CMake reads these values for package versioning.
#define JS8CORE_VERSION_MAJOR 0
#define JS8CORE_VERSION_MINOR 1
#define JS8CORE_VERSION_PATCH 0

#define JS8CORE_DETAIL_STRINGIFY_IMPL(value) #value
#define JS8CORE_DETAIL_STRINGIFY(value) JS8CORE_DETAIL_STRINGIFY_IMPL(value)
#define JS8CORE_VERSION_STRING \
  JS8CORE_DETAIL_STRINGIFY(JS8CORE_VERSION_MAJOR) "." \
  JS8CORE_DETAIL_STRINGIFY(JS8CORE_VERSION_MINOR) "." \
  JS8CORE_DETAIL_STRINGIFY(JS8CORE_VERSION_PATCH)

namespace js8core {

// Version of the linked library. Requires no engine initialization.
std::string_view version() noexcept;

}  // namespace js8core

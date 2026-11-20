#include "js8core/version.hpp"

#include <iostream>
#include <string>

static_assert(JS8CORE_VERSION_MAJOR >= 0);
static_assert(JS8CORE_VERSION_MINOR >= 0);
static_assert(JS8CORE_VERSION_PATCH >= 0);
static_assert(noexcept(js8core::version()));

int main() {
  auto const numeric = std::to_string(JS8CORE_VERSION_MAJOR) + "." +
                       std::to_string(JS8CORE_VERSION_MINOR) + "." +
                       std::to_string(JS8CORE_VERSION_PATCH);
  if (numeric != JS8CORE_VERSION_STRING ||
      js8core::version() != JS8CORE_VERSION_STRING) {
    std::cerr << "Header and library versions disagree\n";
    return 1;
  }
  std::cout << "JS8Core " << js8core::version() << '\n';
  return 0;
}

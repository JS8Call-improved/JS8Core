#include "js8core/version.hpp"

namespace js8core {

std::string_view version() noexcept {
  return JS8CORE_VERSION_STRING;
}

}  // namespace js8core

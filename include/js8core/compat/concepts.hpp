#pragma once

#include <type_traits>

// Standard concepts are available on modern C++20 libraries. On older NDK
// toolchains, expose substitutes only in our own namespace, never in std.
#if defined(__has_include)
#  if __has_include(<concepts>)
#    include <concepts>
#  endif
#endif
#if defined(__cpp_lib_concepts) && __cpp_lib_concepts >= 202002L
namespace js8core::compat {
template <typename T> concept floating_point = std::floating_point<T>;
template <typename T> concept integral = std::integral<T>;
template <typename T> concept signed_integral = std::signed_integral<T>;
template <typename T> concept unsigned_integral = std::unsigned_integral<T>;
template <typename T, typename U> concept same_as = std::same_as<T, U>;
}
#else
namespace js8core::compat {
template <typename T> concept floating_point = std::is_floating_point_v<T>;
template <typename T> concept integral = std::is_integral_v<T>;
template <typename T> concept signed_integral = std::is_integral_v<T> && std::is_signed_v<T>;
template <typename T> concept unsigned_integral = std::is_integral_v<T> && !std::is_signed_v<T>;
template <typename T, typename U> concept same_as = std::is_same_v<T, U> && std::is_same_v<U, T>;
}
#endif

#pragma once

// Probe the actual standard header before its feature-test macro. Never add
// definitions to namespace std: doing so collides when a downstream source
// subsequently includes <numbers>.
#if defined(__has_include)
#  if __has_include(<numbers>)
#    include <numbers>
#  endif
#endif

#if defined(__cpp_lib_math_constants) && __cpp_lib_math_constants >= 201907L
namespace js8core::compat { namespace numbers = std::numbers; }
#else
namespace js8core::compat::numbers {
inline constexpr double pi = 3.141592653589793238462643383279502884;
inline constexpr double e = 2.718281828459045235360287471352662498;
inline constexpr double sqrt2 = 1.414213562373095048801688724209698079;
inline constexpr double sqrt3 = 1.732050807568877293527446341505872367;
inline constexpr double inv_pi = 0.318309886183790671537767526745028724;
inline constexpr double inv_sqrtpi = 0.564189583547756286948079451560772586;
inline constexpr double ln2 = 0.693147180559945309417232121458176568;
inline constexpr double ln10 = 2.302585092994045684017991454684364208;
inline constexpr double egamma = 0.577215664901532860606512090082402431;
inline constexpr double phi = 1.618033988749894848204586834365638118;
template <typename T> inline constexpr T pi_v = static_cast<T>(pi);
template <typename T> inline constexpr T e_v = static_cast<T>(e);
template <typename T> inline constexpr T sqrt2_v = static_cast<T>(sqrt2);
template <typename T> inline constexpr T sqrt3_v = static_cast<T>(sqrt3);
template <typename T> inline constexpr T inv_pi_v = static_cast<T>(inv_pi);
template <typename T> inline constexpr T inv_sqrtpi_v = static_cast<T>(inv_sqrtpi);
template <typename T> inline constexpr T ln2_v = static_cast<T>(ln2);
template <typename T> inline constexpr T ln10_v = static_cast<T>(ln10);
template <typename T> inline constexpr T egamma_v = static_cast<T>(egamma);
template <typename T> inline constexpr T phi_v = static_cast<T>(phi);
}  // namespace js8core::compat::numbers
#endif

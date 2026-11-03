#include "js8core/compat/concepts.hpp"
#include "js8core/compat/numbers.hpp"
#include "js8core/protocol/time_utils.hpp"
// Deliberately include both standard headers *after* the compatibility
// headers. This used to trigger redefinition errors on GCC and newer NDKs.
#include <concepts>
#include <numbers>
#include <type_traits>

static_assert(js8core::compat::numbers::pi > 3.1415);
static_assert(js8core::compat::numbers::pi_v<float> > 3.1415f);
static_assert(js8core::compat::integral<int>);
static_assert(js8core::compat::signed_integral<int>);
static_assert(js8core::compat::unsigned_integral<unsigned>);
static_assert(js8core::compat::floating_point<double>);
static_assert(js8core::compat::same_as<int, int>);
static_assert(!js8core::compat::same_as<int, double>);
static_assert(std::integral<int>);
static_assert(std::numbers::pi > 3.1415);

static_assert(js8core::protocol::code_time(12, 34, 56) == 123456);
static_assert(js8core::protocol::decode_time(123456).hour == 12);
static_assert(js8core::protocol::decode_time(123456).minute == 34);
static_assert(js8core::protocol::decode_time(123456).second == 56);

int main() { return 0; }

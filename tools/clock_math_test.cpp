#include "../src/engine/clock_math.hpp"

#include <cstdio>
#include <cstdint>

namespace clock_math = js8core::detail::slot_clock;

int main() {
  int failures = 0;
  const auto verify = [&failures](bool condition, const char* label) {
    if (!condition) {
      std::fprintf(stderr, "FAIL clock math: %s\n", label);
      ++failures;
    }
  };

  constexpr int rate = 12000;
  constexpr int period = 15000;

  // Minute-boundary arithmetic works for both sides of the Unix epoch.
  verify(clock_math::positive_mod(-1, 60000) == 59999, "clock math case 1");
  verify(clock_math::phase_ms(60023, 60000) == 23, "clock math case 2");
  verify(clock_math::sample_at_phase(250, rate) == 3000, "clock math case 3");
  verify(clock_math::sample_at_phase(59999, rate) == 719988, "clock math case 4");

  // Nominal on-time frames preserve the snapshot's existing correction.
  verify(clock_math::suggested_drift_ms(250, 1200, rate, 100, period, 0.) == 250, "clock math case 5");

  // Just after / before the slot boundary: find the nearest correction.
  verify(clock_math::suggested_drift_ms(0, 1320, rate, 100, period, 0.) == -10, "clock math case 6");
  verify(clock_math::suggested_drift_ms(0, 181080, rate, 100, period, 0.) == 10, "clock math case 7");

  // xdt and capture correction are independent components of the estimate.
  verify(clock_math::suggested_drift_ms(0, 1200, rate, 100, period, .25) == -250, "clock math case 8");
  verify(clock_math::suggested_drift_ms(-500, 1200, rate, 100, period, 0.) == -500, "clock math case 9");
  verify(clock_math::suggested_drift_ms(200, 1320, rate, 100, period, 0.) == 190, "clock math case 10");
  if (failures == 0) std::puts("PASS: 10 clock-math checks");
  return failures == 0 ? 0 : 1;
}

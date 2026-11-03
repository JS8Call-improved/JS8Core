#pragma once

#include <chrono>
#include <cmath>
#include <cstdint>

namespace js8core::detail::slot_clock {

// All JS8 cycle positions are residues of UTC milliseconds. Normalize the
// residue explicitly so that timestamps preceding the epoch work as well.
constexpr std::int64_t positive_mod(std::int64_t value, std::int64_t period) {
  const auto residue = value % period;
  return residue < 0 ? residue + period : residue;
}

inline std::int64_t unix_ms(std::chrono::system_clock::time_point instant) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             instant.time_since_epoch()).count();
}

inline std::chrono::system_clock::time_point corrected_time(
    std::int64_t correction_ms) {
  return std::chrono::system_clock::now() +
         std::chrono::milliseconds(correction_ms);
}

inline std::int64_t phase_ms(std::int64_t epoch_ms, std::int64_t cycle_ms) {
  return positive_mod(epoch_ms, cycle_ms);
}

inline int sample_at_phase(std::int64_t phase, int sample_rate) {
  return static_cast<int>(phase * sample_rate / 1000);
}

// Calculate a candidate receiver clock correction from a detected frame.
// The decoder's window origin is measured in 12-kHz samples from the UTC
// minute. xdt is the observed frame timing displacement in seconds.
// Use the signed nearest periodic residual to avoid special casing either
// side of a slot boundary. All inputs must come from a single RX snapshot.
inline int suggested_drift_ms(std::int64_t capture_correction_ms,
                              int window_start_sample, int sample_rate,
                              int start_delay_ms, int cycle_ms,
                              double xdt_seconds) {
  if (sample_rate <= 0 || cycle_ms <= 0 || !std::isfinite(xdt_seconds)) {
    return static_cast<int>(capture_correction_ms);
  }
  const double observed_ms =
      static_cast<double>(window_start_sample) * 1000.0 / sample_rate +
      xdt_seconds * 1000.0 - start_delay_ms;
  const double signed_phase =
      std::remainder(observed_ms, static_cast<double>(cycle_ms));
  const auto adjustment_ms = static_cast<std::int64_t>(
      std::llround(-signed_phase));
  return static_cast<int>((capture_correction_ms + adjustment_ms) % cycle_ms);
}

}  // namespace js8core::detail::slot_clock

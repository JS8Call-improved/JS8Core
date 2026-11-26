#pragma once

namespace js8core::decoder {

// Apply to every decode pass, including OSD. The stricter pass-4 limit is
// specific to that erasure pass.
constexpr bool hard_errors_acceptable(int pass, int hard_errors, float sync) {
  return hard_errors >= 0 && hard_errors < 60 &&
         !(sync < 2.0f && hard_errors > 35) &&
         !(pass > 2 && hard_errors > 39) &&
         !(pass == 4 && hard_errors > 30);
}

}  // namespace js8core::decoder

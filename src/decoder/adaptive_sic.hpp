#pragma once

#include "tracked_reference.hpp"

namespace js8core::decoder {

struct AdaptiveCancellation {
  TrackedTrajectory trajectory;
  std::vector<std::complex<float>> reference;
  std::vector<float> residual;
};

// Check a complete, aligned frame before spending any of the refinement quota.
bool fixed_cancellation_sufficient(
    std::span<float const> audio, std::span<float const> residual,
    std::span<std::complex<float> const> reference, int symbol_samples);

// Build an optional replacement for the ordinary FFT subtraction of a
// CRC-validated frame. A clipped frame stays on the ordinary path.
AdaptiveCancellation propose_adaptive_cancellation(
    std::span<float const> audio, std::array<int, 79> const& tones,
    TrackedTrajectory const& seed, DecodeControl const* control);

// Evaluate all three inputs against the same decoded waveform and support.
bool improves_cancellation(std::span<float const> audio, std::span<float const> baseline,
                           AdaptiveCancellation const& candidate, DecodeControl const* control);

}  // namespace js8core::decoder

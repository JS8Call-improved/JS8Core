#pragma once

#include <array>
#include <complex>
#include <span>
#include <vector>
#include "js8core/decode_control.hpp"

namespace js8core::decoder {

// All sample coordinates refer to the full-rate received window. Frequency is
// the received lowest-tone frequency at the nominal acquisition epoch. Constant
// channel amplitude/phase are deliberately absent: cancellation fits them.
struct TrackedTrajectory {
  double sample_rate_hz = 12000.0;
  int symbol_samples = 0;
  double epoch_sample = 0.0;
  double frequency_hz = 0.0;
  double frequency_hz_per_symbol = 0.0;
  double timing_offset_samples = 0.0;
  double timing_samples_per_symbol = 0.0;
};

// Unit-amplitude analytic reference, zero outside its actual fractional frame
// support. Rejects partial frames, invalid geometry and aliased frequencies.
std::vector<std::complex<float>> make_tracked_reference(
    std::array<int, 79> const& tones, TrackedTrajectory const& trajectory,
    int received_samples, DecodeControl const* control = nullptr);

struct TrackedCancellation {
  // Empty means rejection; the caller's audio is never changed by this helper.
  std::vector<float> residual;
  double removed_fraction = 0.0;
};

// For an already CRC-validated waveform only. Fit smooth local real/imaginary
// channel coefficients, then require a finite net energy reduction of at least
// 3% before returning replacement audio. This is not frame authentication.
TrackedCancellation cancel_tracked_reference(
    std::span<float const> audio, std::span<std::complex<float> const> reference,
    int symbol_samples, DecodeControl const* control = nullptr);

}  // namespace js8core::decoder

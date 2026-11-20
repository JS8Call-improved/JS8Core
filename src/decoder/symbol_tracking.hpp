#pragma once

#include <array>
#include <complex>
#include <span>
#include <vector>

#include "js8core/protocol/costas.hpp"
#include "js8core/decode_control.hpp"

namespace js8core::decoder {

enum class PilotStatus { Unavailable, Weak, SearchLimit, Accepted };

struct PilotMeasurement {
  PilotStatus status = PilotStatus::Unavailable;
  double center_symbol = 0.0;
  double frequency_hz = 0.0;
  double timing_samples = 0.0;
  double coherence = 0.0;
  bool timing_observable = false;
};

struct SymbolCorrection {
  double frequency_hz = 0.0;
  double timing_samples = 0.0;
};

struct PilotTrackingEstimate {
  std::array<PilotMeasurement, 3> pilots{};
  bool frequency_valid = false;
  bool timing_valid = false;
  // Frequency is evaluated at symbol centers; timing at symbol starts.
  double frequency_at_start_hz = 0.0;
  double frequency_hz_per_symbol = 0.0;
  double timing_at_start_samples = 0.0;
  double timing_samples_per_symbol = 0.0;

  // Invalid components and out-of-frame requests return zero correction.
  SymbolCorrection correction(int symbol) const;
};

// Read-only, tone-zero-centered complex baseband. frame_start_sample is the
// acquired start, not necessarily an integer. The span must contain received
// samples only, excluding FFT padding. Each call is independent of prior calls.
// Supported geometry is 12..128 samples/symbol (all current JS8 submodes).
PilotTrackingEstimate estimate_pilot_tracking(
    std::span<std::complex<float> const> channel, double sample_rate_hz,
    int samples_per_symbol, double frame_start_sample,
    protocol::CostasArray const& costas, DecodeControl const* control = nullptr);

// Extract 79 contiguous symbol windows without changing the input. Corrections
// are applied relative to frame_start_sample. Missing interpolation support or
// invalid geometry/evidence returns an empty vector, never zero-padded symbols.
// Zero corrections at an integer start copy the input exactly. The caller may
// disable a correction component by clearing its validity flag in estimate.
std::vector<std::complex<float>> extract_tracked_symbols(
    std::span<std::complex<float> const> channel, double sample_rate_hz,
    int samples_per_symbol, double frame_start_sample,
    PilotTrackingEstimate const& estimate, DecodeControl const* control = nullptr);

}  // namespace js8core::decoder

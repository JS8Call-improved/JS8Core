#pragma once

#include <cstdint>

#include "symbol_tracking.hpp"

namespace js8core::decoder {

struct AidedSymbols {
  std::vector<std::complex<float>> samples;
  double timing_samples = 0.0;
  double frequency_hz = 0.0;
  float sync_power = 0.0f;
};

// Refine the actual ordinary extraction using the tentative data tones.
// Validate on the known Costas symbols before returning fresh complex samples.
AidedSymbols redemodulate_near_miss(
    std::span<std::complex<float> const> channel, int start, int samples_per_symbol,
    double sample_rate, std::array<std::int8_t, 174> const& tentative,
    protocol::CostasArray const& costas, DecodeControl const* control);

}  // namespace js8core::decoder

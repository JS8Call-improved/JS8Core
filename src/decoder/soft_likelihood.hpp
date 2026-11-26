#pragma once

#include "decoder_tables.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <span>

namespace js8core::decoder {

using ToneMagnitudes = std::array<float, data::kToneRows>;
using SymbolLlrs = std::array<float, 3>;
using FrameToneMagnitudes = std::array<std::array<float, data::kChannelSymbols>, data::kToneRows>;
using FrameLlrs = std::array<float, data::kCodeLength>;

struct PilotTone {
  ToneMagnitudes magnitudes;
  std::size_t active_tone;
};

struct SoftToneModel {
  double noise_energy;     // N = E[|Z|²] for a noise-only complex bin.
  double signal_fraction;  // S / (S + N), with S the active tone's excess energy.
};

// Estimate the model from complete, known Costas symbols. Return no model if
// the pilots don't provide enough evidence to distinguish signal from noise.
std::optional<SoftToneModel> estimate_soft_tone_model(std::span<PilotTone const> pilots);

// Marginalize all eight possible tones. Results are in wire order, most
// significant bit first, and positive means one. Invalid inputs give no LLR.
std::optional<SymbolLlrs> soft_symbol_llrs(ToneMagnitudes const& magnitudes,
                                         SoftToneModel const& model);

// Fit the noise model and demap the data in wire order. Ignore any Costas
// symbol not fully observed and emit three erasures for each incomplete data
// symbol. All positions are in downsampled channel samples.
std::optional<FrameLlrs> soft_frame_llrs(FrameToneMagnitudes const& magnitudes,
                                       protocol::CostasArray const& costas,
                                       int frame_start, int samples_per_symbol, int available_samples);

}  // namespace js8core::decoder

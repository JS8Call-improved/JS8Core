#pragma once

#include <array>
#include <complex>
#include <optional>

#include "js8core/decode_control.hpp"
#include "js8core/protocol/costas.hpp"

namespace js8core::decoder {

// FFT coefficients on the same phase-continuous, tone-zero-centered timeline.
using ToneObservations = std::array<std::array<std::complex<float>, 8>, 79>;
using BitLikelihoods = std::array<float, 174>;

struct DisturbanceModel {
  std::array<double, 8> variance{}; // E[|complex noise|^2], not per-quadrature variance
  double amplitude = 0.0;
};

struct PilotChannel {
  std::complex<double> gain{};
  double radians_per_symbol = 0.0;
  double radians_per_tone = 0.0;
  double validation_error = 1.0;
  // Quadratic contribution at +/-39 symbols from the frame midpoint.
  double edge_phase_radians = 0.0;
};

// Diagonal, frame-local model. Not a full covariance or a multipath equalizer.
std::optional<DisturbanceModel> estimate_disturbance(
    ToneObservations const&, protocol::CostasArray const&, bool whiten,
    DecodeControl const* = nullptr);

// Flat channel with affine time phase and tone-dependent timing phase. Fit on
// even pilot columns; odd columns are held out and must predict successfully.
std::optional<PilotChannel> estimate_pilot_channel(
    ToneObservations const&, protocol::CostasArray const&, DisturbanceModel const&,
    DecodeControl const* = nullptr);

// Same extraction phase reference as the affine model. Require a bounded,
// distinct drift solution and better predictions on held-out Costas pilots.
std::optional<PilotChannel> estimate_quadratic_channel(
    ToneObservations const&, protocol::CostasArray const&, DisturbanceModel const&,
    DecodeControl const* = nullptr);

// Marginalize unknown phase (Rician) or use a validated coherent mean. Evidence
// is noise-scaled and clipped, never frame-standardized like the legacy metrics.
std::optional<BitLikelihoods> make_soft_likelihoods(
    ToneObservations const&, DisturbanceModel const&, PilotChannel const* = nullptr,
    DecodeControl const* = nullptr);

// Noncoherent likelihood with a separate variance per tone and a multiplier
// for broadband bursts. Returns no retry when the disturbance is nearly uniform.
std::optional<BitLikelihoods> make_adaptive_likelihoods(
    ToneObservations const&, protocol::CostasArray const&, DisturbanceModel const&,
    DecodeControl const* = nullptr);

} // namespace js8core::decoder

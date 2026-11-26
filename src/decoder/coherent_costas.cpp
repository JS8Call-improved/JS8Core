#include "coherent_costas.hpp"

#include "js8core/compat/numbers.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace js8core::decoder {
namespace {

constexpr double kTau = 2.0 * compat::numbers::pi;
constexpr int kPilotBlockStride = 36;
// Bound allocation and the cost of one hypothesis. Current profiles use
// 12–32 samples per symbol; fewer than eight would alias the eight tones.
constexpr int kMaxSamplesPerSymbol = 128;

}  // namespace

std::optional<CoherentCostasReference> CoherentCostasReference::make(
    int samples_per_symbol, protocol::CostasArray const& costas) {
  if (samples_per_symbol < 8 || samples_per_symbol > kMaxSamplesPerSymbol)
    return std::nullopt;

  CoherentCostasReference reference;
  reference.samples_per_symbol_ = samples_per_symbol;
  for (std::size_t block = 0; block < costas.size(); ++block) {
    auto& waveform = reference.conjugate_templates_[block];
    waveform.reserve(costas[block].size() * samples_per_symbol);
    std::complex<double> phase{1.0, 0.0};
    for (int tone : costas[block]) {
      if (tone < 0 || tone >= 8) return std::nullopt;
      auto const step = std::polar(1.0, -kTau * tone / samples_per_symbol);
      for (int n = 0; n < samples_per_symbol; ++n) {
        waveform.push_back(phase);
        phase *= step;
      }
    }
  }
  return reference;
}

std::optional<CoherentCostasScore> CoherentCostasReference::score(
    std::span<std::complex<float> const> channel, int frame_start,
    double sample_rate_hz, double trial_offset_hz) const {
  if (!std::isfinite(sample_rate_hz) || sample_rate_hz <= 0.0 ||
      !std::isfinite(trial_offset_hz) || std::abs(trial_offset_hz) >= 0.5 * sample_rate_hz)
    return std::nullopt;

  auto const correction = std::polar(1.0, -kTau * (trial_offset_hz / sample_rate_hz));
  CoherentCostasScore result;
  std::array<double, 3> powers{};
  std::size_t usable = 0;
  for (std::size_t block = 0; block < conjugate_templates_.size(); ++block) {
    auto const& waveform = conjugate_templates_[block];
    auto const start = static_cast<std::int64_t>(frame_start) +
        static_cast<std::int64_t>(block) * kPilotBlockStride * samples_per_symbol_;
    if (start < 0 || static_cast<std::size_t>(start) > channel.size() ||
        waveform.size() > channel.size() - static_cast<std::size_t>(start))
      continue;

    auto const samples = channel.subspan(static_cast<std::size_t>(start), waveform.size());
    std::complex<double> sum{}, rotation{1.0, 0.0};
    double energy = 0.0;
    for (std::size_t n = 0; n < waveform.size(); ++n) {
      auto const value = static_cast<std::complex<double>>(samples[n]);
      energy += std::norm(value);
      sum += value * waveform[n] * rotation;
      // Carry CFO correction across symbol boundaries, resetting only when
      // the next block begins (its absolute phase is unknown).
      rotation *= correction;
    }
    if (!(energy > 0.0) || !std::isfinite(energy)) continue;

    // Unit-power template; normalized squared correlation, bounded by
    // Cauchy–Schwarz. Each complete block votes independently of its gain.
    double const coherence = std::clamp(std::norm(sum) / (waveform.size() * energy), 0.0, 1.0);
    result.blocks[block] = coherence;
    powers[usable++] = coherence;
  }
  if (usable < 2) return std::nullopt;
  std::sort(powers.begin(), powers.end());
  result.support = powers[1];
  return result;
}

std::optional<CoherentCostasPeak> CoherentCostasReference::search(
    std::span<std::complex<float> const> channel, int seed_start, double sample_rate_hz,
    double min_offset_hz, double max_offset_hz, DecodeControl const& control) const {
  if (!std::isfinite(sample_rate_hz) || sample_rate_hz <= 0.0 ||
      !std::isfinite(min_offset_hz) || !std::isfinite(max_offset_hz) ||
      min_offset_hz > max_offset_hz)
    return std::nullopt;
  double const tone_hz = sample_rate_hz / samples_per_symbol_;
  double const radius_hz = tone_hz / 2.0;
  min_offset_hz = std::max(min_offset_hz, -radius_hz);
  max_offset_hz = std::min(max_offset_hz, radius_hz);
  if (min_offset_hz > max_offset_hz) return std::nullopt;

  // At most 135 coarse cells and 35 refinement cells. In the channel's
  // supported modes this covers about half a symbol on either side of a seed.
  int const step = std::clamp((samples_per_symbol_ + 7) / 8, 1, 4);
  int const timing_radius = samples_per_symbol_ / 2;
  auto at = [&](std::int64_t time, double hz, std::optional<CoherentCostasPeak>& best) {
    if (!control.allow_optional()) return false;
    if (time < std::numeric_limits<int>::min() || time > std::numeric_limits<int>::max() ||
        std::abs(time - seed_start) > timing_radius)
      return true;
    auto const start = static_cast<int>(time);
    auto const evidence = score(channel, start, sample_rate_hz, hz);
    if (evidence && (!best || evidence->support > best->support))
      best = CoherentCostasPeak{start, hz, evidence->support};
    return true;
  };

  std::optional<CoherentCostasPeak> best;
  // Include both limits even when the configured receive band is narrower
  // than the usual centered frequency grid.
  for (int t = -4; t <= 4; ++t) {
    int const delta = static_cast<int>(std::round(t * (timing_radius / 4.0)));
    for (int f = 0; f <= 14; ++f) {
      double const hz = std::lerp(min_offset_hz, max_offset_hz, f / 14.0);
      if (!at(static_cast<std::int64_t>(seed_start) + delta, hz, best))
        return std::nullopt;
    }
  }
  if (best) {
    auto const center = *best;
    double const step_hz = (max_offset_hz - min_offset_hz) / 56.0;
    for (int t = 1 - step; t < step; ++t) {
      for (int f = -2; f <= 2; ++f) {
        double const hz = std::clamp(center.frequency_offset_hz + f * step_hz,
                                     min_offset_hz, max_offset_hz);
        if (!at(static_cast<std::int64_t>(center.frame_start) + t, hz, best))
          return std::nullopt;
      }
    }
  }
  return control.allow_optional() ? best : std::nullopt;
}

}  // namespace js8core::decoder

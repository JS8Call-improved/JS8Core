#include "aided_redemodulation.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include "decoder_tables.hpp"
#include "js8core/compat/numbers.hpp"

namespace js8core::decoder {
namespace {

constexpr double kTau = 2.0 * compat::numbers::pi;
using data::kChannelSymbols;
using data::kCodeLength;
using data::kDataSymbols;

// Mid-frame timing and carrier corrections and their changes across the
// frame, in samples and Hz respectively.
struct Adjustment {
  double time = 0.0;
  double frequency = 0.0;
  double time_drift = 0.0;
  double frequency_drift = 0.0;
};

double projection(std::span<std::complex<float> const> symbol, int tone) {
  auto const step = std::polar(1.0, -kTau * tone / symbol.size());
  std::complex<double> phase{1.0}, sum{};
  for (auto value : symbol) {
    sum += static_cast<std::complex<double>>(value) * phase;
    phase *= step;
  }
  return std::norm(sum);
}

std::array<double, 3> pilot_power(std::span<std::complex<float> const> frame, int sps,
                                  protocol::CostasArray const& costas) {
  std::array<double, 3> power{};
  for (std::size_t block = 0; block < costas.size(); ++block)
    for (int pilot = 0; pilot < 7; ++pilot)
      power[block] += projection(frame.subspan((block * 36 + pilot) * sps, sps),
                                 costas[block][pilot]);
  return power;
}

}  // namespace

AidedSymbols redemodulate_near_miss(
    std::span<std::complex<float> const> channel, int start, int sps,
    double sample_rate, std::array<std::int8_t, kCodeLength> const& tentative,
    protocol::CostasArray const& costas, DecodeControl const* control) {
  auto const allowed = [&] { return !control || control->allow_optional(); };
  if (!allowed() || start < 0 || sps < 12 || sps > 128 || !(sample_rate > 0.0) ||
      !std::isfinite(sample_rate) || static_cast<std::size_t>(start) + kChannelSymbols * sps > channel.size())
    return {};

  std::array<int, kDataSymbols> tones{};
  for (int i = 0; i < kDataSymbols; ++i)
    tones[i] = 4 * tentative[3 * i] + 2 * tentative[3 * i + 1] + tentative[3 * i + 2];

  // Center the eight tones before interpolating so high and low tones have
  // comparable interpolation error. The final extraction uses the tracker.
  std::vector<std::complex<float>> centered(channel.size());
  auto const step = std::polar(1.0, -kTau * 3.5 / sps);
  std::complex<double> phase{1.0};
  for (std::size_t i = 0; i < channel.size(); ++i) {
    centered[i] = static_cast<std::complex<float>>(phase * static_cast<std::complex<double>>(channel[i]));
    phase *= step;
  }

  double const tone_hz = sample_rate / sps;
  auto score = [&](Adjustment const& trial) {
    double power = 0.0;
    for (int i = 0; i < kDataSymbols; ++i) {
      int const symbol = i + (i < 29 ? 7 : 14);
      double const relative = (symbol - 39.0) / kChannelSymbols;
      double const first = start + symbol * sps + trial.time + relative * trial.time_drift;
      auto const index = static_cast<int>(std::floor(first));
      if (index < 0 || static_cast<std::size_t>(index) + sps >= centered.size()) return 0.0;
      auto const fraction = static_cast<float>(first - index);
      double const frequency = trial.frequency + relative * trial.frequency_drift;
      auto const rotation = std::polar(1.0, -kTau * (tones[i] - 3.5 + frequency / tone_hz) / sps);
      std::complex<double> oscillator{1.0}, sum{};
      double energy = 0.0;
      for (int n = index; n < index + sps; ++n) {
        auto const value = centered[n] + fraction * (centered[n + 1] - centered[n]);
        sum += oscillator * static_cast<std::complex<double>>(value);
        oscillator *= rotation;
        energy += std::norm(value);
      }
      if (energy > 0.0) power += std::norm(sum) / (sps * energy);
    }
    return power;
  };

  Adjustment best;
  double best_score = score(best);
  double const baseline_score = best_score;
  for (double scale : {1.0, 0.5, 0.25, 0.125}) {
    std::array const steps = {scale * sps / 14.0, scale * tone_hz / 10.0,
                              scale * sps / 9.0, scale * tone_hz / 3.0};
    int axis = 0;
    for (auto field : {&Adjustment::time, &Adjustment::frequency, &Adjustment::time_drift,
                       &Adjustment::frequency_drift}) {
      auto const center = best;
      for (double direction : {-1.0, 1.0}) {
        if (!allowed()) return {};
        auto trial = center;
        trial.*field += direction * steps[axis];
        double const fit = score(trial);
        if (fit > best_score) {
          best_score = fit;
          best = trial;
        }
      }
      ++axis;
    }
  }
  if (!(best_score > 1.01 * baseline_score) || !allowed()) return {};

  PilotTrackingEstimate correction;
  correction.timing_valid = true;
  correction.frequency_valid = true;
  correction.timing_samples_per_symbol = best.time_drift / kChannelSymbols;
  correction.timing_at_start_samples = best.time - 39.0 * correction.timing_samples_per_symbol;
  correction.frequency_hz_per_symbol = best.frequency_drift / kChannelSymbols;
  correction.frequency_at_start_hz = best.frequency - 39.5 * correction.frequency_hz_per_symbol;
  auto refined = extract_tracked_symbols(channel, sample_rate, sps, start, correction, control);
  if (refined.empty()) return {};

  auto const before = pilot_power(channel.subspan(start, kChannelSymbols * sps), sps, costas);
  auto const after = pilot_power(refined, sps, costas);
  double original = 0.0, improved = 0.0;
  for (std::size_t block = 0; block < before.size(); ++block) {
    if (!(after[block] >= 0.9 * before[block])) return {};
    original += before[block];
    improved += after[block];
  }
  if (!(improved > 1.005 * original) || !allowed()) return {};
  return {std::move(refined), correction.timing_at_start_samples,
          correction.frequency_at_start_hz, static_cast<float>(improved)};
}

}  // namespace js8core::decoder

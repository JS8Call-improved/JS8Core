#include "adaptive_sic.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include "js8core/compat/numbers.hpp"
#include "js8core/decoder_state.hpp"

namespace js8core::decoder {
namespace {

constexpr int kSymbols = kJs8NumSymbols;
constexpr double kTau = 2.0 * compat::numbers::pi;

bool allowed(DecodeControl const* control) {
  return !control || control->allow_optional();
}

double frame_start(TrackedTrajectory const& trajectory) {
  return trajectory.epoch_sample + trajectory.timing_offset_samples;
}

double symbol_period(TrackedTrajectory const& trajectory) {
  return trajectory.symbol_samples + trajectory.timing_samples_per_symbol;
}

bool complete_frame(TrackedTrajectory const& trajectory, std::size_t samples) {
  double const first = frame_start(trajectory);
  double const period = symbol_period(trajectory);
  return first >= 0.0 && period >= 32.0 && first + kSymbols * period <= samples;
}

// Sparse samples spread across each symbol include both sides of tone changes.
// Coherently combine short blocks so slow fading need not keep the same phase
// over the whole frame. Alternate sample locations are reserved for validation.
double geometry_score(std::span<float const> audio, std::array<int, kSymbols> const& tones,
                      TrackedTrajectory const& trajectory, int fold) {
  if (!complete_frame(trajectory, audio.size())) return 0.0;
  double const first = frame_start(trajectory);
  double const period = symbol_period(trajectory);
  double const carrier = trajectory.frequency_hz / trajectory.sample_rate_hz;
  double const drift = trajectory.frequency_hz_per_symbol /
      (trajectory.sample_rate_hz * trajectory.symbol_samples);
  double score = 0.0;
  for (int block = 0; block < kSymbols; block += 5) {
    // Sample locations stay fixed across hypotheses.
    double cc = 0.0, ss = 0.0, cs = 0.0, xc = 0.0, xs = 0.0;
    int const end = std::min(block + 5, kSymbols);
    for (int symbol = block; symbol < end; ++symbol) {
      for (int tap = fold; tap < 32; tap += 2) {
        double const fraction = (tap + 0.5) / 32.0;
        auto const sample = static_cast<int>(trajectory.epoch_sample +
            (symbol + fraction) * trajectory.symbol_samples);
        if (sample < 0 || static_cast<std::size_t>(sample) >= audio.size()) continue;
        double const position = (sample - first) / period;
        if (position < 0.0 || position >= kSymbols) continue;
        int const index = static_cast<int>(position);
        double const elapsed = sample - trajectory.epoch_sample;
        // Completed symbols contribute integer cycles for every JS8 tone.
        double const phase = kTau * (carrier * elapsed + 0.5 * drift * elapsed * elapsed +
                                     tones[index] * (position - index));
        double const c = std::cos(phase), s = std::sin(phase);
        cc += c * c;
        ss += s * s;
        cs += c * s;
        xc += audio[sample] * c;
        xs += audio[sample] * s;
      }
    }
    double const determinant = cc * ss - cs * cs;
    if (determinant > 0.1 * cc * ss) {
      score += (ss * xc * xc - 2.0 * cs * xc * xs + cc * xs * xs) / determinant;
    }
  }
  return score;
}

TrackedTrajectory refine_geometry(std::span<float const> audio,
                                  std::array<int, kSymbols> const& tones,
                                  TrackedTrajectory const& seed, DecodeControl const* control) {
  TrackedTrajectory best = seed;
  double score = geometry_score(audio, tones, seed, 0);
  double const training_baseline = score;
  double const tone_spacing = seed.sample_rate_hz / seed.symbol_samples;
  // Search frequency and clock rate around the middle of the frame so a
  // change of slope does not also shift the midpoint.
  constexpr double midpoint = (kSymbols - 1) / 2.0;
  for (double scale : {1.0, 0.5, 0.25, 0.125, 0.0625}) {
    for (int axis = 0; axis < 4; ++axis) {
      auto const center = best;
      for (double direction : {-1.0, 1.0}) {
        if (!allowed(control)) return seed;
        double const step = scale * direction;
        auto trial = center;
        switch (axis) {
          case 0:
            trial.timing_offset_samples += step * seed.symbol_samples / 32.0;
            break;
          case 1:
            trial.frequency_hz += step * tone_spacing / 8.0;
            break;
          case 2: {
            double const slope = step * tone_spacing / (4.0 * kSymbols);
            trial.frequency_hz_per_symbol += slope;
            trial.frequency_hz -= midpoint * slope;
            break;
          }
          case 3: {
            double const slope = step * seed.symbol_samples / (32.0 * kSymbols);
            trial.timing_samples_per_symbol += slope;
            trial.timing_offset_samples -= midpoint * slope;
            break;
          }
        }
        double const trial_score = geometry_score(audio, tones, trial, 0);
        if (trial_score > score) {
          score = trial_score;
          best = trial;
        }
      }
    }
  }
  constexpr double margin = 1.002;
  double const validation_baseline = geometry_score(audio, tones, seed, 1);
  double const validation_score = geometry_score(audio, tones, best, 1);
  if (score > margin * training_baseline && validation_score > margin * validation_baseline) {
    // Test clock drift separately from the carrier and alignment changes.
    auto fixed_clock = best;
    fixed_clock.timing_offset_samples += midpoint *
        (best.timing_samples_per_symbol - seed.timing_samples_per_symbol);
    fixed_clock.timing_samples_per_symbol = seed.timing_samples_per_symbol;
    double const fixed_score = geometry_score(audio, tones, fixed_clock, 1);
    // An otherwise well-aligned frame may need only a few samples of drift
    // correction end to end. Check for a smaller, independent gain.
    constexpr double clock_margin = 1.001;
    if (validation_score <= clock_margin * fixed_score) {
      if (fixed_score > margin * validation_baseline &&
          geometry_score(audio, tones, fixed_clock, 0) > margin * training_baseline)
        return fixed_clock;
      return seed;
    }
    return best;
  }
  return seed;
}

}  // namespace

bool fixed_cancellation_sufficient(
    std::span<float const> audio, std::span<float const> residual,
    std::span<std::complex<float> const> reference, int symbol_samples) {
  if (audio.size() != reference.size() || residual.size() != reference.size() ||
      symbol_samples < 32 || audio.size() != kSymbols * static_cast<std::size_t>(symbol_samples))
    return false;

  double input_energy = 0.0, residual_energy = 0.0;
  double input_power = 0.0, residual_power = 0.0;
  for (std::size_t first = 0; first < audio.size(); first += symbol_samples) {
    std::complex<double> input{}, remaining{};
    for (std::size_t n = first; n < first + symbol_samples; ++n) {
      auto const tone = std::conj(static_cast<std::complex<double>>(reference[n]));
      double const before = audio[n], after = residual[n];
      input += tone * before;
      remaining += tone * after;
      input_energy += before * before;
      residual_energy += after * after;
    }
    input_power += std::norm(input);
    residual_power += std::norm(remaining);
  }
  // Require at least 30 dB of matched suppression. Also require substantial
  // broadband suppression: a misaligned reference can miss off-tone residual.
  return std::isfinite(input_power) && std::isfinite(input_energy) &&
      residual_power < 0.001 * input_power && residual_energy < 0.1 * input_energy;
}

AdaptiveCancellation propose_adaptive_cancellation(
    std::span<float const> audio, std::array<int, kSymbols> const& tones,
    TrackedTrajectory const& seed, DecodeControl const* control) {
  if (!allowed(control) || audio.size() > kJs8NtMax * kJs8RxSampleRate ||
      !complete_frame(seed, audio.size()) || seed.symbol_samples < 32 ||
      seed.epoch_sample < 0.0 || seed.epoch_sample >= audio.size() ||
      !std::isfinite(seed.sample_rate_hz) || seed.sample_rate_hz <= 0.0 ||
      !std::isfinite(seed.frequency_hz) || !std::isfinite(seed.frequency_hz_per_symbol))
    return {};
  auto const trajectory = refine_geometry(audio, tones, seed, control);
  auto reference = make_tracked_reference(tones, trajectory, static_cast<int>(audio.size()), control);
  if (reference.empty()) return {};
  auto fit = cancel_tracked_reference(audio, reference, seed.symbol_samples, control);
  if (fit.residual.empty()) return {};
  return {trajectory, std::move(reference), std::move(fit.residual)};
}

bool improves_cancellation(std::span<float const> audio, std::span<float const> baseline,
                           AdaptiveCancellation const& candidate, DecodeControl const* control) {
  if (!allowed(control) || audio.size() != baseline.size() ||
      audio.size() != candidate.reference.size() || audio.size() != candidate.residual.size() ||
      !complete_frame(candidate.trajectory, audio.size()))
    return false;

  double const start = frame_start(candidate.trajectory);
  double const period = symbol_period(candidate.trajectory);
  std::array<double, 3> power{};
  for (int symbol = 0; symbol < kSymbols; ++symbol) {
    if (!allowed(control)) return false;
    int const first = static_cast<int>(std::ceil(start + symbol * period));
    int const last = static_cast<int>(std::ceil(start + (symbol + 1) * period));
    std::array<std::complex<double>, 3> correlations{};
    for (int n = first; n < last; ++n) {
      auto const r = std::conj(static_cast<std::complex<double>>(candidate.reference[n]));
      correlations[0] += r * static_cast<double>(audio[n]);
      correlations[1] += r * static_cast<double>(baseline[n]);
      correlations[2] += r * static_cast<double>(candidate.residual[n]);
    }
    for (std::size_t i = 0; i < power.size(); ++i)
      power[i] += std::norm(correlations[i]);
  }
  // Require a material improvement over both doing nothing and fixed SIC.
  return std::isfinite(power[0]) && std::isfinite(power[1]) &&
      power[2] < 0.95 * std::min(power[0], power[1]) && allowed(control);
}

}  // namespace js8core::decoder

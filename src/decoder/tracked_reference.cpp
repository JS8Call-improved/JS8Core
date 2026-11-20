#include "tracked_reference.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace js8core::decoder {
namespace {
constexpr double kTau = 6.283185307179586476925286766559;
constexpr int kMaximumSamples = 60 * 12000;
}

std::vector<std::complex<float>> make_tracked_reference(
    std::array<int, 79> const& tones, TrackedTrajectory const& t, int received_samples,
    DecodeControl const* control) {
  if (control && !control->allow_optional()) return {};
  std::array const parameters = {t.sample_rate_hz, t.epoch_sample, t.frequency_hz,
      t.frequency_hz_per_symbol, t.timing_offset_samples, t.timing_samples_per_symbol};
  if (!std::all_of(parameters.begin(), parameters.end(), [](double value) {
        return std::isfinite(value);
      }) || t.sample_rate_hz <= 0.0 || t.symbol_samples < 32 ||
      received_samples <= 0 || received_samples > kMaximumSamples) return {};
  double const period = t.symbol_samples + t.timing_samples_per_symbol;
  double const first = t.epoch_sample + t.timing_offset_samples;
  double const last = first + 79.0 * period;
  if (!std::isfinite(last) || period <= 0.0 || first < 0.0 || last > received_samples ||
      std::abs(t.timing_offset_samples) > 0.25 * t.symbol_samples ||
      std::abs(t.timing_offset_samples + 79.0 * t.timing_samples_per_symbol) >
          0.25 * t.symbol_samples) return {};
  double const frequency_first = t.frequency_hz + t.frequency_hz_per_symbol *
      (first - t.epoch_sample) / t.symbol_samples;
  double const frequency_last = t.frequency_hz + t.frequency_hz_per_symbol *
      (last - t.epoch_sample) / t.symbol_samples;
  double const top = std::max(frequency_first, frequency_last) + 7.0 * t.sample_rate_hz / period;
  if (!std::isfinite(top) || std::min(frequency_first, frequency_last) <= 0.0 ||
      top >= 0.5 * t.sample_rate_hz) return {};
  std::array<double, 80> cycles{};
  for (std::size_t i = 0; i < tones.size(); ++i) {
    if (tones[i] < 0 || tones[i] > 7) return {};
    cycles[i + 1] = cycles[i] + tones[i];
  }
  std::vector<std::complex<float>> reference(static_cast<std::size_t>(received_samples));
  for (int n = static_cast<int>(std::ceil(first)); n < static_cast<int>(std::ceil(last)); ++n) {
    if ((n & 1023) == 0 && control && !control->allow_optional()) return {};
    double const position = (n - first) / period;
    auto const symbol = static_cast<std::size_t>(std::floor(position));
    // Integer receiver samples near a rounded endpoint can lie just outside
    // support. Do not extrapolate an additional symbol or index its tone.
    if (symbol >= tones.size()) continue;
    double const elapsed = n - t.epoch_sample;
    double const phase_cycles = cycles[symbol] + tones[symbol] * (position - symbol) +
        (t.frequency_hz / t.sample_rate_hz) * elapsed +
        0.5 * (t.frequency_hz_per_symbol / t.sample_rate_hz) * elapsed * elapsed / t.symbol_samples;
    if (!std::isfinite(phase_cycles)) return {};
    reference[static_cast<std::size_t>(n)] = static_cast<std::complex<float>>(
        std::polar(1.0, std::remainder(kTau * phase_cycles, kTau)));
  }
  if (control && !control->allow_optional()) return {};
  return reference;
}

TrackedCancellation cancel_tracked_reference(
    std::span<float const> audio, std::span<std::complex<float> const> reference,
    int symbol_samples, DecodeControl const* control) {
  TrackedCancellation result;
  if (control && !control->allow_optional()) return result;
  if (audio.empty() || audio.size() > kMaximumSamples || audio.size() != reference.size() ||
      symbol_samples < 32 || static_cast<std::size_t>(symbol_samples) > audio.size() / 4) return result;
  int const count = static_cast<int>(audio.size());
  int first = count, last = 0;
  double input_energy = 0.0;
  for (int n = 0; n < count; ++n) {
    if ((n & 1023) == 0 && control && !control->allow_optional()) return result;
    auto const value = reference[static_cast<std::size_t>(n)];
    if (!std::isfinite(audio[n]) || !std::isfinite(value.real()) || !std::isfinite(value.imag())) return result;
    double const power = std::norm(static_cast<std::complex<double>>(value));
    if (power != 0.0) {
      if (std::abs(power - 1.0) > 1e-5) return result;
      first = std::min(first, n);
      last = n + 1;
    }
    input_energy += static_cast<double>(audio[n]) * audio[n];
  }
  if (first >= last || input_energy <= std::numeric_limits<double>::min()) return result;
  // Reference support is contiguous, not an arbitrary set of selectable tones.
  for (int n = first; n < last; ++n)
    if (std::norm(reference[static_cast<std::size_t>(n)]) == 0.0f) return result;

  int const length = 4 * symbol_samples;
  int const hop = length / 2;
  std::vector<double> hann(static_cast<std::size_t>(length));
  for (int i = 0; i < length; ++i) hann[i] = 0.5 - 0.5 * std::cos(kTau * i / length);
  std::vector<double> modeled(audio.size()), weights(audio.size());
  for (int center = first; center < last + hop; center += hop) {
    if (control && !control->allow_optional()) return result;
    int const start = center - length / 2;
    int const left = std::max(first, start), right = std::min(last, start + length);
    double cc = 0.0, ss = 0.0, cs = 0.0, xc = 0.0, xs = 0.0, energy = 0.0;
    for (int n = left; n < right; ++n) {
      double const w = hann[n - start];
      auto const r = reference[static_cast<std::size_t>(n)];
      cc += w * r.real() * r.real();
      ss += w * r.imag() * r.imag();
      cs += w * r.real() * r.imag();
      xc += w * audio[n] * r.real();
      xs += w * audio[n] * r.imag();
      energy += w * static_cast<double>(audio[n]) * audio[n];
    }
    double a = 0.0, b = 0.0;
    double const determinant = cc * ss - cs * cs;
    if (cc > 0.0 && ss > 0.0 && determinant > 0.1 * cc * ss && energy > 0.0) {
      double const fit_a = (xc * ss - xs * cs) / determinant;
      double const fit_b = (xs * cc - xc * cs) / determinant;
      double const explained = fit_a * xc + fit_b * xs;
      // Reject weak local projections; do not chase noise through a deep fade.
      // These dimensionless development gates are not a calibrated probability.
      if (explained >= energy * std::max(0.01, 16.0 / (cc + ss))) {
        a = fit_a;
        b = fit_b;
      }
    }
    for (int n = left; n < right; ++n) {
      double const w = hann[n - start];
      auto const r = reference[static_cast<std::size_t>(n)];
      modeled[n] += w * (a * r.real() + b * r.imag());
      weights[n] += w;
    }
  }
  std::vector<float> residual(audio.begin(), audio.end());
  double residual_energy = 0.0;
  for (int n = 0; n < count; ++n) {
    if ((n & 1023) == 0 && control && !control->allow_optional()) return result;
    if (weights[n] > 0.0) residual[n] = static_cast<float>(audio[n] - modeled[n] / weights[n]);
    if (!std::isfinite(residual[n])) return result;
    residual_energy += static_cast<double>(residual[n]) * residual[n];
  }
  double const removed = (input_energy - residual_energy) / input_energy;
  if (!std::isfinite(removed) || removed < 0.03) return result;
  if (control && !control->allow_optional()) return result;
  result.removed_fraction = removed;
  result.residual = std::move(residual);
  return result;
}

}  // namespace js8core::decoder

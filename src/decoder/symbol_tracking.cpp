#include "symbol_tracking.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace js8core::decoder {
namespace {

constexpr double kPi = 3.1415926535897932384626433832795;
constexpr double kTau = 2.0 * kPi;
constexpr int kMaxSps = 128;
constexpr int kRadius = 8;
constexpr double kFrequencyRadius = 0.75;  // in tone spacings
constexpr double kTimingRadius = 0.25;    // in symbol durations

struct PilotWindow {
  std::array<std::array<std::complex<double>, kMaxSps + 1>, 7> samples{};
  std::array<double, 7> energy{};
  bool available = false;
};

class FractionalReader {
 public:
  FractionalReader(std::span<std::complex<double> const> samples, double first, int count)
      : samples_(samples) {
    if (!std::isfinite(first) || first < kRadius || count <= 0 ||
        first + count - 1 > static_cast<double>(samples.size()) - kRadius - 1)
      return;
    base_ = static_cast<std::size_t>(std::floor(first));
    double const fraction = first - std::floor(first);
    integer_ = fraction == 0.0;
    if (!integer_) {
      double sum = 0.0;
      for (int tap = 0; tap < 2 * kRadius; ++tap) {
        double const distance = tap - (kRadius - 1) - fraction;
        double const sinc = distance == 0.0 ? 1.0 :
            std::sin(kPi * distance) / (kPi * distance);
        weights_[tap] = sinc * 0.5 * (1.0 + std::cos(kPi * distance / kRadius));
        sum += weights_[tap];
      }
      for (auto& weight : weights_) weight /= sum;
    }
    available_ = true;
  }

  bool available() const { return available_; }
  std::complex<double> at(int offset) const {
    auto const index = base_ + static_cast<std::size_t>(offset);
    if (integer_) return samples_[index];
    std::complex<double> value{};
    for (int tap = 0; tap < 2 * kRadius; ++tap)
      value += weights_[tap] * samples_[index - (kRadius - 1) + tap];
    return value;
  }

 private:
  std::span<std::complex<double> const> samples_;
  std::array<double, 2 * kRadius> weights_{};
  std::size_t base_ = 0;
  bool integer_ = false;
  bool available_ = false;
};

std::vector<std::complex<double>> center_channel(
    std::span<std::complex<float> const> channel, int sps) {
  std::vector<std::complex<double>> centered;
  centered.reserve(channel.size());
  for (std::size_t i = 0; i < channel.size(); ++i) {
    auto const value = channel[i];
    if (!std::isfinite(value.real()) || !std::isfinite(value.imag())) return {};
    double const phase = -kTau * 3.5 * static_cast<double>(i) / sps;
    centered.push_back(static_cast<std::complex<double>>(value) * std::polar(1.0, phase));
  }
  return centered;
}

// Interpolate after centering the eight tones around zero. This matters for
// 12-sample symbols: uncentered upper tones straddle the complex Nyquist wrap.
PilotWindow prepare_window(std::span<std::complex<double> const> centered,
                           int sps, double first) {
  PilotWindow window;
  FractionalReader reader(centered, first, 7 * sps + 1);
  if (!reader.available()) return window;
  for (int pilot = 0; pilot < 7; ++pilot) {
    for (int n = 0; n <= sps; ++n) {
      auto const value = reader.at(pilot * sps + n);
      window.samples[pilot][n] = value;
      double const weight = (n == 0 || n == sps) ? 0.5 : 1.0;
      window.energy[pilot] += weight * std::norm(value);
    }
  }
  window.available = true;
  return window;
}

struct Evidence {
  double score = -1.0;
  int strong_pilots = 0;
};

Evidence correlate(PilotWindow const& window, int sps,
                   std::array<int, 7> const& tones, double frequency_tones) {
  if (!window.available) return {};
  Evidence evidence{0.0, 0};
  for (int pilot = 0; pilot < 7; ++pilot) {
    if (window.energy[pilot] <= std::numeric_limits<double>::min()) continue;
    auto const step = std::polar(1.0, -kTau *
        (tones[pilot] - 3.5 + frequency_tones) / sps);
    std::complex<double> oscillator{1.0, 0.0}, sum{};
    // Trapezoidal integration references the actual symbol boundaries;
    // left-endpoint-only integration biases fractional timing by half a sample.
    for (int n = 0; n <= sps; ++n) {
      double const weight = (n == 0 || n == sps) ? 0.5 : 1.0;
      sum += weight * window.samples[pilot][n] * oscillator;
      oscillator *= step;
    }
    // Cauchy-Schwarz bounds this ratio by one. Noise alone has expected
    // coherence about 1/sps before searching, not close to a pure tone's one.
    double const coherence = std::clamp(
        std::norm(sum) / (sps * window.energy[pilot]), 0.0, 1.0);
    evidence.score += coherence / 7.0;
    if (coherence >= 0.4) ++evidence.strong_pilots;
  }
  return evidence;
}

PilotMeasurement measure(std::span<std::complex<double> const> centered, int sps,
                         double start, int block, std::array<int, 7> const& tones,
                         double tone_hz, DecodeControl const* control) {
  PilotMeasurement result;
  result.center_symbol = block * 36.0 + 3.5;
  double const first = start + block * 36.0 * sps;
  double best_score = -1.0, best_timing = 0.0, best_frequency = 0.0;
  // Fixed work: 9x13 coarse cells, then 5x5 local cells. Time interpolation
  // is shared by all frequency cells in a row.
  auto search = [&](double timing_center, double frequency_center,
                    int time_steps, int frequency_steps,
                    double time_step, double frequency_step) {
    for (int t = -time_steps; t <= time_steps; ++t) {
      double const timing = timing_center + t * time_step;
      if (std::abs(timing) > kTimingRadius * sps) continue;
      auto const window = prepare_window(centered, sps, first + timing);
      for (int f = -frequency_steps; f <= frequency_steps; ++f) {
        if (control && !control->allow_optional()) return;
        double const frequency = frequency_center + f * frequency_step;
        if (std::abs(frequency) > kFrequencyRadius) continue;
        double const score = correlate(window, sps, tones, frequency).score;
        if (score > best_score) {
          best_score = score;
          best_timing = timing;
          best_frequency = frequency;
        }
      }
    }
  };
  search(0.0, 0.0, 4, 6, sps / 16.0, 0.125);
  if (control && !control->allow_optional()) return result;
  if (best_score < 0.0) return result;
  search(best_timing, best_frequency, 2, 2, sps / 64.0, 0.03125);
  if (control && !control->allow_optional()) return result;

  auto score_at = [&](double timing, double frequency) {
    return correlate(prepare_window(centered, sps, first + timing),
                     sps, tones, frequency);
  };
  // A bounded parabola removes the final grid quantization. Flat or inverted
  // peaks receive no refinement; missing neighbors are never padded with zero.
  auto refine = [](double left, double middle, double right, double step) {
    double const curvature = left - 2.0 * middle + right;
    if (left < 0.0 || right < 0.0 || curvature >= -1e-9) return 0.0;
    return std::clamp(0.5 * (left - right) / curvature, -0.5, 0.5) * step;
  };
  constexpr double frequency_step = 0.03125;
  double const timing_step = sps / 64.0;
  best_frequency += refine(score_at(best_timing, best_frequency - frequency_step).score,
                          best_score,
                          score_at(best_timing, best_frequency + frequency_step).score,
                          frequency_step);
  best_score = score_at(best_timing, best_frequency).score;
  best_timing += refine(score_at(best_timing - timing_step, best_frequency).score,
                       best_score,
                       score_at(best_timing + timing_step, best_frequency).score,
                       timing_step);
  auto const peak = score_at(best_timing, best_frequency);
  result.frequency_hz = best_frequency * tone_hz;
  result.timing_samples = best_timing;
  result.coherence = peak.score;
  result.status = PilotStatus::Weak;
  if (peak.score < 0.5 || peak.strong_pilots < 5) return result;
  result.status = PilotStatus::SearchLimit;
  if (std::abs(best_frequency) >= kFrequencyRadius - frequency_step ||
      std::abs(best_timing) >= kTimingRadius * sps - timing_step) return result;

  double const early = score_at(best_timing - sps / 8.0, best_frequency).score;
  double const late = score_at(best_timing + sps / 8.0, best_frequency).score;
  result.timing_observable = early >= 0.0 && late >= 0.0 &&
                            peak.score - 0.5 * (early + late) >= 0.015;
  result.status = PilotStatus::Accepted;
  return result;
}

struct Line { double intercept = 0.0; double slope = 0.0; double residual = 0.0; };

Line fit(std::array<PilotMeasurement, 3> const& pilots, bool frequency) {
  double weight = 0.0, mean_x = 0.0, mean_y = 0.0;
  for (auto const& pilot : pilots) {
    double const y = frequency ? pilot.frequency_hz : pilot.timing_samples;
    weight += pilot.coherence;
    mean_x += pilot.coherence * pilot.center_symbol;
    mean_y += pilot.coherence * y;
  }
  mean_x /= weight;
  mean_y /= weight;
  double covariance = 0.0, variance = 0.0;
  for (auto const& pilot : pilots) {
    double const y = frequency ? pilot.frequency_hz : pilot.timing_samples;
    covariance += pilot.coherence * (pilot.center_symbol - mean_x) * (y - mean_y);
    variance += pilot.coherence * (pilot.center_symbol - mean_x) * (pilot.center_symbol - mean_x);
  }
  Line line;
  line.slope = covariance / variance;
  line.intercept = mean_y - line.slope * mean_x;
  for (auto const& pilot : pilots) {
    double const y = frequency ? pilot.frequency_hz : pilot.timing_samples;
    line.residual = std::max(line.residual,
        std::abs(y - line.intercept - line.slope * pilot.center_symbol));
  }
  return line;
}

}  // namespace

SymbolCorrection PilotTrackingEstimate::correction(int symbol) const {
  if (symbol < 0 || symbol >= 79) return {};
  return {frequency_valid ? frequency_at_start_hz + (symbol + 0.5) * frequency_hz_per_symbol : 0.0,
          timing_valid ? timing_at_start_samples + symbol * timing_samples_per_symbol : 0.0};
}

PilotTrackingEstimate estimate_pilot_tracking(
    std::span<std::complex<float> const> channel, double sample_rate_hz,
    int samples_per_symbol, double frame_start_sample,
    protocol::CostasArray const& costas, DecodeControl const* control) {
  PilotTrackingEstimate result;
  if (control && !control->allow_optional()) return result;
  if (!std::isfinite(sample_rate_hz) || sample_rate_hz <= 0.0 ||
      !std::isfinite(frame_start_sample) || samples_per_symbol < 12 ||
      samples_per_symbol > kMaxSps || channel.empty()) return result;
  for (auto const& block : costas)
    for (int tone : block) if (tone < 0 || tone > 7) return result;

  auto const centered = center_channel(channel, samples_per_symbol);
  if (centered.empty()) return result;
  double const tone_hz = sample_rate_hz / samples_per_symbol;
  for (int block = 0; block < 3; ++block)
    result.pilots[block] = measure(centered, samples_per_symbol, frame_start_sample,
                                   block, costas[block], tone_hz, control);
  if (control && !control->allow_optional()) return {};
  if (!std::all_of(result.pilots.begin(), result.pilots.end(), [](auto const& pilot) {
        return pilot.status == PilotStatus::Accepted;
      })) return result;

  auto const frequency = fit(result.pilots, true);
  auto const timing = fit(result.pilots, false);
  result.frequency_valid = frequency.residual <= 0.06 * tone_hz &&
      std::abs(frequency.intercept) <= kFrequencyRadius * tone_hz &&
      std::abs(frequency.intercept + 79.0 * frequency.slope) <= kFrequencyRadius * tone_hz;
  result.timing_valid = timing.residual <= 0.04 * samples_per_symbol &&
      std::abs(timing.intercept) <= kTimingRadius * samples_per_symbol &&
      std::abs(timing.intercept + 79.0 * timing.slope) <= kTimingRadius * samples_per_symbol &&
      std::all_of(result.pilots.begin(), result.pilots.end(), [](auto const& pilot) {
        return pilot.timing_observable;
      });
  if (result.frequency_valid) {
    result.frequency_at_start_hz = frequency.intercept;
    result.frequency_hz_per_symbol = frequency.slope;
  }
  if (result.timing_valid) {
    result.timing_at_start_samples = timing.intercept;
    result.timing_samples_per_symbol = timing.slope;
  }
  return result;
}

std::vector<std::complex<float>> extract_tracked_symbols(
    std::span<std::complex<float> const> channel, double sample_rate_hz,
    int samples_per_symbol, double frame_start_sample,
    PilotTrackingEstimate const& estimate, DecodeControl const* control) {
  if (control && !control->allow_optional()) return {};
  if (!std::isfinite(sample_rate_hz) || sample_rate_hz <= 0.0 ||
      !std::isfinite(frame_start_sample) || samples_per_symbol < 12 ||
      samples_per_symbol > kMaxSps || channel.empty()) return {};
  double const frequency = estimate.frequency_valid ? estimate.frequency_at_start_hz : 0.0;
  double const slope = estimate.frequency_valid ? estimate.frequency_hz_per_symbol : 0.0;
  double const timing = estimate.timing_valid ? estimate.timing_at_start_samples : 0.0;
  double const timing_slope = estimate.timing_valid ? estimate.timing_samples_per_symbol : 0.0;
  if (!std::isfinite(frequency) || !std::isfinite(slope) ||
      !std::isfinite(timing) || !std::isfinite(timing_slope)) return {};
  double const tone_hz = sample_rate_hz / samples_per_symbol;
  if (std::abs(frequency) > kFrequencyRadius * tone_hz ||
      std::abs(frequency + 79.0 * slope) > kFrequencyRadius * tone_hz ||
      std::abs(timing) > kTimingRadius * samples_per_symbol ||
      std::abs(timing + 79.0 * timing_slope) > kTimingRadius * samples_per_symbol) return {};

  int const count = 79 * samples_per_symbol;
  if (frequency == 0.0 && slope == 0.0 && timing == 0.0 && timing_slope == 0.0 &&
      frame_start_sample >= 0.0 && std::floor(frame_start_sample) == frame_start_sample &&
      frame_start_sample + count <= static_cast<double>(channel.size())) {
    auto const first = static_cast<std::size_t>(frame_start_sample);
    auto const input = channel.subspan(first, static_cast<std::size_t>(count));
    for (auto value : input)
      if (!std::isfinite(value.real()) || !std::isfinite(value.imag())) return {};
    return {input.begin(), input.end()};
  }
  auto const centered = center_channel(channel, samples_per_symbol);
  if (centered.empty()) return {};
  std::vector<std::complex<float>> symbols(static_cast<std::size_t>(count));
  for (int symbol = 0; symbol < 79; ++symbol) {
    if (control && !control->allow_optional()) return {};
    double const first = frame_start_sample + symbol * samples_per_symbol + timing + symbol * timing_slope;
    FractionalReader reader(centered, first, samples_per_symbol);
    if (!reader.available()) return {};
    for (int n = 0; n < samples_per_symbol; ++n) {
      double const position = first + n;
      double const elapsed_symbols = (position - frame_start_sample) / samples_per_symbol;
      // Integrate the frequency trajectory, rather than restarting a constant
      // oscillator at each symbol. Restore the interpolation bank's center too.
      double const cycles = 3.5 * position / samples_per_symbol -
          (frequency * elapsed_symbols + 0.5 * slope * elapsed_symbols * elapsed_symbols) / tone_hz;
      auto const value = reader.at(n) * std::polar(1.0, kTau * cycles);
      auto const sample = static_cast<std::complex<float>>(value);
      if (!std::isfinite(sample.real()) || !std::isfinite(sample.imag())) return {};
      symbols[static_cast<std::size_t>(symbol * samples_per_symbol + n)] = sample;
    }
  }
  if (control && !control->allow_optional()) return {};
  return symbols;
}

}  // namespace js8core::decoder

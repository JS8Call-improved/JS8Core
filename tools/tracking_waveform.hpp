#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <random>
#include <span>
#include <stdexcept>
#include <vector>

namespace js8core::test {

inline constexpr double kWaveTau = 6.283185307179586476925286766559;

// Test-only model. Positive clock_ppm advances transmitter time relative to
// receiver time, shortening symbols and increasing every transmitted frequency.
struct TrackingSignal {
  std::array<int, 79> tones{};
  double symbol_seconds = 0.16;
  double start_seconds = 0.5;
  double base_hz = 1500.0;
  double amplitude = 5000.0;
  double phase_radians = 0.0;
  double frequency_offset_hz = 0.0;
  double frequency_slope_hz_s = 0.0;
  double frequency_wander_hz = 0.0;
  double frequency_wander_period_s = 10.0;
  double clock_ppm = 0.0;
  double timing_wander_seconds = 0.0;
  double timing_wander_period_s = 10.0;
  double fade_depth = 0.0;
  double fade_center_s = 1.0;
  double fade_width_s = 0.5;
};

// Integrate the piecewise-constant FSK tones exactly, then evaluate that phase
// on a warped time axis. No decoder, modulator or desktop implementation is used.
class TrackingWaveform {
 public:
  explicit TrackingWaveform(TrackingSignal signal) : signal_(signal) {
    auto const& s = signal_;
    const std::array parameters = {
        s.symbol_seconds, s.start_seconds, s.base_hz, s.amplitude,
        s.phase_radians, s.frequency_offset_hz, s.frequency_slope_hz_s,
        s.frequency_wander_hz, s.frequency_wander_period_s, s.clock_ppm,
        s.timing_wander_seconds, s.timing_wander_period_s, s.fade_depth,
        s.fade_center_s, s.fade_width_s};
    if (!std::all_of(parameters.begin(), parameters.end(),
                     [](double x) { return std::isfinite(x); }) ||
        s.symbol_seconds <= 0.0 || s.amplitude < 0.0 ||
        s.frequency_wander_period_s <= 0.0 || s.timing_wander_period_s <= 0.0 ||
        s.fade_width_s <= 0.0 || s.fade_depth < 0.0 || s.fade_depth > 1.0 ||
        s.clock_ppm <= -1e6 ||
        1.0 + s.clock_ppm * 1e-6 <=
            std::abs(s.timing_wander_seconds * kWaveTau / s.timing_wander_period_s)) {
      throw std::invalid_argument("invalid tracking waveform parameters");
    }
    for (std::size_t i = 0; i < s.tones.size(); ++i) {
      if (s.tones[i] < 0 || s.tones[i] > 7)
        throw std::invalid_argument("invalid FSK tone");
      tone_cycles_[i + 1] = tone_cycles_[i] + s.tones[i];
    }
  }

  double transmitter_time(double receiver_elapsed_s) const {
    return receiver_elapsed_s * (1.0 + signal_.clock_ppm * 1e-6) +
           signal_.timing_wander_seconds *
               std::sin(kWaveTau * receiver_elapsed_s / signal_.timing_wander_period_s);
  }

  std::complex<double> at(double receiver_time_s) const {
    auto const& s = signal_;
    double const elapsed = receiver_time_s - s.start_seconds;
    double const tx = transmitter_time(elapsed);
    if (tx < 0.0 || tx >= s.tones.size() * s.symbol_seconds) return {};
    double const position = tx / s.symbol_seconds;
    auto const symbol = static_cast<std::size_t>(position);
    double const cycles = s.base_hz * tx + tone_cycles_[symbol] +
                          s.tones[symbol] * (position - symbol);
    double const wander_omega = kWaveTau / s.frequency_wander_period_s;
    double const residual_cycles = s.frequency_offset_hz * elapsed +
        0.5 * s.frequency_slope_hz_s * elapsed * elapsed +
        s.frequency_wander_hz * (1.0 - std::cos(wander_omega * elapsed)) / wander_omega;
    double const fade_distance = (elapsed - s.fade_center_s) / s.fade_width_s;
    double const gain = 1.0 - s.fade_depth * std::exp(-0.5 * fade_distance * fade_distance);
    return std::polar(s.amplitude * gain,
                      std::remainder(s.phase_radians + kWaveTau * (cycles + residual_cycles),
                                     kWaveTau));
  }

 private:
  TrackingSignal signal_;
  std::array<double, 80> tone_cycles_{};
};

// Mix in floating point before clipping once. Other entries may be interfering
// JS8 frames with independent starts, clocks, phases and frequencies.
inline std::vector<std::int16_t> render_tracking_audio(
    std::span<TrackingWaveform const> signals, int rate, int count,
    double noise_sigma = 0.0, std::uint32_t seed = 1) {
  if (rate <= 0 || count < 0 || !std::isfinite(noise_sigma) || noise_sigma < 0.0)
    throw std::invalid_argument("invalid tracking audio parameters");
  std::mt19937 random(seed);
  std::normal_distribution<double> noise(0.0, 1.0);
  std::vector<std::int16_t> result(static_cast<std::size_t>(count));
  for (int i = 0; i < count; ++i) {
    double value = noise_sigma > 0.0 ? noise_sigma * noise(random) : 0.0;
    for (auto const& signal : signals) value += signal.at(static_cast<double>(i) / rate).imag();
    result[static_cast<std::size_t>(i)] =
        static_cast<std::int16_t>(std::lround(std::clamp(value, -32768.0, 32767.0)));
  }
  return result;
}

}  // namespace js8core::test

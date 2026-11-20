#include "tracking_waveform.hpp"
#include "src/decoder/symbol_tracking.hpp"

#include <cstdio>
#include <limits>

using namespace js8core;

namespace {
struct Geometry {
  char name;
  int sps;
  double duration;
  protocol::CostasType costas;
};
constexpr Geometry kModes[] = {
    {'A', 32, 0.160, protocol::CostasType::Original},
    {'B', 20, 0.100, protocol::CostasType::Modified},
    {'C', 12, 0.050, protocol::CostasType::Modified},
    {'E', 32, 0.320, protocol::CostasType::Modified},
    {'I', 12, 0.032, protocol::CostasType::Modified},
};

test::TrackingSignal make_signal(Geometry mode) {
  test::TrackingSignal signal;
  signal.symbol_seconds = mode.duration;
  signal.start_seconds = 2.0 * mode.duration;
  signal.base_hz = 0.0;
  signal.amplitude = 1.0;
  signal.phase_radians = 0.61;
  for (std::size_t i = 0; i < signal.tones.size(); ++i) signal.tones[i] = i % 2 ? 7 : 0;
  auto const& costas = protocol::costas(mode.costas);
  for (int block = 0; block < 3; ++block)
    for (int pilot = 0; pilot < 7; ++pilot)
      signal.tones[block * 36 + pilot] = costas[block][pilot];
  return signal;
}

std::vector<std::complex<float>> channel_for(test::TrackingSignal signal,
                                            Geometry mode, double noise = 0.0,
                                            unsigned seed = 1) {
  double const rate = mode.sps / mode.duration;
  test::TrackingWaveform waveform(signal);
  std::vector<std::complex<float>> samples(static_cast<std::size_t>(84 * mode.sps));
  std::mt19937 random(seed);
  std::normal_distribution<double> gaussian(0.0, 1.0);
  for (std::size_t i = 0; i < samples.size(); ++i) {
    auto value = waveform.at(i / rate);
    if (noise > 0.0)
      value += std::complex<double>{noise * gaussian(random), noise * gaussian(random)};
    samples[i] = static_cast<std::complex<float>>(value);
  }
  return samples;
}
}  // namespace

int main() {
  int failures = 0, checks = 0;
  auto check = [&](bool condition, char const* name, char mode = '-') {
    ++checks;
    if (!condition) { std::fprintf(stderr, "FAIL mode %c: %s\n", mode, name); ++failures; }
  };
  for (auto mode : kModes) {
    double const rate = mode.sps / mode.duration;
    double const tone_hz = 1.0 / mode.duration;
    auto const& costas = protocol::costas(mode.costas);
    for (double sign : {-1.0, 1.0}) {
      for (double noise : {0.0, 0.2}) {
        auto signal = make_signal(mode);
        signal.start_seconds += sign * 0.07 * mode.duration;
        signal.frequency_offset_hz = sign * 0.13 * tone_hz;
        signal.frequency_slope_hz_s = sign * 0.20 * tone_hz / (79.0 * mode.duration);
        signal.clock_ppm = sign * 1200.0;
        auto samples = channel_for(signal, mode, noise, 123);
        auto const estimate = decoder::estimate_pilot_tracking(samples, rate, mode.sps,
                                                               2.0 * mode.sps, costas);
        check(estimate.frequency_valid, "frequency trajectory accepted", mode.name);
        check(estimate.timing_valid, "timing trajectory accepted", mode.name);
        for (int symbol : {0, 39, 78}) {
          auto const correction = estimate.correction(symbol);
          double const elapsed = (symbol + 0.5) * mode.duration;
          double const expected_frequency = signal.frequency_offset_hz +
              signal.frequency_slope_hz_s * elapsed;
          double const expected_timing = (signal.start_seconds +
              symbol * mode.duration / (1.0 + signal.clock_ppm * 1e-6)) * rate -
              (2.0 + symbol) * mode.sps;
          check(std::abs(correction.frequency_hz - expected_frequency) < 0.035 * tone_hz,
                "frequency estimate accuracy", mode.name);
          if (std::abs(correction.timing_samples - expected_timing) >= 0.035 * mode.sps)
            std::fprintf(stderr, "  symbol=%d sign=%.0f noise=%.1f timing=%.4f expected=%.4f\n",
                         symbol, sign, noise, correction.timing_samples, expected_timing);
          check(std::abs(correction.timing_samples - expected_timing) < 0.035 * mode.sps,
                "fractional timing estimate accuracy", mode.name);
        }
        check(estimate.correction(-1).frequency_hz == 0.0 &&
              estimate.correction(79).timing_samples == 0.0, "out-of-frame bypass", mode.name);
      }
    }

    auto signal = make_signal(mode);
    auto samples = channel_for(signal, mode);
    auto stationary = decoder::estimate_pilot_tracking(samples, rate, mode.sps, 2.0 * mode.sps, costas);
    check(stationary.frequency_valid && stationary.timing_valid, "stationary accepted", mode.name);
    check(std::abs(stationary.correction(39).frequency_hz) < 0.01 * tone_hz &&
          std::abs(stationary.correction(39).timing_samples) < 0.02 * mode.sps,
          "stationary near-zero correction", mode.name);
    for (double scale : {1e-10, 1e10}) {
      auto scaled = samples;
      for (auto& sample : scaled) sample *= static_cast<float>(scale);
      auto const estimate = decoder::estimate_pilot_tracking(scaled, rate, mode.sps,
                                                              2.0 * mode.sps, costas);
      check(estimate.frequency_valid && estimate.timing_valid,
            "amplitude-independent pilot confidence", mode.name);
      check(std::abs(estimate.correction(39).timing_samples -
                     stationary.correction(39).timing_samples) < 1e-5 * mode.sps,
            "amplitude-independent timing estimate", mode.name);
    }
    auto fractional_acquisition = decoder::estimate_pilot_tracking(samples, rate, mode.sps,
                                                                    2.0 * mode.sps + 0.37, costas);
    check(fractional_acquisition.frequency_valid && fractional_acquisition.timing_valid,
          "fractional acquisition accepted", mode.name);
    check(std::abs(fractional_acquisition.correction(39).timing_samples + 0.37) < 0.02 * mode.sps,
          "timing correction is relative to fractional acquisition", mode.name);

    for (unsigned seed = 0; seed < 10; ++seed) {
      auto noise_signal = signal;
      noise_signal.amplitude = 0.0;
      auto noise = channel_for(noise_signal, mode, 1.0, seed);
      auto rejected = decoder::estimate_pilot_tracking(noise, rate, mode.sps, 2.0 * mode.sps, costas);
      check(!rejected.frequency_valid && !rejected.timing_valid, "noise rejected", mode.name);
      check(rejected.correction(39).frequency_hz == 0.0 &&
            rejected.correction(39).timing_samples == 0.0, "rejected evidence bypasses", mode.name);
    }

    auto flat_signal = signal;
    flat_signal.tones.fill(3);
    auto flat = channel_for(flat_signal, mode);
    auto rejected = decoder::estimate_pilot_tracking(flat, rate, mode.sps, 2.0 * mode.sps, costas);
    check(!rejected.frequency_valid && !rejected.timing_valid, "single tone rejected", mode.name);

    auto too_far = signal;
    too_far.frequency_offset_hz = 0.9 * tone_hz;
    rejected = decoder::estimate_pilot_tracking(channel_for(too_far, mode), rate, mode.sps,
                                                 2.0 * mode.sps, costas);
    check(!rejected.frequency_valid && !rejected.timing_valid, "frequency search limit", mode.name);
    too_far = signal;
    too_far.start_seconds += 0.4 * mode.duration;
    rejected = decoder::estimate_pilot_tracking(channel_for(too_far, mode), rate, mode.sps,
                                                 2.0 * mode.sps, costas);
    check(!rejected.frequency_valid && !rejected.timing_valid, "timing search limit", mode.name);

    // Missing trailing pilots must not be replaced by zeros from FFT padding.
    samples.resize(74 * mode.sps);
    rejected = decoder::estimate_pilot_tracking(samples, rate, mode.sps, 2.0 * mode.sps, costas);
    check(!rejected.frequency_valid && !rejected.timing_valid, "partial frame rejected", mode.name);
    samples = channel_for(signal, mode);
    auto const first = samples.begin() + 38 * mode.sps;
    std::fill(first, first + 7 * mode.sps, std::complex<float>{});
    rejected = decoder::estimate_pilot_tracking(samples, rate, mode.sps, 2.0 * mode.sps, costas);
    check(!rejected.frequency_valid && !rejected.timing_valid, "missing middle pilots rejected", mode.name);

    // A shifted middle block is inconsistent with one affine time trajectory.
    samples = channel_for(signal, mode);
    auto shifted_signal = signal;
    shifted_signal.start_seconds += 0.18 * mode.duration;
    auto const shifted = channel_for(shifted_signal, mode);
    std::copy(shifted.begin() + 37 * mode.sps, shifted.begin() + 46 * mode.sps,
              samples.begin() + 37 * mode.sps);
    rejected = decoder::estimate_pilot_tracking(samples, rate, mode.sps, 2.0 * mode.sps, costas);
    check(std::all_of(rejected.pilots.begin(), rejected.pilots.end(), [](auto const& pilot) {
            return pilot.status == decoder::PilotStatus::Accepted;
          }), "timing outlier has individually accepted pilots", mode.name);
    check(!rejected.timing_valid, "inconsistent timing anchors rejected", mode.name);

    samples = channel_for(signal, mode);
    auto shifted_frequency = signal;
    shifted_frequency.frequency_offset_hz = 0.3 * tone_hz;
    auto const middle_offset = channel_for(shifted_frequency, mode);
    std::copy(middle_offset.begin() + 37 * mode.sps, middle_offset.begin() + 46 * mode.sps,
              samples.begin() + 37 * mode.sps);
    rejected = decoder::estimate_pilot_tracking(samples, rate, mode.sps, 2.0 * mode.sps, costas);
    check(std::all_of(rejected.pilots.begin(), rejected.pilots.end(), [](auto const& pilot) {
            return pilot.status == decoder::PilotStatus::Accepted;
          }), "frequency outlier has individually accepted pilots", mode.name);
    check(!rejected.frequency_valid, "inconsistent frequency anchors rejected", mode.name);

    std::fill(samples.begin(), samples.end(), std::complex<float>{});
    rejected = decoder::estimate_pilot_tracking(samples, rate, mode.sps, 2.0 * mode.sps, costas);
    check(!rejected.frequency_valid && !rejected.timing_valid, "silence rejected", mode.name);
    samples[10] = {std::numeric_limits<float>::quiet_NaN(), 0.0f};
    rejected = decoder::estimate_pilot_tracking(samples, rate, mode.sps, 2.0 * mode.sps, costas);
    check(!rejected.frequency_valid && !rejected.timing_valid, "nonfinite samples rejected", mode.name);
  }

  auto const costas = protocol::costas(protocol::CostasType::Original);
  for (double start : {-1e100, 1e100, std::numeric_limits<double>::quiet_NaN()}) {
    std::array<std::complex<float>, 100> samples{};
    auto const result = decoder::estimate_pilot_tracking(samples, 200.0, 32, start, costas);
    check(!result.frequency_valid && !result.timing_valid, "invalid start rejected");
  }
  auto const empty = decoder::estimate_pilot_tracking({}, 200.0, 32, 0.0, costas);
  check(!empty.frequency_valid && !empty.timing_valid, "empty input rejected");
  auto mode = kModes[0];
  auto signal = make_signal(mode);
  auto samples = channel_for(signal, mode);
  for (double rate : {0.0, -200.0, std::numeric_limits<double>::infinity(),
                       std::numeric_limits<double>::quiet_NaN()}) {
    auto const invalid = decoder::estimate_pilot_tracking(samples, rate, 32, 64.0, costas);
    check(!invalid.frequency_valid && !invalid.timing_valid, "invalid rate rejected");
  }
  for (int sps : {-1, 0, 11, 129, std::numeric_limits<int>::max()}) {
    auto const invalid = decoder::estimate_pilot_tracking(samples, 200.0, sps, 64.0, costas);
    check(!invalid.frequency_valid && !invalid.timing_valid, "invalid symbol geometry rejected");
  }
  auto invalid_costas = costas;
  invalid_costas[2][6] = 8;
  auto const invalid = decoder::estimate_pilot_tracking(samples, 200.0, 32, 64.0, invalid_costas);
  check(!invalid.frequency_valid && !invalid.timing_valid, "invalid pilot tone rejected");
  auto const repeated = decoder::estimate_pilot_tracking(samples, 200.0, 32, 64.0, costas);
  check(repeated.frequency_valid && repeated.timing_valid, "invalid calls do not leak state");
  std::printf("%d estimator checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}

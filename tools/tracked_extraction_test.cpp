#include "tracking_waveform.hpp"
#include "src/decoder/symbol_tracking.hpp"

#include <cstdio>
#include <limits>

using namespace js8core;

int main() {
  int failures = 0, checks = 0;
  auto check = [&](bool condition, char const* label) {
    ++checks;
    if (!condition) { std::fprintf(stderr, "FAIL extraction: %s\n", label); ++failures; }
  };
  for (int sps : {12, 20, 32}) {
    constexpr double rate = 200.0;
    double const duration = sps / rate;
    for (double sign : {-1.0, 1.0}) {
      for (int components : {1, 2, 3}) {
        bool const frequency = components & 1;
        bool const timing = components & 2;
        test::TrackingSignal signal;
        signal.symbol_seconds = duration;
        signal.start_seconds = 2.0 * duration + (timing ? sign * 0.071 * duration : 0.0);
        signal.base_hz = 0.0;
        signal.amplitude = 1.0;
        signal.phase_radians = 0.4;
        signal.frequency_offset_hz = frequency ? sign * 0.15 / duration : 0.0;
        signal.frequency_slope_hz_s = frequency ? sign * 0.3 / (79.0 * duration * duration) : 0.0;
        signal.clock_ppm = timing ? sign * 1200.0 : 0.0;
        for (std::size_t i = 0; i < signal.tones.size(); ++i)
          signal.tones[i] = static_cast<int>((3 * i) % 8);
        test::TrackingWaveform waveform(signal);
        std::vector<std::complex<float>> channel(static_cast<std::size_t>(84 * sps));
        for (std::size_t i = 0; i < channel.size(); ++i)
          channel[i] = static_cast<std::complex<float>>(waveform.at(i / rate));
        auto const original = channel;
        decoder::PilotTrackingEstimate estimate;
        estimate.frequency_valid = frequency;
        estimate.timing_valid = timing;
        estimate.frequency_at_start_hz = signal.frequency_offset_hz -
            signal.frequency_slope_hz_s * (signal.start_seconds - 2.0 * duration);
        estimate.frequency_hz_per_symbol = signal.frequency_slope_hz_s * duration;
        estimate.timing_at_start_samples = (signal.start_seconds - 2.0 * duration) * rate;
        estimate.timing_samples_per_symbol = sps / (1.0 + signal.clock_ppm * 1e-6) - sps;
        auto const symbols = decoder::extract_tracked_symbols(channel, rate, sps, 2.0 * sps, estimate);
        check(symbols.size() == static_cast<std::size_t>(79 * sps), "complete frame extracted");
        check(channel == original, "read-only input");
        if (symbols.empty()) continue;
        for (int symbol = 0; symbol < 79; ++symbol) {
          double maximum = -1.0, expected = 0.0;
          int winner = -1;
          for (int tone = 0; tone < 8; ++tone) {
            std::complex<double> sum{};
            for (int n = 0; n < sps; ++n)
              sum += static_cast<std::complex<double>>(symbols[symbol * sps + n]) *
                  std::polar(1.0, -test::kWaveTau * tone * n / sps);
            double const power = std::abs(sum);
            if (tone == signal.tones[symbol]) expected = power;
            if (power > maximum) { maximum = power; winner = tone; }
          }
          check(winner == signal.tones[symbol], "correct FFT tone including upper bank");
          // Abrupt frame gating is not bandlimited; interpolation at onset/end
          // has a larger error than an interior tone transition in this model.
          double const minimum = (symbol == 0 || symbol == 78) ? 0.90 : 0.94;
          check(expected > minimum * sps, "coherent tone energy preserved");
        }
      }
    }

    std::vector<std::complex<float>> samples(static_cast<std::size_t>(79 * sps));
    for (std::size_t i = 0; i < samples.size(); ++i)
      samples[i] = {static_cast<float>(i % 17), -static_cast<float>(i % 13)};
    decoder::PilotTrackingEstimate zero;
    auto copied = decoder::extract_tracked_symbols(samples, rate, sps, 0.0, zero);
    check(copied == samples, "zero-correction integer path is bit-exact and needs no margin");
    samples.pop_back();
    check(decoder::extract_tracked_symbols(samples, rate, sps, 0.0, zero).empty(),
          "missing final sample is not padded");
    samples.resize(84 * sps);
    zero.timing_valid = true;
    zero.timing_at_start_samples = 0.37;
    check(decoder::extract_tracked_symbols(samples, rate, sps, 0.0, zero).empty(),
          "missing interpolation prefix is rejected");
    check(!decoder::extract_tracked_symbols(samples, rate, sps, 2.0 * sps, zero).empty(),
          "fractional timing accepts sufficient margins");
    zero.timing_at_start_samples = std::numeric_limits<double>::quiet_NaN();
    check(decoder::extract_tracked_symbols(samples, rate, sps, 2.0 * sps, zero).empty(),
          "nonfinite trajectory rejected");
    zero = {};
    zero.frequency_valid = true;
    zero.frequency_at_start_hz = rate;
    check(decoder::extract_tracked_symbols(samples, rate, sps, 2.0 * sps, zero).empty(),
          "out-of-envelope trajectory rejected");
    zero = {};
    samples[0] = {std::numeric_limits<float>::infinity(), 0.0f};
    check(decoder::extract_tracked_symbols(samples, rate, sps, 0.0, zero).empty(),
          "nonfinite input rejected on bypass path");
  }
  check(decoder::extract_tracked_symbols({}, 200.0, 32, 0.0, {}).empty(), "empty input rejected");
  std::printf("%d extraction checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}

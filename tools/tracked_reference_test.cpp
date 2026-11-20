#include "tracking_waveform.hpp"
#include "src/decoder/tracked_reference.hpp"

#include <cstdio>
#include <limits>

#include "js8core/protocol/submode.hpp"

using namespace js8core;

namespace {
decoder::TrackedTrajectory truth(test::TrackingSignal const& signal, int sps) {
  constexpr double rate = 12000.0;
  decoder::TrackedTrajectory t;
  t.symbol_samples = sps;
  t.epoch_sample = signal.start_seconds * rate - 3.7;
  t.timing_offset_samples = 3.7;
  t.timing_samples_per_symbol = sps / (1.0 + signal.clock_ppm * 1e-6) - sps;
  t.frequency_hz = signal.base_hz * (1.0 + signal.clock_ppm * 1e-6) + signal.frequency_offset_hz +
      signal.frequency_slope_hz_s * (t.epoch_sample / rate - signal.start_seconds);
  t.frequency_hz_per_symbol = signal.frequency_slope_hz_s * sps / rate;
  return t;
}

double squared_error(std::span<float const> a, std::span<std::int16_t const> b) {
  double sum = 0.0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    double const difference = a[i] - b[i];
    sum += difference * difference;
  }
  return sum;
}

double amplitude(std::span<float const> audio, test::TrackingWaveform const& waveform) {
  std::complex<double> sum{};
  double energy = 0.0;
  for (std::size_t i = 0; i < audio.size(); ++i) {
    auto const value = waveform.at(i / 12000.0);
    sum += static_cast<double>(audio[i]) * std::conj(value);
    energy += std::norm(value);
  }
  return energy > 0.0 ? 2.0 * std::abs(sum) / energy : 0.0;
}
}  // namespace

int main() {
  int checks = 0, failures = 0;
  auto check = [&](bool condition, char const* label) {
    ++checks;
    if (!condition) { std::fprintf(stderr, "FAIL tracked reference: %s\n", label); ++failures; }
  };
  for (auto const& mode : protocol::submodes()) {
    int const count = 12000 * mode.tx_seconds;
    for (double sign : {-1.0, 1.0}) {
      test::TrackingSignal signal;
      signal.symbol_seconds = mode.symbol_samples / 12000.0;
      signal.start_seconds = mode.start_delay_ms / 1000.0 + 0.013037;
      signal.base_hz = 1501.7;
      signal.amplitude = 1.0;
      signal.phase_radians = 0.83;
      signal.clock_ppm = sign * 1500.0;
      signal.frequency_offset_hz = sign * 1.2;
      signal.frequency_slope_hz_s = sign * 0.35 /
          (79.0 * signal.symbol_seconds * signal.symbol_seconds);
      for (std::size_t i = 0; i < signal.tones.size(); ++i)
        signal.tones[i] = static_cast<int>((3 * i + i / 7) % 8);
      auto const model = truth(signal, mode.symbol_samples);
      auto const reference = decoder::make_tracked_reference(signal.tones, model, count);
      check(reference.size() == static_cast<std::size_t>(count), "complete fractional support");
      if (reference.empty()) continue;
      test::TrackingWaveform waveform(signal);
      std::complex<double> phase{};
      double maximum_error = 0.0;
      bool support_matches = true;
      for (int n = 0; n < count; ++n) {
        auto const expected = waveform.at(n / 12000.0);
        auto const actual = static_cast<std::complex<double>>(reference[n]);
        if (std::abs(expected) == 0.0) { support_matches &= actual == std::complex<double>{}; continue; }
        if (std::abs(actual) == 0.0) { support_matches = false; continue; }
        if (phase == std::complex<double>{}) phase = expected / actual;
        maximum_error = std::max(maximum_error, std::abs(expected - phase * actual));
      }
      check(support_matches, "reference support matches independent time warp");
      check(maximum_error < 2e-6, "continuous phase matches independent chirp/clock waveform");

      signal.amplitude = 4500.0;
      test::TrackingWaveform strong(signal);
      auto weak_signal = signal;
      weak_signal.amplitude = 900.0;
      weak_signal.base_hz += 2.3 / signal.symbol_seconds;
      weak_signal.start_seconds += 0.13 * signal.symbol_seconds;
      weak_signal.clock_ppm = -sign * 300.0;
      weak_signal.frequency_slope_hz_s = 0.0;
      weak_signal.phase_radians = -0.4;
      for (auto& tone : weak_signal.tones) tone = (tone + 3) % 8;
      test::TrackingWaveform weak(weak_signal);
      std::array const waves = {strong, weak};
      auto const pcm = test::render_tracking_audio(waves, 12000, count, 2000.0, 123);
      auto const expected = test::render_tracking_audio(std::span(&weak, 1), 12000, count, 2000.0, 123);
      std::vector<float> input(pcm.begin(), pcm.end());
      auto const unchanged = input;
      auto const result = decoder::cancel_tracked_reference(input, reference, mode.symbol_samples);
      check(!result.residual.empty(), "matched tracked cancellation accepted");
      check(input == unchanged, "candidate cancellation never mutates caller audio");
      if (!result.residual.empty()) {
        check(result.removed_fraction >= 0.03 && result.removed_fraction <= 1.0, "finite energy reduction");
        check(squared_error(result.residual, expected) < 0.03 * squared_error(input, expected),
              "at least 97 percent of target error removed");
        std::vector<float> weak_only(expected.begin(), expected.end());
        double const before = amplitude(weak_only, weak);
        double const after = amplitude(result.residual, weak);
        check(std::abs(after / before - 1.0) < 0.08, "weaker overlapping signal amplitude preserved");
      }
      auto const noise = test::render_tracking_audio({}, 12000, count, 2000.0, 123);
      input.assign(noise.begin(), noise.end());
      check(decoder::cancel_tracked_reference(input, reference, mode.symbol_samples).residual.empty(),
            "noise projection rejected by energy guard");
      auto wrong_model = model;
      wrong_model.frequency_hz += 12.0 / signal.symbol_seconds;
      auto const wrong = decoder::make_tracked_reference(signal.tones, wrong_model, count);
      check(!wrong.empty(), "wrong-carrier test geometry is valid");
      input.assign(pcm.begin(), pcm.end());
      check(decoder::cancel_tracked_reference(input, wrong, mode.symbol_samples).residual.empty(),
            "disjoint waveform projection rejected");

      // A fade changes the fitted channel, not the reference's on-air symbols.
      signal.fade_depth = 0.9;
      signal.fade_center_s = 39.5 * signal.symbol_seconds;
      signal.fade_width_s = 5.0 * signal.symbol_seconds;
      auto const faded_pcm = test::render_tracking_audio(std::array{test::TrackingWaveform(signal)},
                                                         12000, count, 1000.0, 9);
      auto const faded_noise = test::render_tracking_audio({}, 12000, count, 1000.0, 9);
      input.assign(faded_pcm.begin(), faded_pcm.end());
      auto const faded = decoder::cancel_tracked_reference(input, reference, mode.symbol_samples);
      check(!faded.residual.empty(), "smooth fade cancellation accepted");
      if (!faded.residual.empty())
        check(squared_error(faded.residual, faded_noise) < 0.03 * squared_error(input, faded_noise),
              "smooth channel fit follows fading without changing tone identities");

      auto invalid = model;
      invalid.frequency_hz = 6000.0;
      check(decoder::make_tracked_reference(signal.tones, invalid, count).empty(), "Nyquist alias rejected");
      invalid = model; invalid.frequency_hz = std::numeric_limits<double>::quiet_NaN();
      check(decoder::make_tracked_reference(signal.tones, invalid, count).empty(), "nonfinite model rejected");
      invalid = model; invalid.timing_samples_per_symbol = -mode.symbol_samples;
      check(decoder::make_tracked_reference(signal.tones, invalid, count).empty(), "nonmonotonic symbol boundaries rejected");
      auto tones = signal.tones;
      tones[78] = 8;
      check(decoder::make_tracked_reference(tones, model, count).empty(), "invalid tone rejected");
      double const end = model.epoch_sample + model.timing_offset_samples +
          79.0 * (mode.symbol_samples + model.timing_samples_per_symbol);
      check(decoder::make_tracked_reference(signal.tones, model, static_cast<int>(std::ceil(end)) - 1).empty(),
            "partial frame rejected before cancellation");
    }
  }

  std::vector<float> audio(4096, 0.0f);
  std::vector<std::complex<float>> reference(4096, {1.0f, 0.0f});
  check(decoder::cancel_tracked_reference(audio, reference, 32).residual.empty(), "silence rejected");
  audio.assign(4096, 1.0f);
  check(decoder::cancel_tracked_reference(audio, reference, 32).residual.empty(), "singular quadrature fit rejected");
  audio[0] = std::numeric_limits<float>::infinity();
  check(decoder::cancel_tracked_reference(audio, reference, 32).residual.empty(), "nonfinite audio rejected");
  check(decoder::cancel_tracked_reference({}, reference, 32).residual.empty(), "mismatched extents rejected");
  audio[0] = 1.0f;
  check(decoder::cancel_tracked_reference(audio, reference, std::numeric_limits<int>::max()).residual.empty(),
        "oversized block geometry rejected before integer multiplication");
  reference[0] = {std::numeric_limits<float>::quiet_NaN(), 0.0f};
  check(decoder::cancel_tracked_reference(audio, reference, 32).residual.empty(), "nonfinite reference rejected");
  reference[0] = {2.0f, 0.0f};
  check(decoder::cancel_tracked_reference(audio, reference, 32).residual.empty(), "non-unit reference rejected");
  reference[0] = {1.0f, 0.0f};
  reference[100] = {};
  check(decoder::cancel_tracked_reference(audio, reference, 32).residual.empty(), "reference support holes rejected");
  std::printf("%d tracked reconstruction/cancellation checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}

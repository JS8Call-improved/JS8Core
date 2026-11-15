#include "tracking_waveform.hpp"

#include <cstdio>
#include <limits>

using namespace js8core::test;

int main() {
  int failures = 0;
  auto check = [&](bool condition, char const* name) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", name); ++failures; }
  };
  auto frequency = [](TrackingWaveform const& waveform, double t) {
    constexpr double dt = 1e-7;
    return std::arg(waveform.at(t + dt) * std::conj(waveform.at(t - dt))) /
           (2.0 * dt * kWaveTau);
  };

  TrackingSignal signal;
  signal.base_hz = 1000.25;
  signal.symbol_seconds = 0.1;
  signal.start_seconds = 0.500037;
  signal.phase_radians = 0.37;
  signal.tones.fill(2);
  signal.tones[1] = 7;
  TrackingWaveform plain(signal);
  check(plain.at(signal.start_seconds - 1e-6) == std::complex<double>{}, "before frame");
  check(plain.at(signal.start_seconds + 7.900001) == std::complex<double>{}, "after frame");
  check(std::abs(plain.at(signal.start_seconds) - std::polar(5000.0, 0.37)) < 1e-8,
        "fractional start and initial phase");
  double const boundary = signal.start_seconds + signal.symbol_seconds;
  check(std::abs(plain.at(boundary + 1e-10) - plain.at(boundary - 1e-10)) < 0.01,
        "continuous phase across unequal tones");
  check(std::abs(frequency(plain, signal.start_seconds + 0.05) - 1020.25) < 1e-5,
        "tone spacing in hertz");

  for (double sign : {-1.0, 1.0}) {
    signal.clock_ppm = sign * 800.0;
    signal.frequency_offset_hz = sign * 1.7;
    signal.frequency_slope_hz_s = sign * 0.3;
    signal.frequency_wander_hz = sign * 0.6;
    signal.timing_wander_seconds = sign * 0.002;
    TrackingWaveform warped(signal);
    double const elapsed = 2.35;
    double const timing_omega = kWaveTau / signal.timing_wander_period_s;
    double const clock_rate = 1.0 + signal.clock_ppm * 1e-6 +
        signal.timing_wander_seconds * timing_omega * std::cos(timing_omega * elapsed);
    double const expected = 1020.25 * clock_rate + signal.frequency_offset_hz +
        signal.frequency_slope_hz_s * elapsed + signal.frequency_wander_hz *
            std::sin(kWaveTau * elapsed / signal.frequency_wander_period_s);
    check(std::abs(frequency(warped, signal.start_seconds + elapsed) - expected) < 1e-4,
          "warped waveform instantaneous frequency");
    check(std::abs(warped.transmitter_time(elapsed) -
        (elapsed * (1.0 + signal.clock_ppm * 1e-6) +
         signal.timing_wander_seconds * std::sin(timing_omega * elapsed))) < 1e-12,
          "clock and timing warp");
  }

  TrackingSignal fading;
  fading.fade_depth = 0.8;
  fading.fade_center_s = 1.0;
  TrackingWaveform faded(fading);
  check(std::abs(std::abs(faded.at(1.5)) - 1000.0) < 1e-8, "fade envelope");

  const std::array pair = {plain, faded};
  auto audio = render_tracking_audio(pair, 12000, 24000);
  auto a = render_tracking_audio(std::span(&plain, 1), 12000, 24000);
  auto b = render_tracking_audio(std::span(&faded, 1), 12000, 24000);
  for (std::size_t i = 0; i < audio.size(); ++i)
    if (std::abs(static_cast<int>(audio[i]) - a[i] - b[i]) > 1) {
      check(false, "linear interference mixing before quantization"); break;
    }
  auto noisy = render_tracking_audio({}, 12000, 24000, 100.0, 42);
  check(noisy == render_tracking_audio({}, 12000, 24000, 100.0, 42), "repeatable noise");
  double sum = 0.0, squares = 0.0;
  for (auto x : noisy) { sum += x; squares += static_cast<double>(x) * x; }
  check(std::abs(sum / noisy.size()) < 3.0 &&
        std::abs(squares / noisy.size() - 10000.0) < 500.0, "noise mean and variance");

  TrackingSignal clipped;
  clipped.amplitude = 100000.0;
  auto loud = render_tracking_audio(std::array{TrackingWaveform(clipped)}, 12000, 12000);
  check(*std::max_element(loud.begin(), loud.end()) == 32767 &&
        *std::min_element(loud.begin(), loud.end()) == -32768, "saturating PCM conversion");

  auto invalid = [&](TrackingSignal s) {
    try { TrackingWaveform waveform(s); } catch (std::invalid_argument const&) { return true; }
    return false;
  };
  TrackingSignal bad;
  bad.symbol_seconds = 0.0;
  check(invalid(bad), "reject zero symbol duration");
  bad = {}; bad.clock_ppm = -1e6;
  check(invalid(bad), "reject nonmonotonic clock");
  bad = {}; bad.timing_wander_seconds = 10.0;
  check(invalid(bad), "reject nonmonotonic wander");
  bad = {}; bad.frequency_offset_hz = std::numeric_limits<double>::quiet_NaN();
  check(invalid(bad), "reject NaN");
  bad = {}; bad.tones[0] = 8;
  check(invalid(bad), "reject invalid tone");
  if (failures == 0) std::puts("PASS: independent tracking waveform checks");
  return failures ? 1 : 0;
}

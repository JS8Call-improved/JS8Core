#include "js8core/dsp/resampler.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <span>
#include <vector>

namespace {
constexpr double pi = 3.1415926535897932384626433832795;

struct Result {
  std::size_t frames = 0;
  double rms = 0.0;
};

Result run_tone(double frequency_hz) {
  constexpr int input_rate = 44100;
  constexpr int output_rate = 12000;
  std::vector<float> input(input_rate);
  for (std::size_t i = 0; i < input.size(); ++i)
    input[i] = static_cast<float>(std::sin(2.0 * pi * frequency_hz * i / input_rate));

  js8core::dsp::FractionalRxResampler converter;
  converter.configure(input_rate, output_rate);
  std::vector<float> output, block;
  for (std::size_t pos = 0; pos < input.size(); pos += 137) {
    const auto size = std::min<std::size_t>(137, input.size() - pos);
    converter.process(std::span<const float>(input.data() + pos, size), block);
    output.insert(output.end(), block.begin(), block.end());
  }
  double sum = 0.0;
  std::size_t count = 0;
  for (std::size_t i = 500; i < output.size(); ++i) {
    sum += static_cast<double>(output[i]) * output[i];
    ++count;
  }
  return {output.size(), count > 0 ? std::sqrt(sum / count) : 0.0};
}
double tone_magnitude(std::vector<float> const& samples, double frequency, double rate) {
  double re = 0.0, im = 0.0;
  for (std::size_t i = 1000; i < samples.size(); ++i) {
    double phase = 2.0 * pi * frequency * static_cast<double>(i) / rate;
    re += samples[i] * std::cos(phase);
    im += samples[i] * std::sin(phase);
  }
  return 2.0 * std::hypot(re, im) / (samples.size() - 1000);
}

bool test_tx_filter() {
  constexpr int input_rate = 12000;
  constexpr int output_rate = 44100;
  js8core::dsp::Resampler converter;
  converter.configure(input_rate, output_rate);
  std::vector<float> result;
  result.reserve(output_rate);
  std::vector<float> block(211);
  std::uint64_t consumed = 0;
  while (result.size() < output_rate) {
    std::size_t n = std::min(block.size(), static_cast<std::size_t>(output_rate) - result.size());
    converter.process(std::span<float>(block.data(), n), [&] {
      return static_cast<float>(std::sin(
          2.0 * pi * 1000.0 * static_cast<double>(consumed++) / input_rate));
    });
    result.insert(result.end(), block.begin(), block.begin() + n);
  }
  double fundamental = tone_magnitude(result, 1000, output_rate);
  double image = tone_magnitude(result, 11000, output_rate);
  if (fundamental < 0.8 || fundamental > 1.2 || image > 0.05 * fundamental) {
    std::cerr << "TX filter response failed: " << fundamental << " / " << image << '\n';
    return false;
  }
  return true;
}
}  // namespace

bool check_native_fir() {
  auto taps = js8core::dsp::make_js8_fir(48000, 12000);
  if (taps.size() != 97) return false;
  auto response = [&](double frequency) {
    double real = 0.0, imag = 0.0;
    for (std::size_t i = 0; i < taps.size(); ++i) {
      double phase = 2.0 * pi * frequency * static_cast<double>(i) / 48000.0;
      real += taps[i] * std::cos(phase);
      imag -= taps[i] * std::sin(phase);
    }
    return std::hypot(real, imag);
  };
  // 3-kHz JS8 audio is in the flat passband; alias-producing energy above
  // the 12-kHz destination Nyquist must be strongly suppressed.
  double pass = response(3000.0), stop = response(8000.0);
  return pass > 0.99 && pass < 1.01 && stop < 0.001;
}

int main() {
  if (!check_native_fir()) {
    std::cerr << "Independent 48-to-12-kHz FIR response FAIL\\n";
    return 1;
  }
  auto pass = run_tone(1000.0);
  auto stop = run_tone(10000.0);
  if (pass.frames < 11990 || pass.frames > 12005 ||
      stop.frames != pass.frames ||
      pass.rms < 0.55 || pass.rms > 0.85 ||
      stop.rms > 0.1) {
    std::cerr << "Fractional RX failed: frames=" << pass.frames
              << " pass_rms=" << pass.rms << " stop_rms=" << stop.rms << '\n';
    return 1;
  }
  if (!test_tx_filter()) return 1;
  std::cout << "Fractional RX and TX rate/passband/stopband PASS\n";
  return 0;
}

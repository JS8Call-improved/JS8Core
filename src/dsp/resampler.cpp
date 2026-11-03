#include "js8core/dsp/resampler.hpp"

#include <algorithm>
#include <cmath>

// The receive filter is a locally generated DSP design, not an on-air
// protocol constant. Its coefficients may differ from desktop JS8Call while
// preserving the same 12-kHz samples and decoder interoperability.

namespace js8core::dsp {

namespace {

std::vector<float> make_windowed_sinc_fir(int input_rate, int target_rate) {
  // Odd length yields a symmetric, integer-delay low-pass.
  // Blackman window provides strong rejection above the 6-kHz RX Nyquist.
  constexpr int kNumTaps = 97;
  constexpr double kPi = 3.14159265358979323846;

  if (input_rate <= 0 || target_rate <= 0) return {};

  // Guard band and window transition retain the full JS8 audio passband.
  double cutoff = 0.44 * static_cast<double>(target_rate) / static_cast<double>(input_rate);
  cutoff = std::clamp(cutoff, 0.0, 0.5);

  std::vector<float> taps(kNumTaps);
  double sum = 0.0;

  for (int i = 0; i < kNumTaps; ++i) {
    double n = i - (kNumTaps - 1) / 2.0;
    double sinc = (n == 0.0)
                      ? 2.0 * cutoff
                      : std::sin(2.0 * kPi * cutoff * n) / (kPi * n);
    double phase = 2.0 * kPi * i / (kNumTaps - 1);
    double window = 0.42 - 0.5 * std::cos(phase) + 0.08 * std::cos(2.0 * phase);
    taps[i] = static_cast<float>(sinc * window);
    sum += taps[i];
  }

  if (sum != 0.0) {
    for (auto& t : taps) t = static_cast<float>(t / sum);
  }

  return taps;
}

std::vector<std::vector<float>> build_polyphase(std::vector<float> const& taps, int factor, bool scale) {
  std::vector<std::vector<float>> phases;
  phases.resize(static_cast<std::size_t>(factor));
  for (int p = 0; p < factor; ++p) {
    for (std::size_t i = static_cast<std::size_t>(p); i < taps.size(); i += static_cast<std::size_t>(factor)) {
      float value = taps[i];
      if (scale) value *= static_cast<float>(factor);
      phases[static_cast<std::size_t>(p)].push_back(value);
    }
  }
  return phases;
}

}  // namespace

std::vector<float> make_js8_fir(int input_rate, int target_rate) {
  return make_windowed_sinc_fir(input_rate, target_rate);
}

void Resampler::configure(int input_rate, int output_rate) {
  reset();
  input_rate_ = input_rate;
  output_rate_ = output_rate;

  if (input_rate_ <= 0 || output_rate_ <= 0) {
    mode_ = Mode::Unconfigured;
    return;
  }

  if (input_rate_ == output_rate_) {
    mode_ = Mode::Passthrough;
    return;
  }

  if (output_rate_ % input_rate_ == 0) {
    mode_ = Mode::Upsample;
    factor_ = output_rate_ / input_rate_;
    int fir_in = std::max(input_rate_, output_rate_);
    int fir_out = std::min(input_rate_, output_rate_);
    taps_ = make_js8_fir(fir_in, fir_out);
    phase_taps_ = build_polyphase(taps_, factor_, true);
    ring_.assign(taps_.size(), 0.0f);
    return;
  }

  if (input_rate_ % output_rate_ == 0) {
    mode_ = Mode::Downsample;
    factor_ = input_rate_ / output_rate_;
    int fir_in = std::max(input_rate_, output_rate_);
    int fir_out = std::min(input_rate_, output_rate_);
    taps_ = make_js8_fir(fir_in, fir_out);
    ring_.assign(taps_.size(), 0.0f);
    return;
  }

  mode_ = Mode::Fractional;
  step_ = static_cast<double>(input_rate_) / static_cast<double>(output_rate_);
  constexpr double pi = 3.14159265358979323846;
  double cutoff = 0.475 * std::min(
      1.0, static_cast<double>(output_rate_) / input_rate_);
  for (int phase = 0; phase < kFracPhases; ++phase) {
    double fractional = static_cast<double>(phase) / kFracPhases;
    double weight = 0.0;
    for (int tap = 0; tap < kFracTaps; ++tap) {
      double x = static_cast<double>(tap - kFracHalf) - fractional;
      double z = 2.0 * pi * cutoff * x;
      double sinc = std::abs(z) < 1.0e-12 ? 2.0 * cutoff :
                    2.0 * cutoff * std::sin(z) / z;
      double window = 0.54 - 0.46 * std::cos(2.0 * pi * tap / (kFracTaps - 1));
      float v = static_cast<float>(sinc * window);
      frac_filters_[phase][tap] = v;
      weight += v;
    }
    for (float& v : frac_filters_[phase])
      v = static_cast<float>(v / weight);
  }
}

void Resampler::reset() {
  mode_ = Mode::Unconfigured;
  input_rate_ = 0;
  output_rate_ = 0;
  factor_ = 1;
  taps_.clear();
  phase_taps_.clear();
  ring_.clear();
  ring_pos_ = 0;
  phase_ = 0;
  step_ = 0.0;
  frac_pos_ = 0.0;
  curr_ = 0.0f;
  next_ = 0.0f;
  has_next_ = false;
  frac_history_.clear();
  frac_history_first_ = 0;
  frac_next_input_ = 0;
}

void FractionalRxResampler::reset() {
  input_rate_ = 0;
  output_rate_ = 0;
  step_ = 0.0;
  position_ = 0.0;
  history_.clear();
}

void FractionalRxResampler::configure(int input_rate, int output_rate) {
  reset();
  if (input_rate <= 0 || output_rate <= 0) return;

  input_rate_ = input_rate;
  output_rate_ = output_rate;
  step_ = static_cast<double>(input_rate) / output_rate;

  constexpr double pi = 3.14159265358979323846;
  // A little guard band keeps the stopband away from the 6-kHz RX Nyquist.
  double cutoff = 0.475 * std::min(1.0, static_cast<double>(output_rate) / input_rate);
  for (int phase = 0; phase < kPhases; ++phase) {
    double fractional = static_cast<double>(phase) / kPhases;
    double weight_sum = 0.0;
    for (int tap = 0; tap < kTaps; ++tap) {
      double x = static_cast<double>(tap - kHalf) - fractional;
      double argument = 2.0 * pi * cutoff * x;
      double sinc = std::abs(argument) < 1.0e-12
                        ? 2.0 * cutoff
                        : 2.0 * cutoff * std::sin(argument) / argument;
      double window = 0.54 - 0.46 * std::cos(2.0 * pi * tap / (kTaps - 1));
      auto value = static_cast<float>(sinc * window);
      filters_[phase][tap] = value;
      weight_sum += value;
    }
    for (auto& value : filters_[phase])
      value = static_cast<float>(value / weight_sum);
  }
}

void FractionalRxResampler::process(std::span<const float> input,
                                    std::vector<float>& output) {
  output.clear();
  if (input_rate_ <= 0 || output_rate_ <= 0 || input.empty()) return;
  history_.insert(history_.end(), input.begin(), input.end());

  // Hold enough future samples to evaluate the symmetric filter. At the
  // beginning of a stream, unavailable negative-time samples are zero.
  while (position_ + kHalf < static_cast<double>(history_.size())) {
    auto center = static_cast<std::size_t>(position_);
    double fraction = position_ - static_cast<double>(center);
    int phase = std::clamp(static_cast<int>(fraction * kPhases), 0, kPhases - 1);
    auto const& filter = filters_[phase];
    double sum = 0.0;
    for (int tap = 0; tap < kTaps; ++tap) {
      auto index = static_cast<std::ptrdiff_t>(center) + tap - kHalf;
      if (index >= 0 && static_cast<std::size_t>(index) < history_.size())
        sum += filter[tap] * history_[static_cast<std::size_t>(index)];
    }
    output.push_back(static_cast<float>(sum));
    position_ += step_;
  }

  // Retain only samples needed for the next window.
  auto discarded = position_ > kHalf
                       ? static_cast<std::size_t>(position_) - kHalf : 0u;
  discarded = std::min(discarded, history_.size());
  if (discarded > 0) {
    history_.erase(history_.begin(),
                   history_.begin() + static_cast<std::ptrdiff_t>(discarded));
    position_ -= static_cast<double>(discarded);
  }
}

}  // namespace js8core::dsp

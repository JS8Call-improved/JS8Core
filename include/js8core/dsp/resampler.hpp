#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <vector>

namespace js8core::dsp {

// Deterministic, independently designed low-pass for sample-rate conversion.
std::vector<float> make_js8_fir(int input_rate, int target_rate);

class Resampler {
public:
  void configure(int input_rate, int output_rate);
  void reset();

  int input_rate() const { return input_rate_; }
  int output_rate() const { return output_rate_; }

  template <typename InputFn>
  void process(std::span<float> output, InputFn next_input) {
    switch (mode_) {
      case Mode::Unconfigured:
        for (auto& v : output) v = 0.0f;
        return;
      case Mode::Passthrough:
        for (auto& v : output) v = next_input();
        return;
      case Mode::Upsample:
        upsample(output, next_input);
        return;
      case Mode::Downsample:
        downsample(output, next_input);
        return;
      case Mode::Fractional:
        fractional(output, next_input);
        return;
    }
  }

private:
  enum class Mode { Unconfigured, Passthrough, Upsample, Downsample, Fractional };

  template <typename InputFn>
  void upsample(std::span<float> output, InputFn next_input) {
    if (phase_taps_.empty() || ring_.empty()) {
      for (auto& v : output) v = 0.0f;
      return;
    }

    for (auto& v : output) {
      if (phase_ == 0) {
        ring_[ring_pos_] = next_input();
        ring_pos_ = (ring_pos_ + 1) % static_cast<int>(ring_.size());
      }

      auto const& taps = phase_taps_[phase_];
      double acc = 0.0;
      int read_pos = (ring_pos_ - 1 + static_cast<int>(ring_.size())) % static_cast<int>(ring_.size());
      for (std::size_t j = 0; j < taps.size(); ++j) {
        int idx = (read_pos - static_cast<int>(j) + static_cast<int>(ring_.size())) % static_cast<int>(ring_.size());
        acc += static_cast<double>(taps[j]) * static_cast<double>(ring_[idx]);
      }
      v = static_cast<float>(acc);

      phase_ = (phase_ + 1) % factor_;
    }
  }

  template <typename InputFn>
  void downsample(std::span<float> output, InputFn next_input) {
    if (taps_.empty() || ring_.empty()) {
      for (auto& v : output) v = 0.0f;
      return;
    }

    for (auto& v : output) {
      for (int i = 0; i < factor_; ++i) {
        ring_[ring_pos_] = next_input();
        ring_pos_ = (ring_pos_ + 1) % static_cast<int>(ring_.size());
      }

      double acc = 0.0;
      int read_pos = (ring_pos_ - 1 + static_cast<int>(ring_.size())) % static_cast<int>(ring_.size());
      for (std::size_t j = 0; j < taps_.size(); ++j) {
        int idx = (read_pos - static_cast<int>(j) + static_cast<int>(ring_.size())) % static_cast<int>(ring_.size());
        acc += static_cast<double>(taps_[j]) * static_cast<double>(ring_[idx]);
      }
      v = static_cast<float>(acc);
    }
  }

  template <typename InputFn>
  void fractional(std::span<float> output, InputFn next_input) {
    if (step_ <= 0.0) {
      for (auto& v : output) v = 0.0f;
      return;
    }

    // Use a band-limited, phase-interpolated kernel instead of linear
    // interpolation, which produces strong out-of-band TX images.
    for (auto& sample : output) {
      auto const center = static_cast<std::uint64_t>(frac_pos_);
      while (frac_next_input_ <= center + kFracHalf) {
        frac_history_.push_back(next_input());
        ++frac_next_input_;
      }
      int phase = std::clamp(
          static_cast<int>((frac_pos_ - static_cast<double>(center)) * kFracPhases),
          0, kFracPhases - 1);
      double sum = 0.0;
      auto const& kernel = frac_filters_[static_cast<std::size_t>(phase)];
      for (int tap = 0; tap < kFracTaps; ++tap) {
        std::int64_t const index = static_cast<std::int64_t>(center) + tap - kFracHalf;
        if (index < static_cast<std::int64_t>(frac_history_first_) ||
            index >= static_cast<std::int64_t>(frac_next_input_)) continue;
        sum += kernel[static_cast<std::size_t>(tap)] *
               frac_history_[static_cast<std::size_t>(
                   index - static_cast<std::int64_t>(frac_history_first_))];
      }
      sample = static_cast<float>(sum);
      frac_pos_ += step_;

      std::uint64_t const keep_from =
          center > kFracHalf ? center - kFracHalf : 0;
      while (frac_history_first_ < keep_from) {
        frac_history_.pop_front();
        ++frac_history_first_;
      }
    }
  }

  Mode mode_ = Mode::Unconfigured;
  int input_rate_ = 0;
  int output_rate_ = 0;
  int factor_ = 1;
  std::vector<float> taps_;
  std::vector<std::vector<float>> phase_taps_;
  std::vector<float> ring_;
  int ring_pos_ = 0;
  int phase_ = 0;
  double step_ = 0.0;
  double frac_pos_ = 0.0;
  float curr_ = 0.0f;
  float next_ = 0.0f;
  bool has_next_ = false;
  static constexpr int kFracTaps = 49;
  static constexpr int kFracHalf = kFracTaps / 2;
  static constexpr int kFracPhases = 256;
  std::array<std::array<float, kFracTaps>, kFracPhases> frac_filters_{};
  std::deque<float> frac_history_;
  std::uint64_t frac_history_first_{0};
  std::uint64_t frac_next_input_{0};
};

// Streaming band-limited converter for non-integer capture ratios such as
// 44.1 kHz -> 12 kHz. Uses 256 fractional phases and a 49-tap windowed-sinc
// low-pass kernel; preserves the needed sample history between callbacks.
class FractionalRxResampler {
public:
  void configure(int input_rate, int output_rate);
  void reset();
  int input_rate() const { return input_rate_; }
  int output_rate() const { return output_rate_; }
  void process(std::span<const float> input, std::vector<float>& output);

private:
  static constexpr int kTaps = 49;
  static constexpr int kHalf = kTaps / 2;
  static constexpr int kPhases = 256;
  int input_rate_ = 0;
  int output_rate_ = 0;
  double step_ = 0.0;
  double position_ = 0.0;
  std::array<std::array<float, kTaps>, kPhases> filters_{};
  std::vector<float> history_;
};

}  // namespace js8core::dsp

#include "js8core/decoder.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include <Eigen/Dense>
#define POCKETFFT_CACHE_SIZE 1000000
#include "pocketfft/pocketfft_hdronly.h"

#include "js8core/compat/numbers.hpp"
#include "js8core/protocol/constants.hpp"
#include "js8core/protocol/costas.hpp"

#include "decoder_tables.hpp"
#include "ldpc.hpp"

namespace js8core {

using dec_data = DecodeState;
using EventEmitter = std::function<void(events::Variant const&)>;
using namespace decoder::data;

namespace {

using namespace decoder::data;

using shape_t = std::vector<std::size_t>;
using stride_t = std::vector<std::ptrdiff_t>;

constexpr std::ptrdiff_t kComplexStride =
    static_cast<std::ptrdiff_t>(sizeof(std::complex<float>));
constexpr std::ptrdiff_t kFloatStride = static_cast<std::ptrdiff_t>(sizeof(float));
constexpr std::complex<float> kZero{0.0f, 0.0f};

// Kahan-compensated accumulator. The Nuttall window is very sensitive to
// summation order; plain float accumulation shifts the coefficients enough to
// matter during correlation.
class Accumulator {
 public:
  explicit Accumulator(float initial = 0.0f)
      : value_(initial), correction_(0.0f) {}

  Accumulator& operator=(float value) {
    value_ = value;
    correction_ = 0.0f;
    return *this;
  }

  Accumulator& operator+=(float term) {
    float const adjusted = term - correction_;
    float const next = value_ + adjusted;
    correction_ = (next - value_) - adjusted;
    value_ = next;
    return *this;
  }

  operator float() const { return value_; }

 private:
  float value_;
  float correction_;
};

// One synchronization candidate: absolute frequency, time offset in seconds,
// and normalized correlation score.
struct Candidate {
  float frequency;
  float offset;
  float score;
};

// A successfully decoded frame, keyed by type and text so that duplicate
// detections can be merged.
struct Frame {
  int type;
  std::string text;

  bool operator==(Frame const&) const noexcept = default;

  struct Hash {
    std::size_t operator()(Frame const& frame) const noexcept {
      std::size_t const a = std::hash<int>{}(frame.type);
      std::size_t const b = std::hash<std::string>{}(frame.text);
      return a ^ (b + 0x9e3779b9u + (a << 6) + (a >> 2));
    }
  };

  using Map = std::unordered_map<Frame, int, Hash>;
};

// Per-submode decoder. All buffers are sized from the ModeProfile so that one
// template instantiation handles each JS8 speed level.
template <typename Mode>
class ChannelDecoder {
 public:
  ChannelDecoder() { initialize(); }

  std::size_t operator()(dec_data const& data, int position, int size,
                         EventEmitter emit);

 private:
  using Spectrum = std::array<std::complex<float>, Mode::fft_span / 2 + 1>;
  using Baseband = std::array<std::complex<float>, Mode::ds_fft_span / 2 + 1>;

  // --- Initialization -------------------------------------------------------

  void initialize();
  void build_window();
  void build_sync_waveforms();
  void build_subtraction_filter();

  // --- Signal conditioning --------------------------------------------------

  void prepare_baseband();
  void extract_channel(float frequency);
  void compute_baseline(int first, int last);
  float evaluate_baseline(float x) const;

  // --- Synchronization and demodulation ------------------------------------

  std::vector<Candidate> find_candidates(int low_hz, int high_hz);
  float alignment_score(int offset, float frequency_delta);
  std::optional<Frame> attempt_decode(bool sync_stats, bool subtract,
                                      float& frequency, float& offset,
                                      int& hard_errors, float& snr,
                                      int& rescue_budget, EventEmitter emit);

  // --- Signal reconstruction -----------------------------------------------

  std::vector<std::complex<float>> make_reference(std::array<int, kChannelSymbols> const& tones,
                                                  float frequency);
  void subtract_reference(std::vector<std::complex<float>> const& reference,
                          float offset);

  // --- Buffers --------------------------------------------------------------

  std::array<float, Mode::fft_span> window_{};
  std::array<std::array<std::array<std::complex<float>, Mode::downsample_sps>, 7>, 3>
      sync_waveforms_{};
  alignas(64) std::array<std::complex<float>, Mode::downsample_sps> symbol_fft_{};
  alignas(64) std::array<std::complex<float>, Mode::nmax> subtract_filter_{};
  alignas(64) std::array<std::complex<float>, Mode::nmax> subtract_work_{};
  alignas(64) std::array<std::complex<float>, Mode::ds_fft_span / 2 + 1> baseband_spectrum_{};
  alignas(64) std::array<std::complex<float>, Mode::fft_span / 2 + 1> spectrum_{};
  alignas(64) std::array<std::complex<float>, kComplexCapacity> channel_{};
  std::array<float, Mode::nmax> samples_{};
  std::vector<float> baseband_input_;

  std::array<std::array<float, Mode::spectra>, Mode::nsps> spectra_{};
  std::array<float, Mode::nsps> average_spectrum_{};
  std::array<float, Mode::fft_span> windowed_{};

  // Baseline fit.
  using Points = Eigen::Matrix<double, 6, 2>;
  using Vandermonde = Eigen::Matrix<double, 6, 6>;
  using Coefficients = Eigen::Vector<double, 6>;

  Points points_{};
  Vandermonde vandermonde_{};
  Coefficients coefficients_{};

  inline static const auto& kCostas = protocol::costas(Mode::costas_type);

  inline static const auto kTaper = [] {
    std::array<std::array<float, Mode::taper_span + 1>, 2> taper{};
    constexpr float pi = js8core::compat::numbers::pi_v<float>;
    for (int i = 0; i <= Mode::taper_span; ++i) {
      float const c = std::cos(i * pi / Mode::taper_span);
      taper[0][static_cast<std::size_t>(i)] = 0.5f * (1.0f - c);  // head
      taper[1][static_cast<std::size_t>(i)] = 0.5f * (1.0f + c);  // tail
    }
    return taper;
  }();

  inline static const auto kBaselineNodes = [] {
    std::array<double, 6> nodes{};
    constexpr double slice = js8core::compat::numbers::pi / (2.0 * 6);
    for (std::size_t i = 0; i < nodes.size(); ++i) {
      nodes[i] = 0.5 * (1.0 - std::cos(slice * (2.0 * i + 1.0)));
    }
    return nodes;
  }();
};

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

template <typename Mode>
void ChannelDecoder<Mode>::initialize() {
  baseband_input_.assign(Mode::ds_fft_span, 0.0f);
  build_window();
  build_sync_waveforms();
  build_subtraction_filter();
}

template <typename Mode>
void ChannelDecoder<Mode>::build_window() {
  constexpr float a0 = 0.3635819f;
  constexpr float a1 = -0.4891775f;
  constexpr float a2 = 0.1365995f;
  constexpr float a3 = -0.0106411f;
  float const pi = 4.0f * std::atan(1.0f);

  float total = 0.0f;
  for (std::size_t i = 0; i < window_.size(); ++i) {
    Accumulator value(a0);
    value += a1 * std::cos(2.0f * pi * i / window_.size());
    value += a2 * std::cos(4.0f * pi * i / window_.size());
    value += a3 * std::cos(6.0f * pi * i / window_.size());
    window_[i] = value;
    total += value;
  }

  float const scale = static_cast<float>(window_.size()) / (300.0f * total);
  for (float& value : window_) value *= scale;
}

template <typename Mode>
void ChannelDecoder<Mode>::build_sync_waveforms() {
  for (int block = 0; block < 3; ++block) {
    for (int column = 0; column < 7; ++column) {
      float const step =
          kTau * static_cast<float>(kCostas[block][column]) / Mode::downsample_sps;
      float phase = 0.0f;
      for (int i = 0; i < Mode::downsample_sps; ++i) {
        sync_waveforms_[static_cast<std::size_t>(block)]
                       [static_cast<std::size_t>(column)]
                       [static_cast<std::size_t>(i)] = std::polar(1.0f, phase);
        phase = std::fmod(phase + step, kTau);
      }
    }
  }
}

template <typename Mode>
void ChannelDecoder<Mode>::build_subtraction_filter() {
  constexpr int radius = kFilterLength / 2;
  const float pi = 4.0f * std::atan(1.0f);

  // A periodic FFT filter is indexed by signed time delay modulo N. Lags
  // below zero belong at the *end* of the N-point array, not after the
  // positive taps. Otherwise the filter has a spurious ~700-sample delay,
  // which ruins phase-aware subtraction of overlapping signals.
  subtract_filter_.fill(kZero);
  float tap_sum = 0.0f;
  for (int lag = -radius; lag <= radius; ++lag) {
    const float amplitude = std::cos(pi * lag / kFilterLength);
    const float weight = amplitude * amplitude;
    tap_sum += weight;
    const auto sample = static_cast<std::size_t>(
        lag < 0 ? Mode::nmax + lag : lag);
    subtract_filter_[sample] = {weight, 0.0f};
  }
  for (auto& tap : subtract_filter_) {
    tap *= (1.0f / tap_sum);
  }

  // Pair a pre-normalized frequency response with an unscaled inverse FFT.
  pocketfft::c2c<float>(shape_t{static_cast<std::size_t>(Mode::nmax)},
                        stride_t{kComplexStride}, stride_t{kComplexStride},
                        shape_t{0}, true, subtract_filter_.data(),
                        subtract_filter_.data(), 1.0f / Mode::nmax, 1);
}

// ---------------------------------------------------------------------------
// Signal conditioning
// ---------------------------------------------------------------------------

template <typename Mode>
void ChannelDecoder<Mode>::prepare_baseband() {
  std::fill(baseband_input_.begin(), baseband_input_.end(), 0.0f);
  std::copy(samples_.begin(), samples_.end(), baseband_input_.begin());

  pocketfft::r2c<float>(shape_t{static_cast<std::size_t>(Mode::ds_fft_span)},
                        stride_t{kFloatStride}, stride_t{kComplexStride},
                        shape_t{0}, true, baseband_input_.data(),
                        baseband_spectrum_.data(), 1.0f, 1);
}

template <typename Mode>
void ChannelDecoder<Mode>::extract_channel(float frequency) {
  constexpr float resolution = 12000.0f / Mode::ds_fft_span;
  constexpr float baud = 12000.0f / Mode::nsps;

  float const top_hz = frequency + 8.5f * baud;
  float const bottom_hz = frequency - 1.5f * baud;
  int const center = static_cast<int>(std::round(frequency / resolution));
  int const top = std::min(static_cast<int>(std::round(top_hz / resolution)),
                           Mode::ds_fft_span / 2);
  int const bottom = std::max(0, static_cast<int>(std::round(bottom_hz / resolution)));

  std::size_t const taper_count = Mode::taper_span + 1;
  std::size_t const width = static_cast<std::size_t>(top - bottom + 1);

  std::fill_n(channel_.begin(), Mode::ds_fft_out, kZero);
  std::copy(baseband_spectrum_.begin() + bottom,
            baseband_spectrum_.begin() + bottom + static_cast<std::ptrdiff_t>(width),
            channel_.begin());

  auto head = channel_.begin();
  auto tail = channel_.begin() + static_cast<std::ptrdiff_t>(width);

  for (std::size_t i = 0; i < taper_count; ++i) {
    head[i] *= kTaper[0][i];
    tail[-static_cast<std::ptrdiff_t>(taper_count) + static_cast<std::ptrdiff_t>(i)] *=
        kTaper[1][i];
  }

  int const rotation = center - bottom;
  std::rotate(channel_.begin(), channel_.begin() + rotation,
              channel_.begin() + Mode::ds_fft_out);

  // The inverse DFT must be *unscaled*, matching FFTW_BACKWARD in the
  // desktop reference. The sample-amplitude normalization immediately below
  // already accounts for the input/output FFT lengths. Applying pocketfft's
  // optional 1/N factor here as well attenuates the channel twice, collapsing
  // every decoded SNR to its -60 dB reporting floor.
  pocketfft::c2c<float>(shape_t{static_cast<std::size_t>(Mode::ds_fft_out)},
                        stride_t{kComplexStride}, stride_t{kComplexStride},
                        shape_t{0}, false, channel_.data(), channel_.data(),
                        1.0f, 1);

  float const scale =
      1.0f / std::sqrt(static_cast<float>(Mode::ds_fft_span) * Mode::ds_fft_out);
  for (int i = 0; i < Mode::ds_fft_out; ++i) {
    channel_[static_cast<std::size_t>(i)] *= scale;
  }
}

template <typename Mode>
float ChannelDecoder<Mode>::evaluate_baseline(float x) const {
  double value = 0.0;
  double power = 1.0;
  for (int i = 0; i < 3; ++i) {
    value += (coefficients_(2 * i) + coefficients_(2 * i + 1) * x) * power;
    power *= x * x;
  }
  return static_cast<float>(value);
}

template <typename Mode>
void ChannelDecoder<Mode>::compute_baseline(int first, int last) {
  constexpr std::size_t bottom =
      static_cast<std::size_t>(kBaselineMinHz / Mode::bin_hz + 0.5f);
  constexpr std::size_t top =
      static_cast<std::size_t>(kBaselineMaxHz / Mode::bin_hz + 0.5f);
  constexpr std::size_t span = top - bottom + 1;
  constexpr std::size_t arm = span / (2 * kBaselineNodes.size());

  auto data = average_spectrum_.begin() + static_cast<std::ptrdiff_t>(bottom);
  auto end = data + static_cast<std::ptrdiff_t>(span);

  std::transform(data, end, data, [](float value) {
    return 10.0f * std::log10(std::max(value, 1.0e-30f));
  });

  for (std::size_t i = 0; i < kBaselineNodes.size(); ++i) {
    double const node = static_cast<double>(span) * kBaselineNodes[i];
    auto base = data + static_cast<std::ptrdiff_t>(std::round(node));
    auto from = std::clamp(base - static_cast<std::ptrdiff_t>(arm), data, end);
    auto to = std::clamp(base + static_cast<std::ptrdiff_t>(arm), data, end);
    std::vector<float> window(from, to);
    std::size_t const pick = window.size() * kBaselineSamplePercent / 100;
    std::nth_element(window.begin(),
                     window.begin() + static_cast<std::ptrdiff_t>(pick),
                     window.end());
    points_.row(static_cast<Eigen::Index>(i)) << node,
        window[static_cast<std::size_t>(pick)];
  }

  Eigen::VectorXd x = points_.col(0);
  Eigen::VectorXd y = points_.col(1);

  vandermonde_.col(0).setOnes();
  for (Eigen::Index i = 1; i < vandermonde_.cols(); ++i) {
    vandermonde_.col(i) = vandermonde_.col(i - 1).cwiseProduct(x);
  }
  coefficients_ = vandermonde_.colPivHouseholderQr().solve(y);

  float const last_index = static_cast<float>(span - 1);
  average_spectrum_.fill(0.0f);
  for (int i = first; i <= last; ++i) {
    float const mapped = (i - first) * last_index / static_cast<float>(last - first);
    average_spectrum_[static_cast<std::size_t>(i)] =
        evaluate_baseline(mapped) + 0.65f;
  }
}

// ---------------------------------------------------------------------------
// Synchronization and demodulation
// ---------------------------------------------------------------------------

template <typename Mode>
float ChannelDecoder<Mode>::alignment_score(int offset, float frequency_delta) {
  constexpr float down_rate = 12000.0f / Mode::downsample_factor;
  constexpr float phase_step = kTau / down_rate;

  std::array<std::complex<float>, Mode::downsample_sps> rotation;
  if (frequency_delta != 0.0f) {
    float const step = phase_step * frequency_delta;
    float phase = 0.0f;
    for (int i = 0; i < Mode::downsample_sps; ++i) {
      rotation[static_cast<std::size_t>(i)] = std::polar(1.0f, phase);
      phase = std::fmod(phase + step, kTau);
      if (phase < 0.0f) phase += kTau;
    }
  } else {
    rotation.fill(std::complex<float>{1.0f, 0.0f});
  }

  float score = 0.0f;
  for (int block = 0; block < 3; ++block) {
    for (int column = 0; column < 7; ++column) {
      int const start =
          36 * block * Mode::downsample_sps + offset + column * Mode::downsample_sps;
      if (start < 0 || start + Mode::downsample_sps > Mode::payload_samples) {
        continue;
      }

      std::complex<float> sum{0.0f, 0.0f};
      for (int i = 0; i < Mode::downsample_sps; ++i) {
        auto const& wave =
            sync_waveforms_[static_cast<std::size_t>(block)]
                           [static_cast<std::size_t>(column)]
                           [static_cast<std::size_t>(i)];
        sum += channel_[static_cast<std::size_t>(start + i)] *
               std::conj(rotation[static_cast<std::size_t>(i)] * wave);
      }
      score += std::norm(sum);
    }
  }
  return score;
}

template <typename Mode>
std::vector<Candidate> ChannelDecoder<Mode>::find_candidates(int low_hz,
                                                             int high_hz) {
  // Invalid or inverted bands must not reach the polynomial baseline fit or
  // be turned into negative/huge vector allocation sizes.
  if (low_hz >= high_hz || high_hz <= 0 || low_hz > 4910) return {};
  average_spectrum_.fill(0.0f);

  for (int j = 0; j < Mode::spectra; ++j) {
    int const start = j * Mode::time_step;
    int const stop = start + Mode::fft_span;
    if (stop > Mode::nmax) break;

    for (int i = 0; i < Mode::fft_span; ++i) {
      windowed_[static_cast<std::size_t>(i)] =
          samples_[static_cast<std::size_t>(start + i)] * window_[static_cast<std::size_t>(i)];
    }

    pocketfft::r2c<float>(shape_t{static_cast<std::size_t>(Mode::fft_span)},
                          stride_t{kFloatStride}, stride_t{kComplexStride},
                          shape_t{0}, true, windowed_.data(), spectrum_.data(),
                          1.0f, 1);

    for (int i = 0; i < Mode::nsps; ++i) {
      float const power = std::norm(spectrum_[static_cast<std::size_t>(i)]);
      spectra_[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)] = power;
      average_spectrum_[static_cast<std::size_t>(i)] += power;
    }
  }

  int const requested_width = high_hz - low_hz;
  if (low_hz < 100) {
    low_hz = 100;
    if (requested_width < 100) high_hz = low_hz + requested_width;
  }
  if (high_hz > 4910) {
    high_hz = 4910;
    if (requested_width < 100) low_hz = high_hz - requested_width;
  }

  int const first = std::max(0, static_cast<int>(std::round(low_hz / Mode::bin_hz)));
  int const last = static_cast<int>(std::round(high_hz / Mode::bin_hz));
  // Even a positive-width Hz interval can collapse to a single FFT bin
  // after rounding. The baseline fit divides by (last - first).
  if (first >= last || last + kOversample * (kToneRows - 1) >= Mode::nsps)
    return {};

  compute_baseline(first, last);

  std::vector<Candidate> entries;
  entries.reserve(static_cast<std::size_t>(last - first + 1));

  for (int bin = first; bin <= last; ++bin) {
    float best = -std::numeric_limits<float>::infinity();
    int best_offset = -Mode::search_radius;

    for (int delta = -Mode::search_radius; delta <= Mode::search_radius; ++delta) {
      std::array<std::array<float, 3>, 2> sums{};

      for (int block = 0; block < 3; ++block) {
        for (int column = 0; column < 7; ++column) {
          int const time = delta + Mode::start_step + kStepsPerSymbol * column +
                           block * 36 * kStepsPerSymbol;
          if (time < 0 || time >= Mode::spectra) continue;

          sums[0][static_cast<std::size_t>(block)] +=
              spectra_[static_cast<std::size_t>(bin + kOversample * kCostas[block][column])]
                      [static_cast<std::size_t>(time)];

          for (int tone = 0; tone < 7; ++tone) {
            sums[1][static_cast<std::size_t>(block)] +=
                spectra_[static_cast<std::size_t>(bin + kOversample * tone)]
                        [static_cast<std::size_t>(time)];
          }
        }
      }

      auto const ratio = [&sums](int from, int to) {
        float signal = 0.0f;
        float total = 0.0f;
        for (int i = from; i <= to; ++i) {
          signal += sums[0][static_cast<std::size_t>(i)];
          total += sums[1][static_cast<std::size_t>(i)];
        }
        return signal / ((total - signal) / 6.0f);
      };

      float const score = std::max({ratio(0, 2), ratio(0, 1), ratio(1, 2)});
      if (std::isfinite(score) && score > best) {
        best = score;
        best_offset = delta;
      }
    }

    entries.push_back(Candidate{Mode::bin_hz * bin,
                                Mode::step_seconds * (best_offset + 0.5f), best});
  }

  if (entries.empty()) return {};

  std::vector<float> scores;
  scores.reserve(entries.size());
  for (auto const& entry : entries) scores.push_back(entry.score);

  std::size_t const rank = scores.size() * 4 / 10;
  std::nth_element(scores.begin(), scores.begin() + static_cast<std::ptrdiff_t>(rank),
                   scores.end());
  float const reference = scores[rank];
  if (!std::isfinite(reference) || reference <= 1.0e-12f) return {};

  for (auto& entry : entries) entry.score /= reference;

  std::vector<std::size_t> order(entries.size());
  std::iota(order.begin(), order.end(), 0);
  std::stable_sort(order.begin(), order.end(), [&entries](std::size_t a, std::size_t b) {
    return entries[a].score > entries[b].score;
  });

  std::vector<char> removed(entries.size(), 0);
  std::vector<Candidate> selected;

  for (std::size_t index : order) {
    if (removed[index]) continue;

    Candidate const& candidate = entries[index];
    if (!std::isfinite(candidate.score) || candidate.score < kMinSync) break;

    selected.push_back(candidate);
    if (selected.size() >= kMaxCandidates) break;

    for (std::size_t other = 0; other < entries.size(); ++other) {
      if (!removed[other] &&
          std::abs(entries[other].frequency - candidate.frequency) <= Mode::az) {
        removed[other] = 1;
      }
    }
  }

  return selected;
}

template <typename Mode>
std::optional<Frame> ChannelDecoder<Mode>::attempt_decode(
    bool sync_stats, bool subtract, float& frequency, float& offset,
    int& hard_errors, float& snr, int& rescue_budget, EventEmitter emit) {
  constexpr float frequency_resolution = 12000.0f / Mode::fft_span;
  constexpr float down_rate = 12000.0f / Mode::downsample_factor;
  constexpr float down_period = 1.0f / down_rate;

  int const bin = static_cast<int>(std::round(frequency / frequency_resolution));
  float const linear_base =
      std::pow(10.0f, 0.1f * (average_spectrum_[static_cast<std::size_t>(bin)] -
                              Mode::baseline_offset));

  extract_channel(frequency);

  int origin = static_cast<int>(std::round((offset + Mode::start_seconds) * down_rate));
  float best_score = 0.0f;
  int best_time = 0;

  for (int candidate = origin - Mode::quarter_symbol;
       candidate <= origin + Mode::quarter_symbol; ++candidate) {
    float const score = alignment_score(candidate, 0.0f);
    if (score > best_score) {
      best_score = score;
      best_time = candidate;
    }
  }

  float const refined_offset = best_time * down_period;
  origin = static_cast<int>(std::round(refined_offset * down_rate));
  best_score = 0.0f;
  float best_delta = 0.0f;

  for (int step = -kFreqSearchSteps; step <= kFreqSearchSteps; ++step) {
    float const delta = step * 0.5f;
    float const score = alignment_score(origin, delta);
    if (score > best_score) {
      best_score = score;
      best_delta = delta;
    }
  }

  float const phase_step = -best_delta * (kTau / down_rate);
  std::complex<float> const rotation_step = std::polar(1.0f, phase_step);
  std::complex<float> rotation{1.0f, 0.0f};
  for (int i = 0; i < Mode::payload_samples; ++i) {
    rotation *= rotation_step;
    channel_[static_cast<std::size_t>(i)] *= rotation;
  }

  offset = refined_offset;
  frequency += best_delta;
  float const sync = alignment_score(origin, 0.0f);

  std::array<std::array<float, kChannelSymbols>, kToneRows> tone_power{};

  for (int symbol = 0; symbol < kChannelSymbols; ++symbol) {
    int const start = best_time + symbol * Mode::downsample_sps;
    symbol_fft_.fill(kZero);
    if (start >= 0 && start + Mode::downsample_sps <= Mode::payload_samples) {
      std::copy(channel_.begin() + start,
                channel_.begin() + start + Mode::downsample_sps,
                symbol_fft_.begin());
    }

    pocketfft::c2c<float>(shape_t{static_cast<std::size_t>(Mode::downsample_sps)},
                          stride_t{kComplexStride}, stride_t{kComplexStride},
                          shape_t{0}, true, symbol_fft_.data(),
                          symbol_fft_.data(), 1.0f, 1);

    for (int row = 0; row < kToneRows; ++row) {
      tone_power[static_cast<std::size_t>(row)][static_cast<std::size_t>(symbol)] =
          std::abs(symbol_fft_[static_cast<std::size_t>(row)]) / 1000.0f;
    }
  }

  int sync_matches = 0;
  for (std::size_t block = 0; block < kCostas.size(); ++block) {
    std::size_t const base = block * 36;
    for (std::size_t column = 0; column < 7; ++column) {
      std::size_t const position = base + column;
      int best_row = 0;
      for (int row = 1; row < kToneRows; ++row) {
        if (tone_power[static_cast<std::size_t>(row)][position] >
            tone_power[static_cast<std::size_t>(best_row)][position]) {
          best_row = row;
        }
      }
      if (kCostas[block][column] == best_row) ++sync_matches;
    }
  }

  if (sync_matches <= 6) return std::nullopt;

  if (sync_stats) {
    events::SyncState event;
    event.kind = events::SyncState::Kind::Candidate;
    event.mode = Mode::event_mode;
    event.frequency = frequency;
    event.dt = offset;
    event.sync.candidate = sync_matches;
    emit(event);
  }

  std::array<std::array<float, kDataSymbols>, kToneRows> data_power{};
  for (int row = 0; row < kToneRows; ++row) {
    std::copy(tone_power[static_cast<std::size_t>(row)].begin() + 7,
              tone_power[static_cast<std::size_t>(row)].begin() + 36,
              data_power[static_cast<std::size_t>(row)].begin());
    std::copy(tone_power[static_cast<std::size_t>(row)].begin() + 43,
              tone_power[static_cast<std::size_t>(row)].begin() + 72,
              data_power[static_cast<std::size_t>(row)].begin() + 29);
  }

  std::array<float, 3 * kDataSymbols> amplitude_llr{};
  std::array<float, 3 * kDataSymbols> logarithmic_llr{};

  for (int symbol = 0; symbol < kDataSymbols; ++symbol) {
    std::array<float, kToneRows> power{};
    for (int row = 0; row < kToneRows; ++row) {
      power[static_cast<std::size_t>(row)] =
          data_power[static_cast<std::size_t>(row)][static_cast<std::size_t>(symbol)];
    }

    std::size_t const bit0 = static_cast<std::size_t>(3 * symbol);
    std::size_t const bit1 = bit0 + 1;
    std::size_t const bit2 = bit0 + 2;

    amplitude_llr[bit0] = std::max({power[4], power[5], power[6], power[7]}) -
                          std::max({power[0], power[1], power[2], power[3]});
    amplitude_llr[bit1] = std::max({power[2], power[3], power[6], power[7]}) -
                          std::max({power[0], power[1], power[4], power[5]});
    amplitude_llr[bit2] = std::max({power[1], power[3], power[5], power[7]}) -
                          std::max({power[0], power[2], power[4], power[6]});

    for (float& value : power) value = std::log(value + 1e-32f);

    logarithmic_llr[bit0] = std::max({power[4], power[5], power[6], power[7]}) -
                            std::max({power[0], power[1], power[2], power[3]});
    logarithmic_llr[bit1] = std::max({power[2], power[3], power[6], power[7]}) -
                            std::max({power[0], power[1], power[4], power[5]});
    logarithmic_llr[bit2] = std::max({power[1], power[3], power[5], power[7]}) -
                            std::max({power[0], power[2], power[4], power[6]});
  }

  auto normalize = [](auto& values) {
    float sum = 0.0f;
    float squares = 0.0f;
    for (float value : values) {
      sum += value;
      squares += value * value;
    }
    float const mean = sum / values.size();
    float const mean_square = squares / values.size();
    float const variance = mean_square - mean * mean;
    float const sigma = std::sqrt(variance > 0.0f ? variance : mean_square);

    if (!std::isfinite(sigma) || sigma <= 1.0e-12f) {
      values.fill(0.0f);
      return;
    }
    for (float& value : values) value = (value / sigma) * 2.83f;
  };

  normalize(amplitude_llr);
  normalize(logarithmic_llr);
  // Later erasure passes modify amplitude_llr. Preserve the full evidence
  // for one optional stronger BP run after the standard decode sequence.
  auto const pristine_amplitude_llr = amplitude_llr;

  std::array<std::int8_t, kPayloadBits> payload{};
  std::array<std::int8_t, kCodeLength> codeword{};

  for (int pass = 1; pass <= 5; ++pass) {
    std::array<float, 3 * kDataSymbols> rescue_llr{};
    if (pass == 5) {
      // Rare weak candidates may need more iterations after the early-stall
      // heuristic fires. Bound that work per receive frame and require solid
      // fine Costas synchronization rather than lowering sync thresholds.
      if (sync_matches < 9 || rescue_budget <= 0) break;
      --rescue_budget;
      rescue_llr = pristine_amplitude_llr;
      for (auto& value : rescue_llr) value *= 1.5f;
    } else if (pass == 3) {
      std::fill(amplitude_llr.begin(), amplitude_llr.begin() + 24, 0.0f);
    } else if (pass == 4) {
      std::fill(amplitude_llr.begin() + 24, amplitude_llr.begin() + 48, 0.0f);
    }

    auto const& llr = (pass == 5) ? rescue_llr
                      : (pass == 2) ? logarithmic_llr : amplitude_llr;
    hard_errors = pass == 5
        ? decoder::belief_propagation(llr, payload, codeword, 150, false)
        : decoder::belief_propagation(llr, payload, codeword);
    snr = -99.0f;

    if (std::all_of(codeword.begin(), codeword.end(),
                    [](std::int8_t bit) { return bit == 0; })) {
      continue;
    }

    bool const acceptable =
        hard_errors >= 0 && hard_errors < 60 &&
        !(sync < 2.0f && hard_errors > 35) &&
        !(pass > 2 && hard_errors > 39) &&
        !(pass == 4 && hard_errors > 30);

    if (!acceptable) {
      hard_errors = -1;
      continue;
    }

    if (!decoder::crc_ok(payload)) continue;

    if (sync_stats) {
      events::SyncState event;
      event.kind = events::SyncState::Kind::Decoded;
      event.mode = Mode::event_mode;
      event.frequency = frequency;
      event.dt = refined_offset;
      event.sync.decoded = sync;
      emit(event);
    }

    std::string const text = decoder::decode_message(payload);
    int const type = (payload[72] << 2) | (payload[73] << 1) | payload[74];

    std::array<int, kChannelSymbols> tones{};
    legacy_encode(type, kCostas, text.data(), tones.data());

    if (subtract) subtract_reference(make_reference(tones, frequency), refined_offset);

    float signal_power = 0.0f;
    for (std::size_t i = 0; i < tones.size(); ++i) {
      float const value =
          tone_power[static_cast<std::size_t>(tones[i])][i];
      signal_power += value * value;
    }

    snr = std::max(10.0f * std::log10(std::max(signal_power / linear_base - 1.0f,
                                               1.259e-10f)) - 32.0f,
                   -60.0f);

    return Frame{type, text};
  }

  return std::nullopt;
}

// ---------------------------------------------------------------------------
// Signal reconstruction
// ---------------------------------------------------------------------------

template <typename Mode>
std::vector<std::complex<float>> ChannelDecoder<Mode>::make_reference(
    std::array<int, kChannelSymbols> const& tones, float frequency) {
  float const carrier = kTau * frequency / 12000.0f;
  float phase = 0.0f;

  std::vector<std::complex<float>> reference;
  reference.reserve(kChannelSymbols * Mode::nsps);

  for (int symbol = 0; symbol < kChannelSymbols; ++symbol) {
    float const step = carrier + kTau * static_cast<float>(tones[static_cast<std::size_t>(symbol)]) /
                                      Mode::nsps;
    for (int i = 0; i < Mode::nsps; ++i) {
      reference.push_back(std::polar(1.0f, phase));
      phase = std::fmod(phase + step, kTau);
    }
  }
  return reference;
}

template <typename Mode>
void ChannelDecoder<Mode>::subtract_reference(
    std::vector<std::complex<float>> const& reference, float offset) {
  int const start = static_cast<int>(offset * 12000.0f);
  std::size_t const reference_start =
      start < 0 ? static_cast<std::size_t>(-start) : 0;
  std::size_t const signal_start = start > 0 ? static_cast<std::size_t>(start) : 0;

  if (reference_start >= reference.size() || signal_start >= samples_.size()) return;

  std::size_t const count =
      std::min(reference.size() - reference_start, samples_.size() - signal_start);

  for (std::size_t i = 0; i < count; ++i) {
    subtract_work_[i] = samples_[signal_start + i] * std::conj(reference[reference_start + i]);
  }
  std::fill(subtract_work_.begin() + static_cast<std::ptrdiff_t>(count),
            subtract_work_.end(), kZero);

  pocketfft::c2c<float>(shape_t{static_cast<std::size_t>(Mode::nmax)},
                        stride_t{kComplexStride}, stride_t{kComplexStride},
                        shape_t{0}, true, subtract_work_.data(),
                        subtract_work_.data(), 1.0f, 1);

  for (std::size_t i = 0; i < subtract_work_.size(); ++i) {
    subtract_work_[i] *= subtract_filter_[i];
  }

  // The frequency-domain subtraction filter is already normalized by
  // 1/N when it is constructed. As with FFTW_BACKWARD, the inverse here is
  // unscaled; normalizing again would leave only 1/N of the cancellation
  // signal and mask weaker stations during later decode passes.
  pocketfft::c2c<float>(shape_t{static_cast<std::size_t>(Mode::nmax)},
                        stride_t{kComplexStride}, stride_t{kComplexStride},
                        shape_t{0}, false, subtract_work_.data(),
                        subtract_work_.data(), 1.0f, 1);

  for (std::size_t i = 0; i < count; ++i) {
    samples_[signal_start + i] -=
        2.0f * std::real(subtract_work_[i] * reference[reference_start + i]);
  }
}

// ---------------------------------------------------------------------------
// Frame entry point
// ---------------------------------------------------------------------------

template <typename Mode>
std::size_t ChannelDecoder<Mode>::operator()(dec_data const& data, int position,
                                             int size, EventEmitter emit) {
  int const pos = std::max(0, position);
  int const count = std::clamp(size, 0, Mode::nmax);
  if (pos >= kJs8NtMax * kJs8RxSampleRate || count == 0) return 0;

  if (data.params.syncStats) emit(events::SyncStart{pos, count});

  samples_.fill(0.0f);

  int const available = kJs8NtMax * kJs8RxSampleRate - pos;
  if (available < count) {
    int const first = available;
    int const second = count - first;

    for (int i = 0; i < first; ++i) {
      samples_[static_cast<std::size_t>(i)] =
          static_cast<float>(data.samples[static_cast<std::size_t>(pos + i)]);
    }
    for (int i = 0; i < second; ++i) {
      samples_[static_cast<std::size_t>(first + i)] =
          static_cast<float>(data.samples[static_cast<std::size_t>(i)]);
    }
  } else {
    for (int i = 0; i < count; ++i) {
      samples_[static_cast<std::size_t>(i)] =
          static_cast<float>(data.samples[static_cast<std::size_t>(pos + i)]);
    }
  }

  Frame::Map decoded;
  int rescue_budget = 32;

  for (int pass = 1; pass <= 3; ++pass) {
    auto candidates = find_candidates(data.params.nfa, data.params.nfb);
    if (candidates.empty()) break;

    int const preferred = data.params.nfqso;
    std::sort(candidates.begin(), candidates.end(),
              [preferred](Candidate const& a, Candidate const& b) {
                float const da = std::abs(a.frequency - preferred);
                float const db = std::abs(b.frequency - preferred);
                if (da < 10.0f && db >= 10.0f) return true;
                if (db < 10.0f && da >= 10.0f) return false;
                return std::tie(da, a.frequency) < std::tie(db, b.frequency);
              });

    prepare_baseband();

    bool const subtract = pass < 3;
    bool improved = false;

    for (auto candidate : candidates) {
      float frequency = candidate.frequency;
      float offset = candidate.offset;
      int hard_errors = -1;
      float snr = 0.0f;

      auto frame = attempt_decode(data.params.syncStats, subtract, frequency,
                                  offset, hard_errors, snr, rescue_budget, emit);
      if (!frame) continue;

      int const rounded_snr = static_cast<int>(std::round(snr));
      auto [slot, inserted] = decoded.try_emplace(std::move(*frame), rounded_snr);
      if (!inserted && slot->second >= rounded_snr) continue;

      improved = true;
      if (!inserted) slot->second = rounded_snr;

      emit(events::Decoded{data.params.utc,
                           rounded_snr,
                           offset - Mode::start_seconds,
                           frequency,
                           slot->first.text,
                           slot->first.type,
                           1.0f - hard_errors / 60.0f,
                           Mode::event_mode});
    }

    if (!improved) break;
  }

  return decoded.size();
}

}  // namespace

std::size_t legacy_decode(DecodeState const& state,
                          std::function<void(events::Variant const&)> emit_fn) {
  static thread_local std::unique_ptr<ChannelDecoder<ProfileA>> decoder_a;
  static thread_local std::unique_ptr<ChannelDecoder<ProfileB>> decoder_b;
  static thread_local std::unique_ptr<ChannelDecoder<ProfileC>> decoder_c;
  static thread_local std::unique_ptr<ChannelDecoder<ProfileE>> decoder_e;
  static thread_local std::unique_ptr<ChannelDecoder<ProfileI>> decoder_i;

  auto emit = [&](events::Variant const& event) {
    if (emit_fn) emit_fn(event);
  };

  auto run = [&](auto& decoder, int position, int size) {
    using Decoder =
        typename std::remove_reference_t<decltype(decoder)>::element_type;
    if (!decoder) decoder = std::make_unique<Decoder>();
    return (*decoder)(state, position, size, emit);
  };

  int const submodes = state.params.nsubmodes;
  std::size_t total = 0;

  emit(events::DecodeStarted{submodes});

  if ((submodes & (1 << ProfileI::bit_index)) != 0) {
    total += run(decoder_i, state.params.kposI, state.params.kszI);
  }
  if ((submodes & (1 << ProfileE::bit_index)) != 0) {
    total += run(decoder_e, state.params.kposE, state.params.kszE);
  }
  if ((submodes & (1 << ProfileC::bit_index)) != 0) {
    total += run(decoder_c, state.params.kposC, state.params.kszC);
  }
  if ((submodes & (1 << ProfileB::bit_index)) != 0) {
    total += run(decoder_b, state.params.kposB, state.params.kszB);
  }
  if ((submodes & (1 << ProfileA::bit_index)) != 0) {
    total += run(decoder_a, state.params.kposA, state.params.kszA);
  }

  emit(events::DecodeFinished{total});
  return total;
}

void legacy_encode(int const type, protocol::CostasArray const& costas,
                   char const* const message, int* const tones) {
  std::array<std::uint8_t, 11> bytes{};

  for (int i = 0, j = 0; i < kFrameCharacters; i += 4, j += 3) {
    std::uint32_t const word = (static_cast<std::uint32_t>(decoder::word_for(message[i])) << 18) |
                               (static_cast<std::uint32_t>(decoder::word_for(message[i + 1])) << 12) |
                               (static_cast<std::uint32_t>(decoder::word_for(message[i + 2])) << 6) |
                               static_cast<std::uint32_t>(decoder::word_for(message[i + 3]));

    bytes[static_cast<std::size_t>(j)] = static_cast<std::uint8_t>(word >> 16);
    bytes[static_cast<std::size_t>(j + 1)] = static_cast<std::uint8_t>(word >> 8);
    bytes[static_cast<std::size_t>(j + 2)] = static_cast<std::uint8_t>(word);
  }

  for (int i = 0; i < kFrameCharacters; ++i) {
    if (decoder::word_for(message[i]) == 0xFF) {
      throw std::runtime_error("Invalid character in message");
    }
  }

  bytes[9] = static_cast<std::uint8_t>((type & 0x7) << 5);
  std::uint16_t const crc = decoder::crc12(bytes.data(), bytes.size());
  bytes[9] |= static_cast<std::uint8_t>((crc >> 7) & 0x1F);
  bytes[10] = static_cast<std::uint8_t>((crc & 0x7F) << 1);

  int* costas_out = tones;
  int* parity_out = tones + 7;
  int* data_out = tones + 43;

  for (auto const& array : costas) {
    std::copy(array.begin(), array.end(), costas_out);
    costas_out += 36;
  }

  std::size_t bit_count = 0;
  std::size_t byte_index = 0;
  std::uint8_t byte_mask = 0x80;
  std::uint8_t parity_word = 0;
  std::uint8_t data_word = 0;

  for (std::size_t i = 0; i < kPayloadBits; ++i) {
    bool parity = false;
    for (std::size_t j = 0; j < kPayloadBits; ++j) {
      bool const message_bit =
          (bytes[j / 8] & (1u << (7 - (j % 8)))) != 0;
      parity ^= decoder::parity_bit(i, j) && message_bit;
    }

    parity_word = static_cast<std::uint8_t>((parity_word << 1) | (parity ? 1u : 0u));
    data_word = static_cast<std::uint8_t>(
        (data_word << 1) | ((bytes[byte_index] & byte_mask) != 0 ? 1u : 0u));
    byte_mask = byte_mask == 1 ? (++byte_index, 0x80) : static_cast<std::uint8_t>(byte_mask >> 1);

    if (++bit_count == 3) {
      *parity_out++ = parity_word;
      *data_out++ = data_word;
      parity_word = 0;
      data_word = 0;
      bit_count = 0;
    }
  }
}

}  // namespace js8core

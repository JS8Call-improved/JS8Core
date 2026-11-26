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
#include <utility>
#include <vector>

#include <Eigen/Dense>
#define POCKETFFT_CACHE_SIZE 1000000
#include "pocketfft/pocketfft_hdronly.h"

#include "js8core/compat/numbers.hpp"
#include "js8core/protocol/constants.hpp"
#include "js8core/protocol/costas.hpp"
#include "js8core/protocol/varicode.hpp"

#include "adaptive_sic.hpp"
#include "aided_redemodulation.hpp"
#include "coherent_costas.hpp"
#include "decode_quality.hpp"
#include "decoder_tables.hpp"
#include "frame_history.hpp"
#include "ldpc.hpp"
#if !defined(JS8CORE_DISABLE_SOFT_DEMAPPER)
#include "soft_likelihood.hpp"
#include "soft_likelihoods.hpp"
#endif
#include "symbol_tracking.hpp"
#include "tracked_reference.hpp"

namespace js8core {

using dec_data = DecodeState;
using EventEmitter = std::function<void(events::Variant const&)>;
using namespace decoder::data;

namespace {

using namespace decoder::data;
using decoder::Frame;

using shape_t = std::vector<std::size_t>;
using stride_t = std::vector<std::ptrdiff_t>;

constexpr std::ptrdiff_t kComplexStride =
    static_cast<std::ptrdiff_t>(sizeof(std::complex<float>));
constexpr std::ptrdiff_t kFloatStride = static_cast<std::ptrdiff_t>(sizeof(float));
constexpr std::complex<float> kZero{0.0f, 0.0f};

struct RetryBudget {
  int rescue;
  int redemodulation;
  int whitening;
  int quadratic;
};

// Last try (pass 6): ordered statistics decoding of the full-evidence reading
// when belief propagation fails. Its search order comes from a short, damped
// re-run of belief propagation on the same evidence (Zhang, Yue, Li, Vucetic,
// arXiv 2306.00443, 2023): floor(girth / 4) + 1 = 3 rounds for this code's
// girth of 8. Candidates are still scored against the evidence.
constexpr int kOsdOrder = 2;
constexpr int kRefineRounds = 3;
constexpr float kRefineFeedback = 0.6f;
// Ordered statistics decoding always returns its closest codeword, so a
// last-try answer needs at least 9 of 21 Costas tones matching and a
// discrepancy of at most 6.5% of the total |LLR| (set on Normal-speed test
// signals and noise-only audio); the CRC check still applies.
constexpr int kOsdMinSyncMatches = 9;
constexpr float kOsdMaxDiscrepancy = 0.065f;
// It must also beat the runner-up candidate by at least 0.25% of the total
// |LLR| (Forney's test, approximated with the runner-up). Each last-try
// answer that reaches the CRC check has about a 1 in 4,096 chance of being
// wrong; this gap cuts how many reach it.
constexpr float kOsdMinRunnerUpGap = 0.0025f;
// A last-try answer that passes the CRC check is still refused when no
// unpacker reads it: fast data when type bit 4 is set, else data, heartbeat,
// compound or directed (JS8Call-improved's order). Such a frame would be shown
// as its 12 raw characters. About a third of random 87-bit payloads are
// unreadable, so this removes about a third of wrong last-try answers that
// pass the CRC. Passes 1 to 5 keep every CRC-valid frame: a refused frame is
// not subtracted either, and on test audio with random frame text the
// frames under it were then lost.
bool frame_readable(Frame const& candidate) {
  namespace vc = js8core::protocol::varicode;
  auto const& frame = candidate.text;
  if (candidate.type & 4) return !vc::unpack_fast_data_message(frame).empty();
  if (!vc::unpack_data_message(frame).empty()) return true;
  std::uint8_t kind = 0;
  std::uint8_t bits3 = 0;
  bool alt = false;
  if (vc::unpack_heartbeat_message(frame, &kind, &alt, &bits3).size() >= 2) return true;
  kind = 0;
  bits3 = 0;
  std::uint16_t num = 0;
  if (vc::unpack_compound_message(frame, &kind, &num, &bits3).size() >= 2) return true;
  kind = 0;
  return !vc::unpack_directed_message(frame, &kind).empty();
}

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

enum class CandidateSearch { Ordinary, Tracking, Deep };
constexpr std::size_t kDeepSyncSeedLimit = 8;
constexpr int kDeepSyncDecodeLimit = 3;
constexpr float kDeepSyncSeedFloor = 1.0f;
constexpr double kDeepSyncSupport = 0.16;

struct DeepSyncWindow {
  int count = 0;
  Frame::Map decoded;
};

// Per-submode decoder. All buffers are sized from the ModeProfile so that one
// template instantiation handles each JS8 speed level.
template <typename Mode>
class ChannelDecoder {
 public:
  ChannelDecoder() { initialize(); }

  std::size_t operator()(dec_data const& data, int position, int size,
                         EventEmitter emit, std::size_t& tracked_cancellations,
                         DecodeControl const& control, DeepSyncWindow* deferred = nullptr);
  std::size_t deep_sync(dec_data const& data, DeepSyncWindow& window, EventEmitter emit,
                        DecodeControl const& control);

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

  std::vector<Candidate> find_candidates(int low_hz, int high_hz,
      DecodeControl const* control, CandidateSearch search = CandidateSearch::Ordinary);
  float alignment_score(int offset, float frequency_delta, int available = Mode::payload_samples);
  // Frames reported so far in this period, for the last-try position check.
  std::vector<decoder::ReportedFrame> reported_;
  struct AttemptResult {
    Frame frame;
    bool subtracted;
  };
  std::optional<AttemptResult> attempt_decode(
      bool sync_stats, bool subtract, float& frequency, float& offset,
      int& hard_errors, float& snr, int& rescue_budget, EventEmitter emit,
      SymbolTracking tracking = SymbolTracking::Disabled, int received_samples = 0,
      std::optional<decoder::TrackedTrajectory>* trajectory = nullptr,
      DecodeControl const* control = nullptr, decoder::CoherentCostasPeak const* deep = nullptr,
      bool coherent_likelihood = false, int* adaptive_budget = nullptr,
      RetryBudget* retry_budget = nullptr);

  // --- Signal reconstruction -----------------------------------------------

  std::vector<std::complex<float>> make_reference(std::array<int, kChannelSymbols> const& tones,
                                                  float frequency);
  bool subtract_reference(std::vector<std::complex<float>> const& reference,
                          float offset, std::array<int, kChannelSymbols> const& tones,
                          decoder::TrackedTrajectory const& seed, int received_samples,
                          int* adaptive_budget,
                          DecodeControl const* control);

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
float ChannelDecoder<Mode>::alignment_score(int offset, float frequency_delta, int available) {
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
      if (start < 0 || start + Mode::downsample_sps > available) {
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
    int high_hz, DecodeControl const* control, CandidateSearch search) {
  bool const optional = search != CandidateSearch::Ordinary;
  // Invalid or inverted bands must not reach the polynomial baseline fit or
  // be turned into negative/huge vector allocation sizes.
  if (low_hz >= high_hz || high_hz <= 0 || low_hz > 4910) return {};
  average_spectrum_.fill(0.0f);

  for (int j = 0; j < Mode::spectra; ++j) {
    if (control && (control->is_canceled() || (optional && !control->allow_optional()))) return {};
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
    if (control && (control->is_canceled() || (optional && !control->allow_optional()))) return {};
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

    // The spectra fill the whole 2-symbol FFT span with audio under a window
    // that peaks one symbol in, so a tone scores best when the spectrum
    // starts 2 steps (half a symbol) before it. Use the actual window start
    // (start_step + best_offset), then advance 2 steps to estimate the
    // signal start. start_step rounds down from the nominal start time.
    entries.push_back(Candidate{
        Mode::bin_hz * bin,
        (best_offset + Mode::start_step + 2) * Mode::step_seconds - Mode::start_seconds,
        best});
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
  float const floor = search == CandidateSearch::Deep ? kDeepSyncSeedFloor : kMinSync;
  std::size_t const limit = search == CandidateSearch::Deep ? kDeepSyncSeedLimit : kMaxCandidates;

  for (std::size_t index : order) {
    if (removed[index]) continue;

    Candidate const& candidate = entries[index];
    if (!std::isfinite(candidate.score) || candidate.score < floor) break;

    selected.push_back(candidate);
    if (selected.size() >= limit) break;

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
auto ChannelDecoder<Mode>::attempt_decode(
    bool sync_stats, bool subtract, float& frequency, float& offset,
    int& hard_errors, float& snr, int& rescue_budget, EventEmitter emit,
    SymbolTracking tracking, int received_samples,
    std::optional<decoder::TrackedTrajectory>* trajectory, DecodeControl const* control,
    decoder::CoherentCostasPeak const* deep, [[maybe_unused]] bool coherent_likelihood,
    int* adaptive_budget, RetryBudget* retry_budget)
    -> std::optional<AttemptResult> {
  if (trajectory) trajectory->reset();
  auto allowed = [&] {
    return !control || (deep ? control->allow_optional() :
        (!control->is_canceled() &&
         (tracking == SymbolTracking::Disabled || control->allow_optional())));
  };
  if (!allowed()) return std::nullopt;
  constexpr float frequency_resolution = 12000.0f / Mode::fft_span;
  constexpr float down_rate = 12000.0f / Mode::downsample_factor;
  constexpr float down_period = 1.0f / down_rate;

  int const bin = static_cast<int>(std::round(frequency / frequency_resolution));
  float const linear_base =
      std::pow(10.0f, 0.1f * (average_spectrum_[static_cast<std::size_t>(bin)] -
                              Mode::baseline_offset));

  float const acquisition_frequency = frequency;
  extract_channel(frequency);

  int best_time = 0;
  float best_delta = 0.0f;
  if (deep) {
    // The coherent peak is already registered against the actual FFT bin.
    best_time = deep->frame_start;
    best_delta = static_cast<float>(deep->frequency_offset_hz);
    constexpr float resolution = 12000.0f / Mode::ds_fft_span;
    frequency = std::round(frequency / resolution) * resolution;
  } else {
    int origin = static_cast<int>(std::round((offset + Mode::start_seconds) * down_rate));
    float best_score = 0.0f;
    for (int candidate = origin - Mode::quarter_symbol;
         candidate <= origin + Mode::quarter_symbol; ++candidate) {
      float const score = alignment_score(candidate, 0.0f);
      if (score > best_score) {
        best_score = score;
        best_time = candidate;
      }
    }

    origin = static_cast<int>(std::round(best_time * down_period * down_rate));
    best_score = 0.0f;
    for (int step = -kFreqSearchSteps; step <= kFreqSearchSteps; ++step) {
      float const delta = step * 0.5f;
      float const score = alignment_score(origin, delta);
      if (score > best_score) {
        best_score = score;
        best_delta = delta;
      }
    }

    // Time again at the corrected frequency, over half the first window.
    float best_retime = 0.0f;
    best_time = origin;
    for (int candidate = origin - Mode::quarter_symbol / 2;
         candidate <= origin + Mode::quarter_symbol / 2; ++candidate) {
      float const score = alignment_score(candidate, best_delta);
      if (score > best_retime) {
        best_retime = score;
        best_time = candidate;
      }
    }
  }
  float const refined_offset = best_time * down_period;
  int const origin = static_cast<int>(std::round(refined_offset * down_rate));
  int const received_limit = std::min(Mode::ds_fft_out, received_samples / Mode::downsample_factor);
  int const demodulation_limit = deep ? received_limit : Mode::payload_samples;
  int rotation_limit = demodulation_limit;
  constexpr int last_data_end = (kChannelSymbols - kSyncSymbols / 3) * Mode::downsample_sps;
#if !defined(JS8CORE_DISABLE_SOFT_DEMAPPER)
  // The ordinary demodulator stops at a fixed buffer index. A frame starting
  // later can still have a complete final Costas block in the received audio.
  // Extend pilot observations only; recovering data beyond the ordinary
  // demodulation window is left to deep sync or symbol tracking.
  std::optional<decoder::ToneObservations> observations;
  std::optional<decoder::DisturbanceModel> disturbance;
  std::optional<decoder::PilotChannel> affine_channel;
  bool const try_whitening = retry_budget && retry_budget->whitening > 0;
  bool const try_quadratic = retry_budget && retry_budget->quadratic > 0;
  if ((coherent_likelihood || try_whitening || try_quadratic) && (!control || control->allow_optional()) &&
      (tracking != SymbolTracking::Disabled ||
       (best_time >= 0 && best_time + last_data_end <= demodulation_limit &&
        best_time + Mode::payload_samples <= received_limit))) {
    observations.emplace();
    if (tracking == SymbolTracking::Disabled)
      rotation_limit = std::max(rotation_limit, best_time + Mode::payload_samples);
  }
#endif

  float const phase_step = -best_delta * (kTau / down_rate);
  std::complex<float> const rotation_step = std::polar(1.0f, phase_step);
  std::complex<float> rotation{1.0f, 0.0f};
  for (int i = 0; i < rotation_limit; ++i) {
    rotation *= rotation_step;
    channel_[static_cast<std::size_t>(i)] *= rotation;
  }

  offset = refined_offset;
  frequency += best_delta;
  float const sync = alignment_score(origin, 0.0f, demodulation_limit);

  std::array<std::array<float, kChannelSymbols>, kToneRows> tone_power{};
  auto transform_symbol = [&] {
    pocketfft::c2c<float>(shape_t{static_cast<std::size_t>(Mode::downsample_sps)},
                          stride_t{kComplexStride}, stride_t{kComplexStride},
                          shape_t{0}, true, symbol_fft_.data(),
                          symbol_fft_.data(), 1.0f, 1);
  };
  auto store_magnitudes = [&](int symbol) {
    for (int row = 0; row < kToneRows; ++row)
      tone_power[row][symbol] = std::abs(symbol_fft_[row]) / 1000.0f;
  };

  std::vector<std::complex<float>> tracked_symbols;
  std::optional<decoder::TrackedTrajectory> fitted_trajectory;
  if (tracking != SymbolTracking::Disabled) {
    // Re-extract the unmodified channel: the established path rotates only its
    // legacy payload prefix. Tracking needs the full received frame and margins.
    extract_channel(acquisition_frequency);
    int const available = std::clamp(received_samples / Mode::downsample_factor,
                                     0, Mode::ds_fft_out);
    std::vector<std::complex<float>> tracking_channel(channel_.begin(), channel_.begin() + available);
    for (int i = 0; i < available; ++i)
      tracking_channel[static_cast<std::size_t>(i)] *=
          std::polar(1.0f, -kTau * best_delta * (i + 1) / down_rate);
    auto estimate = decoder::estimate_pilot_tracking(
        tracking_channel, down_rate, Mode::downsample_sps, best_time, kCostas, control);
    if (tracking == SymbolTracking::Frequency) estimate.timing_valid = false;
    if (tracking == SymbolTracking::Timing) estimate.frequency_valid = false;
    if (!estimate.frequency_valid && !estimate.timing_valid) return std::nullopt;
    tracked_symbols = decoder::extract_tracked_symbols(
        tracking_channel, down_rate, Mode::downsample_sps, best_time, estimate, control);
    if (tracked_symbols.empty()) return std::nullopt;
    constexpr float channel_resolution = 12000.0f / Mode::ds_fft_span;
    frequency = std::round(acquisition_frequency / channel_resolution) * channel_resolution + best_delta;
    if (estimate.frequency_valid) frequency += static_cast<float>(estimate.frequency_at_start_hz);
    if (estimate.timing_valid) offset += static_cast<float>(estimate.timing_at_start_samples / down_rate);
    if (trajectory && estimate.frequency_valid && estimate.timing_valid) {
      fitted_trajectory = decoder::TrackedTrajectory{
          12000.0, Mode::nsps, static_cast<double>(best_time * Mode::downsample_factor),
          frequency, estimate.frequency_hz_per_symbol,
          estimate.timing_at_start_samples * Mode::downsample_factor,
          estimate.timing_samples_per_symbol * Mode::downsample_factor};
    }
    // The fixed reference cannot represent a tracked timing/frequency trajectory.
    subtract = false;
  }

  for (int symbol = 0; symbol < kChannelSymbols; ++symbol) {
    if (!allowed()) return std::nullopt;
    int const start = best_time + symbol * Mode::downsample_sps;
    bool const legacy_symbol = !tracked_symbols.empty() ||
        (start >= 0 && start + Mode::downsample_sps <= demodulation_limit);
    bool demodulate = legacy_symbol;
#if !defined(JS8CORE_DISABLE_SOFT_DEMAPPER)
    demodulate |= observations.has_value();
#endif
    if (!demodulate) continue;
    if (!tracked_symbols.empty()) {
      auto const first = tracked_symbols.begin() + symbol * Mode::downsample_sps;
      std::copy_n(first, Mode::downsample_sps, symbol_fft_.begin());
    } else {
      std::copy(channel_.begin() + start,
                channel_.begin() + start + Mode::downsample_sps,
                symbol_fft_.begin());
    }

    transform_symbol();

#if !defined(JS8CORE_DISABLE_SOFT_DEMAPPER)
    if (observations) {
      auto& bins = (*observations)[static_cast<std::size_t>(symbol)];
      std::copy_n(symbol_fft_.begin(), kToneRows, bins.begin());
    }
#endif
    if (legacy_symbol) store_magnitudes(symbol);
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

  auto max_tone_llrs = [&](bool logarithmic) {
    std::array<float, kCodeLength> llrs{};
    for (std::size_t symbol = 0; symbol < kDataSymbols; ++symbol) {
      std::array<float, kToneRows> power{};
      for (std::size_t row = 0; row < kToneRows; ++row) {
        float const value = tone_power[row][symbol + (symbol < 29 ? 7 : 14)];
        power[row] = logarithmic ? std::log(value + 1e-32f) : value;
      }
      llrs[3 * symbol] = std::max({power[4], power[5], power[6], power[7]}) -
                         std::max({power[0], power[1], power[2], power[3]});
      llrs[3 * symbol + 1] = std::max({power[2], power[3], power[6], power[7]}) -
                             std::max({power[0], power[1], power[4], power[5]});
      llrs[3 * symbol + 2] = std::max({power[1], power[3], power[5], power[7]}) -
                             std::max({power[0], power[2], power[4], power[6]});
    }
    normalize(llrs);
    return llrs;
  };

  auto amplitude_llr = max_tone_llrs(false);
  // Later erasure passes modify amplitude_llr. Preserve the full evidence
  // for stronger BP and the optional last-try OSD.
  auto const pristine_amplitude_llr = amplitude_llr;

  auto pass_two_llrs = [&]([[maybe_unused]] int available) {
#if !defined(JS8CORE_DISABLE_SOFT_DEMAPPER)
    if (tracking == SymbolTracking::Disabled) {
      if (auto soft = decoder::soft_frame_llrs(tone_power, kCostas, best_time,
                                               Mode::downsample_sps, available))
        return *soft;
    }
#endif
    // Only compute the legacy log metric if pass 2 actually needs it.
    return max_tone_llrs(true);
  };

  std::array<std::int8_t, kPayloadBits> payload{};
  std::array<std::int8_t, kCodeLength> codeword{};

  auto optional_allowed = [&] { return !control || control->allow_optional(); };
  auto accepts_codeword = [&](int pass, float result_sync) {
    if (std::all_of(codeword.begin(), codeword.end(),
                    [](std::int8_t bit) { return bit == 0; }))
      return false;
    // OSD's weighted discrepancy limits do not replace the hard-error limits.
    if (!decoder::hard_errors_acceptable(pass, hard_errors, result_sync)) {
      hard_errors = -1;
      return false;
    }
    return decoder::crc_ok(payload);
  };
  auto make_frame = [&] {
    return Frame{(payload[72] << 2) | (payload[73] << 1) | payload[74],
                 decoder::decode_message(payload)};
  };
  auto suitable_last_try = [&](Frame const& frame, float hz, float dt) {
    return frame_readable(frame) && !decoder::different_frame_near(reported_, hz, dt, frame);
  };
  auto finalize = [&](Frame frame, float result_sync, bool cancel_signal) {
    if (sync_stats) {
      events::SyncState event;
      event.kind = events::SyncState::Kind::Decoded;
      event.mode = Mode::event_mode;
      event.frequency = frequency;
      event.dt = offset;
      event.sync.decoded = result_sync;
      emit(event);
    }

    std::array<int, kChannelSymbols> tones{};
    legacy_encode(frame.type, kCostas, frame.text.data(), tones.data());
    bool subtracted = false;
    if (cancel_signal && subtract && !deep) {
      decoder::TrackedTrajectory const seed{12000.0, Mode::nsps,
          static_cast<double>(best_time * Mode::downsample_factor), frequency};
      subtracted = subtract_reference(make_reference(tones, frequency), refined_offset,
                                      tones, seed, received_samples, adaptive_budget, control);
    }

    float signal_power = 0.0f;
    for (std::size_t i = 0; i < tones.size(); ++i) {
      float const value = tone_power[static_cast<std::size_t>(tones[i])][i];
      signal_power += value * value;
    }
    snr = std::max(10.0f * std::log10(std::max(signal_power / linear_base - 1.0f,
                                                1.259e-10f)) - 32.0f,
                    -60.0f);
    if (trajectory) *trajectory = fitted_trajectory;
    return AttemptResult{std::move(frame), subtracted};
  };

  constexpr int max_rescue_checks = 14;
  constexpr int max_aided_checks = 8;
  decoder::BpNearMiss near_miss;
  std::array<float, kCodeLength> near_miss_llrs{};
  int source_pass = 0;
  bool const gather_near_miss = retry_budget && sync_matches >= 9 &&
      (retry_budget->rescue > 0 || retry_budget->redemodulation > 0);
  if (gather_near_miss) near_miss.failed_checks = max_rescue_checks + 1;
  auto run_bp = [&](std::array<float, kCodeLength> const& llrs, int pass,
                    int iterations = 60, bool early_abort = true,
                    DecodeControl const* bp_control = nullptr) {
    int const before = near_miss.failed_checks;
    hard_errors = decoder::belief_propagation(llrs, payload, codeword, iterations, early_abort,
        gather_near_miss ? &near_miss : nullptr, bp_control);
    if (near_miss.failed_checks < before) {
      source_pass = pass;
      near_miss_llrs = llrs;
    }
  };

  for (int pass = 1; pass <= (deep ? 4 : 6); ++pass) {
    if (!allowed()) return std::nullopt;
    std::array<float, 3 * kDataSymbols> rescue_llr{};
    if (pass == 6) {
      // Last try: ordered statistics decoding on the full evidence, for the
      // ordinary passes only (tracked attempts are left as they are).
      if (tracking != SymbolTracking::Disabled || sync_matches < kOsdMinSyncMatches) break;
      float evidence = 0.0f;
      for (float value : pristine_amplitude_llr) evidence += std::abs(value);
      if (evidence <= 0.0f || !optional_allowed()) break;
      std::array<float, 3 * kDataSymbols> refined{};
      decoder::refine_reliabilities(pristine_amplitude_llr, kRefineRounds, kRefineFeedback,
                                    refined);
      float discrepancy = 0.0f;
      float runner_up = 0.0f;
      hard_errors = decoder::ordered_statistics_decode(
          pristine_amplitude_llr, payload, codeword, kOsdOrder, &discrepancy, true,
          &runner_up, &refined, control);
      if (hard_errors < 0 || discrepancy > kOsdMaxDiscrepancy * evidence ||
          runner_up - discrepancy < kOsdMinRunnerUpGap * evidence) {
        hard_errors = -1;
        break;
      }
    } else if (pass == 5) {
      // Rare weak candidates may need more iterations after the early-stall
      // heuristic fires. Bound that work per receive frame and require solid
      // fine Costas synchronization rather than lowering sync thresholds.
      if (sync_matches < 9 || rescue_budget <= 0) continue;
      --rescue_budget;
      rescue_llr = pristine_amplitude_llr;
      for (auto& value : rescue_llr) value *= 1.5f;
    } else if (pass == 3) {
      std::fill(amplitude_llr.begin(), amplitude_llr.begin() + 24, 0.0f);
    } else if (pass == 4) {
      std::fill(amplitude_llr.begin() + 24, amplitude_llr.begin() + 48, 0.0f);
    }

    if (pass == 2)
      run_bp(pass_two_llrs(std::min(demodulation_limit, received_limit)), pass);
    else if (pass == 5)
      run_bp(rescue_llr, pass, 150, false);
    else if (pass <= 5)
      run_bp(amplitude_llr, pass);
    snr = -99.0f;

    bool accepted = accepts_codeword(pass, sync);
#if !defined(JS8CORE_DISABLE_SOFT_DEMAPPER)
    // Keep the established pass-2 result whenever it passes all decode gates.
    // Only a rejected result can spend the optional budget on another BP try.
    if (!accepted && pass == 2 && coherent_likelihood && observations && optional_allowed()) {
      disturbance = decoder::estimate_disturbance(*observations, kCostas, true, control);
      if (disturbance) {
        affine_channel = decoder::estimate_pilot_channel(*observations, kCostas, *disturbance, control);
        // An untrusted phase fit still permits the phase-independent Rician retry.
        if (auto llrs = decoder::make_soft_likelihoods(
                *observations, *disturbance, affine_channel ? &*affine_channel : nullptr, control)) {
          run_bp(*llrs, pass);
          accepted = accepts_codeword(pass, sync) && optional_allowed();
        }
      }
    }
#endif
    if (!accepted) continue;
    auto frame = make_frame();
    if (pass == 6 && !suitable_last_try(frame, frequency, offset)) continue;
    if (pass == 6 ? !optional_allowed() : !allowed()) return std::nullopt;
    // OSD results are report-only.
    return finalize(std::move(frame), sync, pass != 6);
  }

#if !defined(JS8CORE_DISABLE_SOFT_DEMAPPER)
  // Keep the established metrics and OSD ahead of this retry. Its evidence is
  // from the same capture, so run BP on it separately.
  if (try_whitening && observations && sync_matches >= kOsdMinSyncMatches && optional_allowed()) {
    if (!coherent_likelihood)
      disturbance = decoder::estimate_disturbance(*observations, kCostas, true, control);
    if (disturbance) {
      if (auto llrs = decoder::make_adaptive_likelihoods(*observations, kCostas, *disturbance, control)) {
        --retry_budget->whitening;
        hard_errors = decoder::belief_propagation(*llrs, payload, codeword, 60, true, nullptr, control);
        if (accepts_codeword(2, sync) && optional_allowed()) {
          auto frame = make_frame();
          if (suitable_last_try(frame, frequency, offset))
            return finalize(std::move(frame), sync, false);
        }
      }
    }
  }
#endif

  auto quadratic_retry = [&]() -> std::optional<AttemptResult> {
#if !defined(JS8CORE_DISABLE_SOFT_DEMAPPER)
    if (!try_quadratic || !observations || sync_matches < kOsdMinSyncMatches ||
        (affine_channel && affine_channel->validation_error < 0.04) || !optional_allowed())
      return std::nullopt;
    if (!disturbance && !coherent_likelihood && !try_whitening)
      disturbance = decoder::estimate_disturbance(*observations, kCostas, true, control);
    if (!disturbance) return std::nullopt;

    --retry_budget->quadratic;
    auto const channel = decoder::estimate_quadratic_channel(*observations, kCostas, *disturbance, control);
    if (!channel) return std::nullopt;
    auto const llrs = decoder::make_soft_likelihoods(*observations, *disturbance, &*channel, control);
    if (!llrs) return std::nullopt;
    hard_errors = decoder::belief_propagation(*llrs, payload, codeword, 60, true, nullptr, control);
    if (!accepts_codeword(2, sync) || !optional_allowed()) return std::nullopt;
    auto frame = make_frame();
    if (!suitable_last_try(frame, frequency, offset)) return std::nullopt;
    // A rejected aided attempt may have replaced the working magnitudes.
    // Report using the original bins that the quadratic model actually scored.
    for (int symbol = 0; symbol < kChannelSymbols; ++symbol)
      for (int row = 0; row < kToneRows; ++row)
        tone_power[row][symbol] = std::abs((*observations)[symbol][row]) / 1000.0f;
    return finalize(std::move(frame), sync, false);
#else
    return std::nullopt;
#endif
  };

  if (!source_pass || !optional_allowed()) return quadratic_retry();
  if (retry_budget->rescue > 0) {
    --retry_budget->rescue;
    auto const seed_llrs = near_miss_llrs;
    int const seed_pass = source_pass;
    for (float scale : {1.0f, 0.85f, 1.15f}) {
      if (scale == 1.0f && seed_pass == 5) continue;  // Already tried a longer run.
      if (!optional_allowed()) return std::nullopt;
      auto llrs = seed_llrs;
      for (auto& value : llrs) value *= scale;
      run_bp(llrs, seed_pass, 90, false, control);
      if (!accepts_codeword(seed_pass, sync)) continue;
      if (!optional_allowed()) return std::nullopt;
      auto frame = make_frame();
      if (suitable_last_try(frame, frequency, offset))
        return finalize(std::move(frame), sync, true);
    }
  }

  if (retry_budget->redemodulation <= 0 || near_miss.failed_checks > max_aided_checks ||
      best_time < 0 || best_time + last_data_end > demodulation_limit ||
      best_time + Mode::payload_samples > received_limit || !optional_allowed())
    return quadratic_retry();
  --retry_budget->redemodulation;

  // Replay coarse extraction from the actual FFT center, including the
  // original frequency correction. Do not reuse a partially rotated buffer.
  extract_channel(acquisition_frequency);
  rotation = {1.0f, 0.0f};
  for (int i = 0; i < received_limit; ++i) {
    rotation *= rotation_step;
    channel_[i] *= rotation;
  }
  auto const aided = decoder::redemodulate_near_miss(
      std::span(channel_).first(received_limit), best_time, Mode::downsample_sps,
      down_rate, near_miss.bits, kCostas, control);
  if (aided.samples.empty()) return quadratic_retry();
  for (int symbol = 0; symbol < kChannelSymbols; ++symbol) {
    if (!optional_allowed()) return std::nullopt;
    std::copy_n(aided.samples.begin() + symbol * Mode::downsample_sps,
                Mode::downsample_sps, symbol_fft_.begin());
    transform_symbol();
    store_magnitudes(symbol);
  }
  hard_errors = decoder::belief_propagation(pass_two_llrs(received_limit), payload, codeword,
                                           60, true, nullptr, control);
  if (!accepts_codeword(2, aided.sync_power) || !optional_allowed()) return quadratic_retry();
  constexpr float channel_resolution = 12000.0f / Mode::ds_fft_span;
  float const aided_hz = std::round(acquisition_frequency / channel_resolution) * channel_resolution +
      best_delta + static_cast<float>(aided.frequency_hz);
  float const aided_dt = refined_offset + static_cast<float>(aided.timing_samples / down_rate);
  auto frame = make_frame();
  if (!suitable_last_try(frame, aided_hz, aided_dt)) return quadratic_retry();
  frequency = aided_hz;
  offset = aided_dt;
  return finalize(std::move(frame), aided.sync_power, false);
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
bool ChannelDecoder<Mode>::subtract_reference(
    std::vector<std::complex<float>> const& reference, float offset,
    std::array<int, kChannelSymbols> const& tones, decoder::TrackedTrajectory const& seed,
    int received_samples, int* adaptive_budget, DecodeControl const* control) {
  int const start = static_cast<int>(offset * 12000.0f);
  std::size_t const reference_start =
      start < 0 ? static_cast<std::size_t>(-start) : 0;
  std::size_t const signal_start = start > 0 ? static_cast<std::size_t>(start) : 0;

  if (reference_start >= reference.size() || signal_start >= samples_.size()) return false;

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

  auto const fixed_sample = [&](std::size_t i) {
    return samples_[signal_start + i] -
        2.0f * std::real(subtract_work_[i] * reference[reference_start + i]);
  };
  auto const received = std::span<float const>(samples_).first(received_samples);
  bool const complete = start >= 0 && signal_start + reference.size() <= received.size() &&
      seed.epoch_sample >= 0.0 && seed.epoch_sample + reference.size() <= received.size();
  if (complete && adaptive_budget && *adaptive_budget > 0 &&
      (!control || control->allow_optional())) {
    std::vector<float> fixed(count);
    for (std::size_t i = 0; i < count; ++i) fixed[i] = fixed_sample(i);
    if (!decoder::fixed_cancellation_sufficient(received.subspan(signal_start, count), fixed,
                                                 reference, Mode::nsps) &&
        (!control || control->allow_optional())) {
      --*adaptive_budget;
      auto const adaptive = decoder::propose_adaptive_cancellation(received, tones, seed, control);
      if (!adaptive.residual.empty()) {
        std::vector<float> baseline(received.begin(), received.end());
        std::copy(fixed.begin(), fixed.end(), baseline.begin() + signal_start);
        if (decoder::improves_cancellation(received, baseline, adaptive, control)) {
          std::copy(adaptive.residual.begin(), adaptive.residual.end(), samples_.begin());
          return true;
        }
      }
    }
    std::copy(fixed.begin(), fixed.end(), samples_.begin() + signal_start);
    return true;
  }
  for (std::size_t i = 0; i < count; ++i) samples_[signal_start + i] = fixed_sample(i);
  return true;
}

// ---------------------------------------------------------------------------
// Frame entry point
// ---------------------------------------------------------------------------

template <typename Mode>
std::size_t ChannelDecoder<Mode>::operator()(dec_data const& data, int position,
                                             int size, EventEmitter emit,
                                             std::size_t& tracked_cancellations,
                                             DecodeControl const& control, DeepSyncWindow* deferred) {
  if (control.is_canceled()) return 0;
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
  reported_.clear();
  int rescue_budget = 32;
  int adaptive_budget = 4;
  RetryBudget retry_budget{data.params.ldpc_rescue ? 4 : 0,
                           data.params.aided_redemodulation ? 2 : 0,
                           data.params.adaptive_whitening ? 4 : 0,
                           data.params.quadratic_phase ? 3 : 0};

  for (int pass = 1; pass <= 3; ++pass) {
    if (control.is_canceled()) break;
    auto candidates = find_candidates(data.params.nfa, data.params.nfb, &control);
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
    bool changed = false;

    for (auto candidate : candidates) {
      if (control.is_canceled()) break;
      float frequency = candidate.frequency;
      float offset = candidate.offset;
      int hard_errors = -1;
      float snr = 0.0f;

      auto result = attempt_decode(data.params.syncStats, subtract, frequency,
                                   offset, hard_errors, snr, rescue_budget, emit,
                                   SymbolTracking::Disabled, count, nullptr, &control, nullptr,
                                    data.params.coherent_likelihood,
                                    data.params.adaptive_sic ? &adaptive_budget : nullptr,
                                    &retry_budget);
      if (!result) continue;
      changed |= result->subtracted;

      int const rounded_snr = static_cast<int>(std::round(snr));
      auto [slot, inserted] = decoded.try_emplace(std::move(result->frame), rounded_snr);
      if (!inserted && slot->second >= rounded_snr) continue;

      if (!inserted) slot->second = rounded_snr;

      emit(events::Decoded{data.params.utc,
                           rounded_snr,
                           offset - Mode::start_seconds,
                           frequency,
                           slot->first.text,
                           slot->first.type,
                           1.0f - hard_errors / 60.0f,
                           Mode::event_mode});
      reported_.push_back({frequency, offset, slot->first});
    }

    // Another pass is only useful if the audio changed. A BP decode can
    // subtract even when it duplicates a frame previously reported by OSD.
    if (!changed) break;
  }

  // Run optional tracking only after every established SIC pass. Its separate
  // budgets cannot consume legacy rescue work or change legacy subtraction.
  if (data.params.symbol_tracking == SymbolTracking::Frequency ||
      data.params.symbol_tracking == SymbolTracking::Timing ||
      data.params.symbol_tracking == SymbolTracking::Both) {
    std::vector<std::pair<Frame, decoder::TrackedTrajectory>> pending;
    auto tracking_pass = [&](bool after_cancellation) {
      if (!control.allow_optional()) return;
      auto const candidates = find_candidates(data.params.nfa, data.params.nfb, &control,
                                              CandidateSearch::Tracking);
      if (!control.allow_optional()) return;
      prepare_baseband();  // SIC may have changed samples since the previous FFT.
      int tracking_budget = 8;
      int tracking_rescue_budget = 8;
      for (auto const& candidate : candidates) {
        if (!control.allow_optional()) return;
        // A whole-frame track cannot be established from a window that cannot
        // contain all symbols even after acquisition and tracking displacements.
        float const nominal_start = (candidate.offset + Mode::start_seconds) * kJs8RxSampleRate;
        int const margin = 8 * Mode::downsample_factor;
        if (nominal_start + Mode::nsps / 2.0f < margin ||
            nominal_start - Mode::nsps / 2.0f + kChannelSymbols * Mode::nsps + margin > count)
          continue;
        if (tracking_budget <= 0) break;
        // Candidate suppression can discard an adjacent half-tone FFT bin which
        // is a better center for a drifting frame. Revisit those two bins within
        // the same overall attempt budget, without relaxing pilot acceptance.
        for (float bin_shift : {0.0f, -0.5f, 0.5f}) {
          if (!control.allow_optional()) return;
          if (tracking_budget <= 0) break;
          float frequency = candidate.frequency + bin_shift * (12000.0f / Mode::nsps);
          if (frequency < 100.0f || frequency > 4910.0f) continue;
          --tracking_budget;
          float offset = candidate.offset, snr = 0.0f;
          int hard_errors = -1;
          std::optional<decoder::TrackedTrajectory> trajectory;
          auto result = attempt_decode(data.params.syncStats, false, frequency, offset,
                                       hard_errors, snr, tracking_rescue_budget, emit,
                                       data.params.symbol_tracking, count,
                                       data.params.tracked_sic && !after_cancellation ? &trajectory : nullptr,
                                       &control, nullptr, data.params.coherent_likelihood);
          if (!result) continue;
          if (decoded.contains(result->frame)) break;
          int const rounded_snr = static_cast<int>(std::round(snr));
          auto [slot, inserted] = decoded.try_emplace(std::move(result->frame), rounded_snr);
          if (!inserted) break;
          events::Decoded event{data.params.utc, rounded_snr, offset - Mode::start_seconds,
                                frequency, slot->first.text, slot->first.type,
                                1.0f - hard_errors / 60.0f, Mode::event_mode};
          event.tracking_used = true;
          event.after_tracked_sic = after_cancellation;
          emit(event);
          if (trajectory) pending.emplace_back(slot->first, *trajectory);
          break;
        }
      }
    };
    // Complete the original optional pass before modifying any residual, so
    // merely enabling tracked SIC cannot replace its existing results either.
    tracking_pass(false);
    if (data.params.tracked_sic && data.params.symbol_tracking == SymbolTracking::Both) {
      bool changed = false;
      int cancellation_budget = 2;
      for (auto const& [frame, trajectory] : pending) {
        if (!control.allow_optional()) break;
        if (cancellation_budget-- <= 0) break;
        std::array<int, kChannelSymbols> tones{};
        legacy_encode(frame.type, kCostas, frame.text.data(), tones.data());
        auto const reference = decoder::make_tracked_reference(tones, trajectory, count, &control);
        if (reference.empty()) continue;
        auto residual = decoder::cancel_tracked_reference(
            std::span<float const>(samples_.data(), static_cast<std::size_t>(count)), reference, Mode::nsps, &control);
        if (residual.residual.empty() || !control.allow_optional()) continue;
        std::copy(residual.residual.begin(), residual.residual.end(), samples_.begin());
        ++tracked_cancellations;
        changed = true;
      }
      // One bounded follow-up search; no recursive cancellation cascade.
      if (changed) tracking_pass(true);
    }
  }

  auto const decoded_count = decoded.size();
  if (deferred && !control.is_canceled()) *deferred = {count, std::move(decoded)};
  return decoded_count;
}

template <typename Mode>
std::size_t ChannelDecoder<Mode>::deep_sync(dec_data const& data, DeepSyncWindow& window,
                                            EventEmitter emit, DecodeControl const& control) {
  int const available = std::min(Mode::ds_fft_out, window.count / Mode::downsample_factor);
  // Two complete Costas blocks need at least 36 + 7 symbols of observed time.
  if (available < 43 * Mode::downsample_sps || !control.allow_optional()) return 0;
  int const low = std::max(data.params.nfa, 100);
  int const high = std::min(data.params.nfb, 4910);
  if (low >= high) return 0;

  auto const seeds = find_candidates(low, high, &control, CandidateSearch::Deep);
  if (seeds.empty() || !control.allow_optional()) return 0;
  auto const reference = decoder::CoherentCostasReference::make(Mode::downsample_sps, kCostas);
  if (!reference) return 0;
  // Rebuild from the final residual after original and tracked cancellation.
  prepare_baseband();

  constexpr float resolution = 12000.0f / Mode::ds_fft_span;
  constexpr float down_rate = 12000.0f / Mode::downsample_factor;
  constexpr float tone_hz = down_rate / Mode::downsample_sps;
  struct Hypothesis {
    float seed_frequency;
    double frequency;
    decoder::CoherentCostasPeak peak;
  };
  std::vector<Hypothesis> peaks;
  peaks.reserve(seeds.size());
  for (auto const& seed : seeds) {
    if (!control.allow_optional()) return 0;
    extract_channel(seed.frequency);
    double const center = std::round(seed.frequency / resolution) * resolution;
    int const start = static_cast<int>(std::round((seed.offset + Mode::start_seconds) * down_rate));
    auto const peak = reference->search(std::span(channel_).first(available), start, down_rate,
                                        low - center, high - center, control);
    if (peak && peak->support >= kDeepSyncSupport)
      peaks.push_back({seed.frequency, center + peak->frequency_offset_hz, *peak});
  }
  std::stable_sort(peaks.begin(), peaks.end(), [](auto const& a, auto const& b) {
    return a.peak.support > b.peak.support;
  });

  std::array<Hypothesis const*, kDeepSyncDecodeLimit> attempted{};
  std::size_t attempts = 0, added = 0;
  for (auto const& hypothesis : peaks) {
    if (attempts == attempted.size() || !control.allow_optional()) break;
    auto const prior = std::span(attempted).first(attempts);
    if (std::any_of(prior.begin(), prior.end(), [&](auto const* other) {
          return std::abs(hypothesis.frequency - other->frequency) < tone_hz / 4.0 &&
              std::abs(hypothesis.peak.frame_start - other->peak.frame_start) <= Mode::quarter_symbol;
        })) continue;
    attempted[attempts++] = &hypothesis;

    float frequency = hypothesis.seed_frequency;
    float offset = 0.0f, snr = 0.0f;
    int hard_errors = -1, no_rescue = 0;
    auto frame = attempt_decode(false, false, frequency, offset, hard_errors, snr, no_rescue,
                                 emit, SymbolTracking::Disabled, window.count, nullptr, &control,
                                 &hypothesis.peak, data.params.coherent_likelihood);
    if (!frame) continue;
    if (!control.allow_optional()) break;
    int const rounded_snr = static_cast<int>(std::round(snr));
    auto [slot, inserted] = window.decoded.try_emplace(std::move(frame->frame), rounded_snr);
    if (!inserted) continue;
    ++added;
    emit(events::Decoded{data.params.utc, rounded_snr, offset - Mode::start_seconds, frequency,
                         slot->first.text, slot->first.type, 1.0f - hard_errors / 60.0f,
                         Mode::event_mode});
  }
  return added;
}

}  // namespace

std::size_t legacy_decode(DecodeState const& state,
                           std::function<void(events::Variant const&)> emit_fn) {
  return decode_with_control(state, std::move(emit_fn), DecodeControl{});
}

std::size_t decode_with_control(DecodeState const& state,
                          std::function<void(events::Variant const&)> emit_fn,
                          DecodeControl const& requested) {
  static thread_local std::unique_ptr<ChannelDecoder<ProfileA>> decoder_a;
  static thread_local std::unique_ptr<ChannelDecoder<ProfileB>> decoder_b;
  static thread_local std::unique_ptr<ChannelDecoder<ProfileC>> decoder_c;
  static thread_local std::unique_ptr<ChannelDecoder<ProfileE>> decoder_e;
  static thread_local std::unique_ptr<ChannelDecoder<ProfileI>> decoder_i;

  auto emit = [&](events::Variant const& event) {
    if (emit_fn) emit_fn(event);
  };

  std::size_t tracked_cancellations = 0;
  bool canceled = false, skipped = false;
  DecodeControl control;
  if (requested.cancel_requested || requested.optional_allowed) {
    control.cancel_requested = [&] { canceled = canceled || requested.is_canceled(); return canceled; };
    control.optional_allowed = [&] {
      if (!skipped) {
        try { skipped = requested.optional_allowed && !requested.optional_allowed(); }
        catch (...) { skipped = true; }
      }
      return !skipped;
    };
  }

  auto run = [&](auto& decoder, int position, int size, DeepSyncWindow& deferred) {
    using Decoder =
        typename std::remove_reference_t<decltype(decoder)>::element_type;
    if (!decoder) decoder = std::make_unique<Decoder>();
    return (*decoder)(state, position, size, emit, tracked_cancellations, control,
                      state.params.deep_sync ? &deferred : nullptr);
  };

  int const submodes = state.params.nsubmodes;
  std::size_t total = 0;
  DeepSyncWindow deep_a, deep_b, deep_c, deep_e, deep_i;

  emit(events::DecodeStarted{submodes});

  if ((submodes & (1 << ProfileI::bit_index)) != 0) {
    total += run(decoder_i, state.params.kposI, state.params.kszI, deep_i);
  }
  if ((submodes & (1 << ProfileE::bit_index)) != 0) {
    total += run(decoder_e, state.params.kposE, state.params.kszE, deep_e);
  }
  if ((submodes & (1 << ProfileC::bit_index)) != 0) {
    total += run(decoder_c, state.params.kposC, state.params.kszC, deep_c);
  }
  if ((submodes & (1 << ProfileB::bit_index)) != 0) {
    total += run(decoder_b, state.params.kposB, state.params.kszB, deep_b);
  }
  if ((submodes & (1 << ProfileA::bit_index)) != 0) {
    total += run(decoder_a, state.params.kposA, state.params.kszA, deep_a);
  }

  // Optional deep acquisition gets only the time left after every established
  // decode path, including tracking in later submodes.
  if (state.params.deep_sync) {
    auto finish = [&](auto& decoder, DeepSyncWindow& window) {
      if (window.count > 0) total += decoder->deep_sync(state, window, emit, control);
    };
    finish(decoder_i, deep_i);
    finish(decoder_e, deep_e);
    finish(decoder_c, deep_c);
    finish(decoder_b, deep_b);
    finish(decoder_a, deep_a);
  }

  control.is_canceled();
  emit(events::DecodeFinished{total, tracked_cancellations, skipped, canceled});
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

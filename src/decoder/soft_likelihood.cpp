#include "soft_likelihood.hpp"

#include "js8core/compat/numbers.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace js8core::decoder {
namespace {

constexpr std::size_t kMinPilots = 14; // At least two blocks' worth of symbols.
constexpr std::size_t kMaxPilots = data::kSyncSymbols;
// Keep the bit-wise inputs below the LDPC check-message saturation regime.
constexpr double kMaxLlr = 12.0;

double energy(float magnitude) {
  if (!std::isfinite(magnitude) || magnitude < 0.0f)
    return std::numeric_limits<double>::quiet_NaN();
  return static_cast<double>(magnitude) * magnitude;
}

double median(std::span<double> values) {
  auto middle = values.begin() + values.size() / 2;
  std::nth_element(values.begin(), middle, values.end());
  if (values.size() % 2 != 0) return *middle;
  return 0.5 * (*middle + *std::max_element(values.begin(), middle));
}

}  // namespace

std::optional<SoftToneModel> estimate_soft_tone_model(std::span<PilotTone const> pilots) {
  if (pilots.size() < kMinPilots || pilots.size() > kMaxPilots) return std::nullopt;
  std::array<double, kMaxPilots> active{};
  std::array<double, kMaxPilots * 7> inactive{};
  std::size_t on = 0, off = 0;
  for (auto const& pilot : pilots) {
    if (pilot.active_tone >= pilot.magnitudes.size()) return std::nullopt;
    for (std::size_t tone = 0; tone < pilot.magnitudes.size(); ++tone) {
      double const e = energy(pilot.magnitudes[tone]);
      if (!std::isfinite(e)) return std::nullopt;
      if (tone == pilot.active_tone) active[on++] = e;
      else inactive[off++] = e;
    }
  }

  // Squared magnitude of a zero-mean circular complex Gaussian is exponential:
  // median = mean * ln(2). Use known inactive bins, not the weakest observed
  // bins, so the noise estimate does not select downward fluctuations.
  double const noise = median(std::span(inactive).first(off)) / compat::numbers::ln2;
  double const total = median(std::span(active).first(on)) / compat::numbers::ln2;
  if (!(noise > 0.0 && total > noise)) return std::nullopt;
  return SoftToneModel{noise, (total - noise) / total};
}

std::optional<SymbolLlrs> soft_symbol_llrs(ToneMagnitudes const& magnitudes,
                                         SoftToneModel const& model) {
  if (!std::isfinite(model.noise_energy) || model.noise_energy <= 0.0 ||
      !(model.signal_fraction >= 0.0 && model.signal_fraction <= 1.0))
    return std::nullopt;

  // Under each tone hypothesis, that bin has variance N+S and the other bins
  // have variance N. Subtracting the common noise-only log likelihood leaves
  // |Z[k]|² * (1/N - 1/(N+S)); the determinant penalty is also common.
  std::array<double, 8> scores{};
  for (std::size_t tone = 0; tone < scores.size(); ++tone) {
    scores[tone] = (energy(magnitudes[tone]) * model.signal_fraction) / model.noise_energy;
    if (!std::isfinite(scores[tone])) return std::nullopt;
  }

  SymbolLlrs llrs{};
  for (std::size_t bit = 0; bit < llrs.size(); ++bit) {
    auto const group = [mask = 4u >> bit](std::size_t tone) { return (tone & mask) != 0; };
    std::array<double, 2> peak{};
    for (std::size_t tone = 0; tone < scores.size(); ++tone)
      peak[group(tone)] = std::max(peak[group(tone)], scores[tone]);
    std::array<double, 2> sum{};
    for (std::size_t tone = 0; tone < scores.size(); ++tone)
      sum[group(tone)] += std::exp(scores[tone] - peak[group(tone)]);
    // Take the peak difference separately so very large equal peaks don't
    // round away the effect of the other tones.
    double const llr = (peak[1] - peak[0]) + (std::log(sum[1]) - std::log(sum[0]));
    llrs[bit] = static_cast<float>(std::clamp(llr, -kMaxLlr, kMaxLlr));
  }
  return llrs;
}

std::optional<FrameLlrs> soft_frame_llrs(FrameToneMagnitudes const& magnitudes,
                                       protocol::CostasArray const& costas,
                                       int frame_start, int samples_per_symbol, int available_samples) {
  if (samples_per_symbol <= 0 || available_samples <= 0) return std::nullopt;
  constexpr auto kBlockSize = protocol::CostasArray::value_type{}.size();
  constexpr auto kBlockStride = kBlockSize + data::kDataSymbols / 2;
  auto complete = [&](std::size_t symbol) {
    auto const start = static_cast<std::int64_t>(frame_start) +
        static_cast<std::int64_t>(symbol) * samples_per_symbol;
    return start >= 0 && start + samples_per_symbol <= available_samples;
  };
  auto read_tones = [&](std::size_t symbol) {
    ToneMagnitudes tones{};
    for (std::size_t tone = 0; tone < tones.size(); ++tone)
      tones[tone] = magnitudes[tone][symbol];
    return tones;
  };

  std::array<PilotTone, kMaxPilots> pilots{};
  std::size_t pilot_count = 0;
  for (std::size_t block = 0; block < costas.size(); ++block) {
    for (std::size_t column = 0; column < kBlockSize; ++column) {
      std::size_t const symbol = block * kBlockStride + column;
      if (complete(symbol))
        pilots[pilot_count++] = {read_tones(symbol), static_cast<std::size_t>(costas[block][column])};
    }
  }
  auto const model = estimate_soft_tone_model(std::span(pilots).first(pilot_count));
  if (!model) return std::nullopt;

  FrameLlrs llrs{};
  std::size_t bit = 0;
  for (std::size_t symbol = 0; symbol < data::kChannelSymbols; ++symbol) {
    if (symbol % kBlockStride < kBlockSize) continue;
    if (complete(symbol)) {
      auto const result = soft_symbol_llrs(read_tones(symbol), *model);
      if (!result) return std::nullopt;
      std::copy(result->begin(), result->end(), llrs.begin() + bit);
    }
    bit += SymbolLlrs{}.size();
  }
  return llrs;
}

}  // namespace js8core::decoder

#include "soft_likelihoods.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace js8core::decoder {
namespace {
constexpr double kPi = 3.14159265358979323846;
bool allowed(DecodeControl const* c) { return !c || c->allow_optional(); }
bool finite(std::complex<float> z) { return std::isfinite(z.real()) && std::isfinite(z.imag()); }
template <typename Samples>
double median(Samples values) {
  auto middle = values.begin() + values.size() / 2;
  std::nth_element(values.begin(), middle, values.end());
  return *middle;
}
bool valid(DisturbanceModel const& model) {
  return std::isfinite(model.amplitude) && model.amplitude > 0.0 &&
      std::all_of(model.variance.begin(), model.variance.end(), [](double v) {
        return std::isfinite(v) && v > std::numeric_limits<double>::min();
      });
}
double log_i0(double x) {
  // Positive series below 20; scaled asymptotic expansion avoids overflow above.
  if (x < 20.0) {
    double term = 1.0, sum = 1.0;
    for (int n = 1; n < 64; ++n) {
      term *= (x * x / 4.0) / (n * n);
      sum += term;
      if (term < sum * 1e-15) break;
    }
    return std::log(sum);
  }
  double const inv = 1.0 / x;
  return x - 0.5 * std::log(2.0 * kPi * x) +
      std::log(1.0 + inv / 8.0 + 9.0 * inv * inv / 128.0 +
               225.0 * inv * inv * inv / 3072.0);
}
double log_sum(std::array<double, 8> const& scores, int mask, bool set) {
  double largest = -std::numeric_limits<double>::infinity();
  for (int k = 0; k < 8; ++k) if (((k & mask) != 0) == set) largest = std::max(largest, scores[k]);
  double sum = 0.0;
  for (int k = 0; k < 8; ++k) if (((k & mask) != 0) == set) sum += std::exp(scores[k] - largest);
  return largest + std::log(sum);
}

double phase(PilotChannel const& channel, double time, double tone) {
  double const normalized = time / 39.0;
  return channel.radians_per_symbol * time + channel.radians_per_tone * tone +
      channel.edge_phase_radians * normalized * normalized;
}

struct PilotPoint { int s, k; std::complex<double> z; double w; };

std::optional<double> prediction_error(PilotChannel const& channel,
    std::span<PilotPoint const> held, double block_limit) {
  double residual = 0.0, observed = 0.0;
  std::array<double, 3> block_error{}, block_energy{};
  for (auto const& p : held) {
    auto const mean = channel.gain * std::polar(1.0, phase(channel, p.s - 39, p.k - 3.5));
    double const e = p.w * std::norm(p.z - mean), power = p.w * std::norm(p.z);
    residual += e;
    observed += power;
    block_error[p.s / 36] += e;
    block_energy[p.s / 36] += power;
  }
  if (!std::isfinite(observed) || !std::isfinite(residual) || observed <= 0.0) return {};
  for (int b = 0; b < 3; ++b)
    if (block_energy[b] <= 0.0 || block_error[b] > block_limit * block_energy[b]) return {};
  return residual / observed;
}

void append_symbol(BitLikelihoods& result, int data, std::array<double, 8> const& scores) {
  for (int bit = 0; bit < 3; ++bit) {
    int const mask = 1 << (2 - bit);
    result[3 * data + bit] = static_cast<float>(std::clamp(
        log_sum(scores, mask, true) - log_sum(scores, mask, false), -20.0, 20.0));
  }
}
} // namespace

std::optional<DisturbanceModel> estimate_disturbance(
    ToneObservations const& observations, protocol::CostasArray const& costas,
    bool whiten, DecodeControl const* control) {
  if (!allowed(control)) return {};
  DisturbanceModel model;
  for (int k = 0; k < 8; ++k) {
    std::vector<double> powers;
    for (int s = 0; s < 79; ++s) {
      auto const z = observations[s][k];
      if (!finite(z)) return {};
      // Only known pilot tones can be excluded safely. Removing the largest
      // data bin would systematically hide a persistent CW interferer.
      if (s % 36 < 7 && costas[s / 36][s % 36] == k) continue;
      powers.push_back(std::norm(static_cast<std::complex<double>>(z)));
    }
    model.variance[k] = median(std::move(powers)) / std::log(2.0);
  }
  double const pooled = median(std::vector<double>(model.variance.begin(), model.variance.end()));
  if (!std::isfinite(pooled) || pooled <= std::numeric_limits<double>::min()) return {};
  for (auto& v : model.variance)
    v = whiten ? std::clamp(0.85 * v + 0.15 * pooled, 0.25 * pooled, 64.0 * pooled) : pooled;
  std::vector<double> signal;
  for (int b = 0; b < 3; ++b) for (int j = 0; j < 7; ++j) {
    int const k = costas[b][j];
    signal.push_back(std::max(0.0, std::norm(static_cast<std::complex<double>>(
        observations[36 * b + j][k])) - model.variance[k]));
  }
  model.amplitude = std::sqrt(median(std::move(signal)));
  if (!valid(model) || !allowed(control)) return {};
  return model;
}

std::optional<PilotChannel> estimate_pilot_channel(
    ToneObservations const& observations, protocol::CostasArray const& costas,
    DisturbanceModel const& noise, DecodeControl const* control) {
  if (!valid(noise) || !allowed(control)) return {};
  // Each point uses only known pilot tones. Unknown data never train the model.
  std::vector<PilotPoint> train, held;
  for (int b = 0; b < 3; ++b) for (int j = 0; j < 7; ++j) {
    int const s = b * 36 + j, k = costas[b][j];
    if (!finite(observations[s][k])) return {};
    (j % 2 ? held : train).push_back({s, k, observations[s][k], 1.0 / noise.variance[k]});
  }
  double energy = 0.0, weight = 0.0;
  for (auto const& p : train) { energy += p.w * std::norm(p.z); weight += p.w; }
  if (!std::isfinite(energy) || !std::isfinite(weight) || energy <= 0.0 || weight <= 0.0) return {};
  auto fit = [&](double omega, double beta) {
    std::complex<double> sum{};
    for (auto const& p : train)
      sum += p.w * p.z * std::polar(1.0, -omega * (p.s - 39) - beta * (p.k - 3.5));
    return sum / weight;
  };
  double best = -1.0, omega = 0.0, beta = 0.0;
  // Bounded two-dimensional search also handles the tone phase introduced by
  // a fractional start and the transmitter's sample-before/after phase origin.
  for (int f = -16; f <= 16; ++f) {
    if (!allowed(control)) return {};
    for (int t = -16; t <= 16; ++t) {
      double const w = f * 0.005, v = t * 0.1;
      double const score = std::norm(fit(w, v));
      if (score > best) { best = score; omega = w; beta = v; }
    }
  }
  double const coarse_omega = omega, coarse_beta = beta;
  for (int f = -5; f <= 5; ++f) {
    if (!allowed(control)) return {};
    for (int t = -5; t <= 5; ++t) {
      double const w = coarse_omega + f * 0.0005, v = coarse_beta + t * 0.01;
      double const score = std::norm(fit(w, v));
      if (score > best) { best = score; omega = w; beta = v; }
    }
  }
  if (std::abs(omega) >= 0.078 || std::abs(beta) >= 1.58 || best * weight < 0.5 * energy) return {};
  // Reject an alternative time-phase lobe rather than choose an ambiguous wrap.
  for (int f = -16; f <= 16; ++f) {
    if (!allowed(control)) return {};
    if (std::abs(f * 0.005 - omega) < 0.04) continue;
    for (int t = -16; t <= 16; ++t)
      if (std::norm(fit(f * 0.005, t * 0.1)) > 0.85 * best) return {};
  }
  PilotChannel result{fit(omega, beta), omega, beta, 0.0};
  auto const error = prediction_error(result, held, 0.65);
  if (!error || *error > 0.45) return {};
  result.validation_error = *error;
  if (!allowed(control)) return {};
  return result;
}

std::optional<PilotChannel> estimate_quadratic_channel(
    ToneObservations const& observations, protocol::CostasArray const& costas,
    DisturbanceModel const& noise, DecodeControl const* control) {
  if (!valid(noise) || !allowed(control)) return {};
  std::array<PilotPoint, 12> train{};
  std::array<PilotPoint, 9> held{};
  double weight = 0.0, energy = 0.0;
  for (int b = 0; b < 3; ++b) {
    for (int j = 0; j < 7; ++j) {
      int const s = 36 * b + j, k = costas[b][j];
      if (k < 0 || k >= 8 || !finite(observations[s][k])) return {};
      PilotPoint const point{s, k, observations[s][k], 1.0 / noise.variance[k]};
      if (j % 2) {
        held[3 * b + j / 2] = point;
      } else {
        train[4 * b + j / 2] = point;
        weight += point.w;
        energy += point.w * std::norm(point.z);
      }
    }
  }
  if (!(energy > 0.0 && weight > 0.0) || !std::isfinite(energy) || !std::isfinite(weight)) return {};

  // Grid dimensions cover residual rate, fractional timing, and phase
  // curvature at either end. Cache the phasors for the inner search.
  constexpr int rate_steps = 12, timing_steps = 8, drift_steps = 8;
  constexpr double rate_spacing = 0.01, timing_spacing = 0.2, drift_spacing = 0.4;
  using Rotation = std::array<std::complex<double>, 12>;
  std::array<Rotation, 2 * rate_steps + 1> rate_rotation{};
  std::array<Rotation, 2 * timing_steps + 1> timing_rotation{};
  for (std::size_t i = 0; i < train.size(); ++i) {
    for (int r = -rate_steps; r <= rate_steps; ++r)
      rate_rotation[r + rate_steps][i] = std::polar(1.0, -r * rate_spacing * (train[i].s - 39));
    for (int t = -timing_steps; t <= timing_steps; ++t)
      timing_rotation[t + timing_steps][i] = std::polar(1.0, -t * timing_spacing * (train[i].k - 3.5));
  }

  struct Fit {
    PilotChannel channel;
    double power = 0.0;
  };
  Fit best, affine;
  std::array<std::array<std::array<double, 2 * timing_steps + 1>, 2 * rate_steps + 1>,
             2 * drift_steps + 1> grid{};
  for (int d = -drift_steps; d <= drift_steps; ++d) {
    if (!allowed(control)) return {};
    double const curvature = d * drift_spacing;
    Rotation weighted{};
    for (std::size_t i = 0; i < train.size(); ++i) {
      double const x = (train[i].s - 39) / 39.0;
      weighted[i] = train[i].w * train[i].z * std::polar(1.0, -curvature * x * x) / weight;
    }
    for (int r = -rate_steps; r <= rate_steps; ++r) {
      Rotation combined{};
      for (std::size_t i = 0; i < train.size(); ++i)
        combined[i] = weighted[i] * rate_rotation[r + rate_steps][i];
      for (int t = -timing_steps; t <= timing_steps; ++t) {
        std::complex<double> gain{};
        for (std::size_t i = 0; i < train.size(); ++i)
          gain += combined[i] * timing_rotation[t + timing_steps][i];
        double const power = std::norm(gain);
        grid[d + drift_steps][r + rate_steps][t + timing_steps] = power;
        if (power > best.power)
          best = {{gain, r * rate_spacing, t * timing_spacing, 0.0, curvature}, power};
        if (d == 0 && power > affine.power)
          affine = {{gain, r * rate_spacing, t * timing_spacing, 0.0}, power};
      }
    }
  }

  auto refine = [&](Fit& fit, bool drift) {
    for (double scale : {0.5, 0.25, 0.125, 0.0625}) {
      if (!allowed(control)) return;
      auto const center = fit.channel;
      for (int d = -static_cast<int>(drift); d <= static_cast<int>(drift); ++d) {
        for (int r : {-1, 0, 1}) {
          for (int t : {-1, 0, 1}) {
            auto trial = center;
            trial.radians_per_symbol += scale * r * rate_spacing;
            trial.radians_per_tone += scale * t * timing_spacing;
            trial.edge_phase_radians += scale * d * drift_spacing;
            trial.gain = {};
            for (auto const& p : train)
              trial.gain += p.w * p.z *
                  std::polar(1.0, -phase(trial, p.s - 39, p.k - 3.5));
            trial.gain /= weight;
            double const power = std::norm(trial.gain);
            if (power > fit.power) fit = {trial, power};
          }
        }
      }
    }
  };
  refine(best, true);
  auto& result = best.channel;
  // The derivative is linear: its largest magnitude is at a frame end.
  if (!allowed(control) || std::abs(result.edge_phase_radians) < 0.35 ||
      std::abs(result.edge_phase_radians) >= 3.15 || std::abs(result.radians_per_tone) >= 1.58 ||
      std::abs(result.radians_per_symbol) >= 0.115 ||
      std::abs(result.radians_per_symbol) + 2 * std::abs(result.edge_phase_radians) / 39.0 >= 0.22 ||
      best.power * weight < 0.6 * energy)
    return {};

  // A competing separated lobe makes the predicted data phases ambiguous.
  for (int d = -drift_steps; d <= drift_steps; ++d) {
    if (!allowed(control)) return {};
    for (int r = -rate_steps; r <= rate_steps; ++r) {
      auto const& timings = grid[d + drift_steps][r + rate_steps];
      bool const separated_carrier = std::abs(d * drift_spacing - result.edge_phase_radians) >= 1.8 ||
          std::abs(r * rate_spacing - result.radians_per_symbol) >= 0.055;
      for (int t = -timing_steps; t <= timing_steps; ++t) {
        bool const separated_timing = std::abs(t * timing_spacing - result.radians_per_tone) >= 0.9;
        if ((separated_carrier || separated_timing) && timings[t + timing_steps] > 0.94 * best.power)
          return {};
      }
    }
  }

  refine(affine, false);
  auto const error = prediction_error(result, held, 0.45);
  auto const affine_error = prediction_error(affine.channel, held, std::numeric_limits<double>::infinity());
  if (!allowed(control) || !error || !affine_error ||
      *error > 0.28 || *affine_error - *error < 0.04 || *error > 0.85 * *affine_error ||
      best.power < 1.025 * affine.power)
    return {};
  result.validation_error = *error;
  return result;
}

std::optional<BitLikelihoods> make_soft_likelihoods(
    ToneObservations const& observations, DisturbanceModel const& noise,
    PilotChannel const* channel, DecodeControl const* control) {
  if (!valid(noise)) return {};
  for (auto const& symbol : observations)
    for (auto const z : symbol) if (!finite(z)) return {};
  if (channel && (!std::isfinite(channel->gain.real()) || !std::isfinite(channel->gain.imag()) ||
      !std::isfinite(channel->radians_per_symbol) || !std::isfinite(channel->radians_per_tone) ||
      !std::isfinite(channel->edge_phase_radians) ||
      !std::isfinite(channel->validation_error) || channel->validation_error < 0.0 ||
      channel->validation_error > 0.45)) return {};
  BitLikelihoods result{};
  int data = 0;
  for (int s = 0; s < 79; ++s) {
    if (!allowed(control)) return {};
    if (s % 36 < 7) continue;
    std::array<double, 8> scores{};
    for (int k = 0; k < 8; ++k) {
      if (!finite(observations[s][k])) return {};
      auto const z = static_cast<std::complex<double>>(observations[s][k]);
      double const variance = noise.variance[k];
      if (channel) {
        auto const mean = channel->gain * std::polar(1.0, phase(*channel, s - 39, k - 3.5));
        // Inflate uncertainty by held-out prediction error, not a hard phase
        // decision. Common background terms cancel between tone hypotheses.
        scores[k] = (2.0 * std::real(z * std::conj(mean)) - std::norm(mean)) /
            (variance + channel->validation_error * std::norm(mean));
      } else {
        double const a = noise.amplitude;
        scores[k] = log_i0(2.0 * a * std::abs(z) / variance) - a * a / variance;
      }
      if (!std::isfinite(scores[k])) return {};
    }
    append_symbol(result, data, scores);
    ++data;
  }
  if (!allowed(control)) return {};
  return result;
}

std::optional<BitLikelihoods> make_adaptive_likelihoods(
    ToneObservations const& observations, protocol::CostasArray const& costas,
    DisturbanceModel const& noise, DecodeControl const* control) {
  if (!valid(noise) || !allowed(control)) return {};

  // Normalize by the frame-wide variance before looking for power spread
  // across tones. In data symbols, discard the largest normalized bin for
  // this time estimate only; the frame-wide estimate still sees CW energy.
  std::array<double, 79> local{};
  for (int s = 0; s < 79; ++s) {
    if (!allowed(control)) return {};
    std::array<double, 8> ratios{};
    int const pilot = s % 36 < 7 ? costas[s / 36][s % 36] : -1;
    int used = 0;
    for (int k = 0; k < 8; ++k) {
      if (!finite(observations[s][k])) return {};
      if (k != pilot)
        ratios[used++] = std::norm(static_cast<std::complex<double>>(observations[s][k])) /
            noise.variance[k];
    }
    // The fourth order statistic discards the largest data bin implicitly.
    // Debias it for unit exponential power: E[X_(4)] = sum of 1/i.
    constexpr double pilot_median = 1.0 / 7 + 1.0 / 6 + 1.0 / 5 + 1.0 / 4;
    constexpr double data_median = 1.0 / 8 + 1.0 / 7 + 1.0 / 6 + 1.0 / 5;
    std::nth_element(ratios.begin(), ratios.begin() + 3, ratios.begin() + used);
    local[s] = ratios[3] / (pilot >= 0 ? pilot_median : data_median);
  }
  auto const reference = median(local);
  if (!std::isfinite(reference) || reference <= 0.0) return {};

  auto const [min_tone, max_tone] = std::minmax_element(noise.variance.begin(), noise.variance.end());
  bool has_burst = false;
  for (int s = 0; s < 79; ++s) {
    double const excess = std::max(0.0, local[s] / reference - 2.0);
    local[s] = 1.0 + std::min(31.0, 0.75 * excess);
    has_burst |= s % 36 >= 7 && local[s] >= 2.0;
  }
  if (!has_burst && *max_tone < 2.5 * *min_tone) return {};

  // Under hypothesis k, only bin k gains signal variance S. Its likelihood
  // relative to the all-noise hypothesis includes the normalizing determinant
  // of that bin, which no longer cancels when the tone variances differ.
  double const signal = noise.amplitude * noise.amplitude;
  BitLikelihoods result{};
  int data = 0;
  for (int s = 0; s < 79; ++s) {
    if (s % 36 < 7) continue;
    if (!allowed(control)) return {};
    std::array<double, 8> scores{};
    for (int k = 0; k < 8; ++k) {
      double const variance = noise.variance[k] * local[s];
      double const power = std::norm(static_cast<std::complex<double>>(observations[s][k]));
      scores[k] = (power / variance) * (signal / (signal + variance)) -
          std::log1p(signal / variance);
      if (!std::isfinite(scores[k])) return {};
    }
    append_symbol(result, data++, scores);
  }
  if (!allowed(control)) return {};
  return result;
}
} // namespace js8core::decoder

#include "tracking_waveform.hpp"

#include "js8core/decoder.hpp"
#include "src/decoder/coherent_costas.hpp"
#include "src/decoder/decoder_tables.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <random>
#include <utility>
#include <vector>

namespace {

using namespace js8core;
using Samples = std::vector<std::complex<float>>;
constexpr double kTau = 6.283185307179586476925286766559;
static_assert(DecodeParams{}.deep_sync && EngineConfig{}.deep_sync);

Samples synthesize(protocol::CostasArray const& costas, int sps, int start, int count,
                   double rate_hz, double offset_hz, std::array<double, 3> const& gains,
                   std::array<double, 3> const& phases) {
  Samples samples(count);
  for (std::size_t block = 0; block < costas.size(); ++block) {
    int cycles = 0;
    for (std::size_t column = 0; column < costas[block].size(); ++column) {
      int const tone = costas[block][column];
      int const symbol = 36 * static_cast<int>(block) + static_cast<int>(column);
      for (int n = 0; n < sps; ++n) {
        int const time = start + symbol * sps + n;
        if (time < 0 || time >= count) continue;
        double const phase = phases[block] + kTau *
            (cycles + tone * (static_cast<double>(n) / sps) + offset_hz * time / rate_hz);
        samples[time] = {static_cast<float>(gains[block] * std::cos(phase)),
                         static_cast<float>(gains[block] * std::sin(phase))};
      }
      cycles += tone;
    }
  }
  return samples;
}

void add_noise(Samples& samples, float amplitude) {
  std::mt19937 random(1732);
  auto uniform = [&] {
    return (static_cast<int>(random() >> 16) - 32768) * (amplitude / 32768.0f);
  };
  for (auto& sample : samples) sample += std::complex<float>{uniform(), uniform()};
}

struct DecodeRun {
  std::vector<events::Decoded> frames;
  events::DecodeFinished summary;
};

DecodeRun decode(DecodeState const& state, DecodeControl const& control = {}) {
  DecodeRun result;
  decode_with_control(state, [&](events::Variant const& event) {
    if (auto const* frame = std::get_if<events::Decoded>(&event)) result.frames.push_back(*frame);
    if (auto const* summary = std::get_if<events::DecodeFinished>(&event)) result.summary = *summary;
  }, control);
  return result;
}

DecodeState turbo_window(std::vector<std::int16_t> samples, int low, int high) {
  DecodeState state;
  state.params.kszI = static_cast<int>(samples.size());
  state.samples = std::move(samples);
  state.samples.resize(720000);
  state.params.nfa = low;
  state.params.nfb = high;
  state.params.nfqso = 1500;
  state.params.nsubmodes = 1 << 4;
  state.params.symbol_tracking = SymbolTracking::Disabled;
  return state;
}

DecodeState late_turbo_window() {
  test::TrackingSignal late;
  late.symbol_seconds = 384.0 / 12000;
  late.start_seconds = 1.23;
  late.base_hz = 1506.0;
  late.amplitude = 4500.0;
  auto const& costas = protocol::costas(protocol::CostasType::Modified);
  legacy_encode(4, costas, "Ux2zqX++++++", late.tones.data());

  auto early = late;
  early.start_seconds = 0.112;
  early.base_hz = 1860.0;
  legacy_encode(2, costas, "HELLO-WORLD-", early.tones.data());

  auto samples = test::render_tracking_audio(
      std::array{test::TrackingWaveform(late), test::TrackingWaveform(early)}, 12000, 48000);
  std::mt19937 noise(61);
  for (auto& sample : samples)
    sample = static_cast<std::int16_t>(sample + (static_cast<int>(noise() >> 16) - 32768) / 7);
  return turbo_window(std::move(samples), 1400, 1900);
}

DecodeState weak_pilot_window() {
  constexpr int start = 1350, sps = 384;
  test::TrackingSignal signal;
  signal.symbol_seconds = sps / 12000.0;
  signal.start_seconds = start / 12000.0;
  signal.base_hz = 1506.0;
  signal.amplitude = 4500.0;
  legacy_encode(4, protocol::costas(protocol::CostasType::Modified),
                "Ux2zqX++++++", signal.tones.data());
  auto samples = test::render_tracking_audio(
      std::array{test::TrackingWaveform(signal)}, 12000, 48000);
  std::mt19937 noise(45);
  for (int i = 0; i < static_cast<int>(samples.size()); ++i) {
    // Fade just the pilots into the noise: legacy coarse acquisition misses
    // the carrier, although the data itself is strong after coherent sync.
    int const symbol = i >= start ? (i - start) / sps : -1;
    int const value = symbol >= 0 && symbol < 79 && symbol % 36 < 7
        ? samples[i] * 1050 / 4500 : samples[i];
    samples[i] = static_cast<std::int16_t>(value + (static_cast<int>(noise() >> 16) - 32768) / 5);
  }
  return turbo_window(std::move(samples), 1410, 1590);
}

}  // namespace

int main() {
  using namespace js8core::decoder;
  int failures = 0, checks = 0;
  auto check = [&](bool ok, char const* name) {
    ++checks;
    if (!ok) {
      std::fprintf(stderr, "FAIL coherent Costas: %s\n", name);
      ++failures;
    }
  };

  constexpr std::array phases{0.4, 3.2, -1.4};
  constexpr std::array gains{1.0, 0.22, 2.0};
  auto check_mode = [&]<typename Mode>() {
    auto const& costas = js8core::protocol::costas(Mode::costas_type);
    constexpr int sps = Mode::downsample_sps;
    constexpr double rate = 12000.0 / Mode::downsample_factor;
    constexpr double offset = rate / (4.0 * sps);
    auto const reference = CoherentCostasReference::make(sps, costas);
    check(reference.has_value(), "valid submode builds a reference");
    if (!reference) return;

    for (int start : {0, sps / 2, 2 * sps}) {
      auto const samples = synthesize(costas, sps, start, start + 79 * sps,
                                      rate, offset, gains, phases);
      auto const aligned = reference->score(samples, start, rate, offset);
      bool all_blocks = aligned.has_value();
      if (aligned)
        for (auto const& block : aligned->blocks) all_blocks &= block && *block > 0.99999;
      check(all_blocks && aligned->support > 0.99999,
            "full nominal or late capture matches despite different block phases and gains");

      auto const wrong_frequency = reference->score(samples, start, rate, -offset);
      check(wrong_frequency && wrong_frequency->support < 0.02,
            "CFO correction preserves phase across symbols and rejects the wrong sign");
      auto const wrong_time = reference->score(samples, start + sps, rate, offset);
      check(wrong_time && wrong_time->support < 0.06, "a symbol of timing error breaks coherence");
    }

    auto const early = synthesize(costas, sps, -1, 79 * sps, rate, offset, gains, phases);
    auto const missing_head = reference->score(early, -1, rate, offset);
    check(missing_head && !missing_head->blocks[0] && missing_head->blocks[1] &&
          missing_head->blocks[2] && missing_head->support > 0.99999,
          "a clipped head leaves two complete, useful pilot blocks");

    auto late = synthesize(costas, sps, 2 * sps, 81 * sps, rate, offset, gains, phases);
    auto const missing_tail = reference->score(std::span(late).first(79 * sps),
                                               2 * sps, rate, offset);
    check(missing_tail && missing_tail->blocks[0] && missing_tail->blocks[1] &&
          !missing_tail->blocks[2] && missing_tail->support > 0.99999,
          "the observed span excludes a clipped final pilot block");
    check(!reference->score(std::span(late).first(40 * sps), 2 * sps, rate, offset),
          "one complete block cannot produce a score");

    auto const scaled_samples = [&] {
      auto scaled = late;
      for (auto& sample : scaled) sample *= 0.125f;
      return scaled;
    }();
    auto const normal_gain = reference->score(late, 2 * sps, rate, offset);
    auto const scaled_gain = reference->score(scaled_samples, 2 * sps, rate, offset);
    check(normal_gain && scaled_gain && normal_gain->blocks == scaled_gain->blocks,
          "common amplitude scaling does not change block scores");

    // A strong CW carrier occupies one tone for all symbols, not a Costas
    // sequence. It contributes coherently to only one of seven symbols.
    Samples carrier(79 * sps);
    for (std::size_t n = 0; n < carrier.size(); ++n) {
      double const phase = kTau * 2.0 * n / sps;
      carrier[n] = {static_cast<float>(20.0 * std::cos(phase)),
                    static_cast<float>(20.0 * std::sin(phase))};
    }
    auto const cw = reference->score(carrier, 0, rate, 0.0);
    check(cw && std::abs(cw->support - 1.0 / 49.0) < 1e-6,
          "a much stronger single tone cannot imitate the Costas sequence");
  };

  check_mode.operator()<data::ProfileA>();
  check_mode.operator()<data::ProfileB>();
  check_mode.operator()<data::ProfileC>();
  check_mode.operator()<data::ProfileE>();
  check_mode.operator()<data::ProfileI>();

  auto const& costas = js8core::protocol::costas(js8core::protocol::CostasType::Original);
  constexpr int sps = data::ProfileA::downsample_sps;
  constexpr double rate = 12000.0 / data::ProfileA::downsample_factor;
  constexpr double tone_hz = rate / sps;
  auto const reference = CoherentCostasReference::make(sps, costas);
  if (!reference) return 1;

  constexpr int start = 25;
  constexpr double frequency_step = tone_hz / 28.0;
  constexpr double target_hz = 7 * frequency_step;
  auto noisy = synthesize(costas, sps, start, start + 80 * sps, rate, target_hz,
                          {0.8, 1.0, 1.2}, phases);
  add_noise(noisy, 0.6f);
  Samples noise_only(noisy.size());
  add_noise(noise_only, 0.6f);
  double best_noise = -1.0;
  for (int time = start - 6; time <= start + 6; ++time) {
    for (int bin = -14; bin <= 14; ++bin) {
      double const hz = bin * frequency_step;
      auto const noise_score = reference->score(noise_only, time, rate, hz);
      if (noise_score && noise_score->support > best_noise) best_noise = noise_score->support;
    }
  }
  DecodeControl control;
  int probes = 0;
  control.optional_allowed = [&] { ++probes; return true; };
  auto const best = reference->search(noisy, start - 5, rate, -tone_hz / 2, tone_hz / 2, control);
  check(best && std::abs(best->frame_start - start) <= 1 &&
        std::abs(best->frequency_offset_hz - target_hz) <= frequency_step &&
        best->support > 0.7, "coherent search localizes both timing and frequency in noise");
  check(probes == 171, "search has a fixed 170-cell cap plus the final permission check");
  check(best_noise >= 0.0 && best_noise < 0.06, "noise-only grid has weak cross-block support");
  for (int stop_after : {1, 20, 142, 171}) {
    probes = 0;
    control.optional_allowed = [&] { return ++probes < stop_after; };
    check(!reference->search(noisy, start, rate, -tone_hz / 2, tone_hz / 2, control) &&
          probes == stop_after, "denial discards the entire coarse or refined search");
  }
  auto const restricted = reference->search(noisy, start, rate, 0.0, target_hz / 2, {});
  check(restricted && restricted->frequency_offset_hz >= 0.0 &&
        restricted->frequency_offset_hz <= target_hz / 2, "search stays in the requested band");

  auto phase_flips = synthesize(costas, sps, 0, 79 * sps, rate, -target_hz, gains, phases);
  auto const negative_offset = reference->score(phase_flips, 0, rate, -target_hz);
  check(negative_offset && negative_offset->support > 0.99999,
        "negative frequency offsets use the same convention");
  for (std::size_t n = 0; n < phase_flips.size(); ++n) {
    if (n / sps % 2) phase_flips[n] = -phase_flips[n];
  }
  auto const incoherent = reference->score(phase_flips, 0, rate, -target_hz);
  check(incoherent && std::abs(incoherent->support - 1.0 / 49.0) < 1e-6,
        "phase flips within a block defeat coherence despite correct pilot tones");

  auto single_block = synthesize(costas, sps, 0, 79 * sps, rate, target_hz,
                                 {100.0, 0.0, 0.0}, phases);
  check(!reference->score(single_block, 0, rate, target_hz), "silent blocks give no support");
  add_noise(single_block, 0.6f);
  auto const one = reference->score(single_block, 0, rate, target_hz);
  check(one && one->blocks[0] && *one->blocks[0] > 0.99 && one->support < 0.06,
        "the best block cannot dominate weak evidence in the other two");

  auto invalid = synthesize(costas, sps, 0, 79 * sps, rate, 0.0, gains, phases);
  invalid.front() = {std::numeric_limits<float>::quiet_NaN(), 0.0f};
  auto const rejected_block = reference->score(invalid, 0, rate, 0.0);
  check(rejected_block && !rejected_block->blocks[0] && rejected_block->support > 0.99999,
        "a nonfinite block does not contaminate the other two scores");
  check(!reference->score(invalid, 0, 0.0, 0.0) &&
        !reference->score(invalid, 0, rate, rate / 2), "invalid frequency geometry fails closed");
  check(!reference->score(invalid, std::numeric_limits<int>::min(), rate, 0.0) &&
        !reference->score(invalid, std::numeric_limits<int>::max(), rate, 0.0),
        "out-of-window hypotheses cannot read samples");
  auto invalid_pattern = costas;
  invalid_pattern[0][0] = 8;
  check(!CoherentCostasReference::make(7, costas) &&
        !CoherentCostasReference::make(129, costas) &&
        !CoherentCostasReference::make(sps, invalid_pattern), "invalid references are rejected");

  auto window = late_turbo_window();
  auto const deep = decode(window);
  window.params.deep_sync = false;
  auto const ordinary = decode(window);
  auto const recovered = [](DecodeRun const& run) {
    return run.frames.size() == 2 && run.frames.back().data == "Ux2zqX++++++" &&
        run.frames.back().type == 4 && std::abs(run.frames.back().frequency - 1506.0f) < 1.0f &&
        std::abs(run.frames.back().xdt - 1.13f) < 0.004f &&
        !run.frames.back().tracking_used && !run.frames.back().after_tracked_sic &&
        run.summary.decoded == 2 && run.summary.tracked_cancellations == 0;
  };
  check(ordinary.frames.size() == 1 && ordinary.frames.front().data == "HELLO-WORLD-",
        "without deep sync only the ordinary frame is reported");
  window.params.deep_sync = true;
  check(recovered(deep) && deep.frames.front().data == "HELLO-WORLD-" &&
        ordinary.frames.size() == 1 && deep.frames.front().frequency == ordinary.frames.front().frequency &&
        deep.frames.front().quality == ordinary.frames.front().quality,
        "deep sync appends the late frame without replacing ordinary results");
  DecodeControl deny;
  deny.optional_allowed = [] { return false; };
  auto const denied = decode(window, deny);
  check(denied.frames.size() == 1 && denied.frames.front().data == "HELLO-WORLD-" &&
        denied.summary.optional_work_skipped && !denied.summary.canceled,
        "denial suppresses deep sync without losing the ordinary frame");

  auto multi = window;
  test::TrackingSignal normal;
  normal.symbol_seconds = 1920.0 / 12000;
  normal.start_seconds = 0.5;
  normal.base_hz = 1650.0;
  legacy_encode(3, costas, "SJWkJnSNwzqH", normal.tones.data());
  auto const normal_audio = test::render_tracking_audio(
      std::array{test::TrackingWaveform(normal)}, 12000, 180000);
  multi.params.kposA = 48000;
  multi.params.kszA = static_cast<int>(normal_audio.size());
  multi.params.nsubmodes |= 1 << 0;
  std::copy(normal_audio.begin(), normal_audio.end(), multi.samples.begin() + multi.params.kposA);
  auto const multiple = decode(multi);
  check(multiple.frames.size() == 3 && multiple.frames[0].data == "HELLO-WORLD-" &&
        multiple.frames[1].data == "SJWkJnSNwzqH" && multiple.frames[2].data == "Ux2zqX++++++",
        "deep sync waits for ordinary work in all requested submodes");

  // The bytes straddle the end of the receive ring, but cover the same window.
  window.params.kposI = 719000;
  std::rotate(window.samples.begin(), window.samples.begin() + 1000, window.samples.end());
  check(recovered(decode(window)), "a wrapped window recovers the same late frame");
  window.params.kszI = 384 * 42;
  auto const clipped = decode(window);
  check(std::none_of(clipped.frames.begin(), clipped.frames.end(), [](auto const& frame) {
          return frame.data == "Ux2zqX++++++";
        }), "a short capture cannot manufacture the late decode from padding");

  auto weak = weak_pilot_window();
  auto const acquired = decode(weak);
  weak.params.deep_sync = false;
  check(decode(weak).frames.empty(), "ordinary acquisition misses the weak-pilot fixture");
  check(acquired.frames.size() == 1 && acquired.frames[0].data == "Ux2zqX++++++" &&
        acquired.frames[0].type == 4 && std::abs(acquired.frames[0].frequency - 1506.0f) < 1.0f &&
        std::abs(acquired.frames[0].xdt - 0.0125f) < 0.006f,
        "deep sync recovers a frame below the ordinary coarse cutoff");

  auto negative = late_turbo_window();
  negative.params.deep_sync = true;
  std::mt19937 random(764);
  for (int i = 0; i < negative.params.kszI; ++i) {
    int const noise = (static_cast<int>(random() >> 16) - 32768) / 5;
    negative.samples[i] = static_cast<std::int16_t>(noise);
  }
  check(decode(negative).frames.empty(), "deep sync emits no frame on the noise-only fixture");
  for (int i = 0; i < negative.params.kszI; ++i) {
    int const carrier = static_cast<int>(9000 * std::cos(kTau * 1506.0 * i / 12000.0));
    negative.samples[i] = static_cast<std::int16_t>(negative.samples[i] + carrier);
  }
  check(decode(negative).frames.empty(), "deep sync emits no frame on the carrier-plus-noise fixture");

  std::printf("%d coherent Costas score checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}

#include "tracking_waveform.hpp"

#include "js8core/decoder.hpp"
#include "src/decoder/soft_likelihood.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <random>

using namespace js8core::decoder;

namespace {

bool decodes_soft_fixture(bool inverted_data, bool coherent_likelihood) {
  using namespace js8core;
  int const start = inverted_data ? 6000 : 1350;
  int const symbol_samples = inverted_data ? 1920 : 384;
  int const window = inverted_data ? 180000 : 48000;
  auto const costas_type = inverted_data ? protocol::CostasType::Original
                                        : protocol::CostasType::Modified;
  constexpr char frame[] = "Ux2zqX++++++";
  test::TrackingSignal signal;
  signal.symbol_seconds = static_cast<double>(symbol_samples) / kJs8RxSampleRate;
  signal.start_seconds = static_cast<double>(start) / kJs8RxSampleRate;
  signal.amplitude = 6000;
  legacy_encode(4, protocol::costas(costas_type), frame, signal.tones.data());

  DecodeState state;
  state.samples = test::render_tracking_audio(
      std::array{test::TrackingWaveform(signal)}, kJs8RxSampleRate, window);
  std::mt19937 rng(inverted_data ? 109 : 105);
  int const data_amplitude = inverted_data ? -600 : 1295;
  for (int i = 0; i < window; ++i) {
    // Keep the pilots strong enough for acquisition while making the data
    // marginal. Take the generator's bits directly for portable noise.
    int const noise = (static_cast<int>(rng() >> 16) - 32768) * 22000 / 65536;
    int const symbol = i >= start ? (i - start) / symbol_samples : -1;
    int sample = state.samples[i];
    if (symbol >= 0 && symbol < kJs8NumSymbols && symbol % 36 >= 7)
      sample = sample * data_amplitude / 6000;
    state.samples[i] = static_cast<std::int16_t>(sample + noise);
  }
  state.samples.resize(kJs8NtMax * kJs8RxSampleRate);
  state.params.nfa = 1400;
  state.params.nfb = 1610;
  state.params.nfqso = 1500;
  if (inverted_data) {
    state.params.nsubmodes = 1;
    state.params.kszA = window;
  } else {
    state.params.nsubmodes = 1 << 4;
    state.params.kszI = window;
  }
  state.params.symbol_tracking = SymbolTracking::Disabled;
  state.params.deep_sync = false;
  state.params.coherent_likelihood = coherent_likelihood;
  DecodeControl control;
  if (!inverted_data)
    control.optional_allowed = [] { return false; };  // No OSD recovery.
  bool found = false;
  decode_with_control(state, [&](events::Variant const& event) {
    auto const* decoded = std::get_if<events::Decoded>(&event);
    found |= decoded && decoded->type == 4 && decoded->data == frame;
  }, control);
  return found;
}

}  // namespace

int main() {
  int failures = 0;
  auto check = [&](bool ok, char const* name) {
    if (!ok) {
      std::fprintf(stderr, "FAIL soft likelihood: %s\n", name);
      ++failures;
    }
  };

  SoftToneModel const model{1.0, 0.5};
  for (std::size_t tone = 0; tone < 8; ++tone) {
    ToneMagnitudes magnitudes{};
    magnitudes[tone] = 4.0f;
    auto const result = soft_symbol_llrs(magnitudes, model);
    bool correct = result.has_value();
    for (std::size_t bit = 0; bit < 3 && correct; ++bit)
      correct = ((*result)[bit] > 0.0f) == ((tone & (4u >> bit)) != 0);
    check(correct, "one strong tone maps to its three wire bits");
  }

  ToneMagnitudes tied{};
  tied.fill(2.0f);
  check(soft_symbol_llrs(tied, model) == SymbolLlrs{}, "equal magnitudes are erasures");
  check(soft_symbol_llrs({}, model) == SymbolLlrs{}, "silence has zero evidence");

  // Tone 4 is the strongest individual tone; the three nearly-as-likely
  // tones in the bit-zero group make that bit more probable overall.
  ToneMagnitudes const competing{2.0f, 2.0f, 2.0f, 0.0f, 2.1f, 0.0f, 0.0f, 0.0f};
  auto const llrs = soft_symbol_llrs(competing, model);
  double const expected = std::log((std::exp(0.5 * 2.1f * 2.1f) + 3.0) /
                                   (3.0 * std::exp(2.0) + 1.0));
  check(llrs && (*llrs)[0] < 0.0f && std::abs((*llrs)[0] - expected) < 1e-6,
        "all four tones in each group are marginalized");

  auto scaled = competing;
  for (float& value : scaled) value *= 8.0f;
  check(soft_symbol_llrs(scaled, {64.0, 0.5}) == llrs,
        "common scaling of the tone magnitudes and noise amplitude is invariant");

  ToneMagnitudes extreme{};
  extreme[7] = std::numeric_limits<float>::max();
  auto const bounded = soft_symbol_llrs(extreme, {1e-30, 1.0});
  check(bounded && std::all_of(bounded->begin(), bounded->end(), [](float llr) {
          return std::isfinite(llr) && llr > 0.0f && llr <= 12.0f;
        }), "extreme inputs are finite and bounded");
  extreme.fill(std::numeric_limits<float>::max());
  check(soft_symbol_llrs(extreme, model) == SymbolLlrs{}, "extreme ties are also erasures");
  extreme[0] = std::numeric_limits<float>::quiet_NaN();
  check(!soft_symbol_llrs(extreme, model) && !soft_symbol_llrs(tied, {0.0, 0.5}),
        "invalid samples or noise model ask the caller to fall back");

  std::array<PilotTone, 21> pilots{};
  for (std::size_t i = 0; i < pilots.size(); ++i) {
    pilots[i].magnitudes.fill(2.0f);
    pilots[i].active_tone = i % 8;
    pilots[i].magnitudes[pilots[i].active_tone] = 4.0f;
    pilots[i].magnitudes[(pilots[i].active_tone + 1) % 8] = 1000.0f;
  }
  auto const fit = estimate_soft_tone_model(pilots);
  check(fit && std::abs(fit->noise_energy - 4.0 / std::log(2.0)) < 1e-12 &&
        std::abs(fit->signal_fraction - 0.75) < 1e-12,
        "known inactive pilot bins yield a noise estimate robust to an outlier tone");
  check(!estimate_soft_tone_model(std::span(pilots).first(13)),
        "incomplete pilot evidence asks the caller to fall back");
  for (auto& pilot : pilots) pilot.magnitudes.fill(2.0f);
  check(!estimate_soft_tone_model(pilots), "pilots must distinguish signal from noise");
  for (auto& pilot : pilots) pilot.magnitudes.fill(0.0f);
  check(!estimate_soft_tone_model(pilots), "zero-energy pilot estimates are invalid");

  auto const& costas = js8core::protocol::costas(js8core::protocol::CostasType::Modified);
  FrameToneMagnitudes frame{};
  for (auto& row : frame) row.fill(2.0f);
  for (std::size_t block = 0; block < costas.size(); ++block) {
    for (std::size_t column = 0; column < costas[block].size(); ++column)
      frame[static_cast<std::size_t>(costas[block][column])][block * 36 + column] = 4.0f;
  }
  // Both edge data tones appear strong in the FFT bins. Only a full
  // symbol's samples should be allowed to turn them into bit evidence.
  frame[7][7] = frame[7][71] = 4.0f;
  auto const complete = soft_frame_llrs(frame, costas, 0, 8, 79 * 8);
  auto const short_tail = soft_frame_llrs(frame, costas, 0, 8, 72 * 8 - 1);
  auto const short_head = soft_frame_llrs(frame, costas, -7 * 8 - 1, 8, 79 * 8);
  auto const has_positive_bits = [](auto bit) {
    return bit[0] > 0.0f && bit[1] > 0.0f && bit[2] > 0.0f;
  };
  constexpr SymbolLlrs erased{};
  check(complete && has_positive_bits(complete->begin()) &&
        has_positive_bits(complete->end() - 3), "complete edge data symbols retain their evidence");
  check(short_tail && has_positive_bits(short_tail->begin()) &&
        std::equal(short_tail->end() - 3, short_tail->end(), erased.begin()),
        "a partially observed trailing data symbol is erased");
  check(short_head && std::equal(short_head->begin(), short_head->begin() + 3, erased.begin()) &&
        has_positive_bits(short_head->end() - 3), "a partially observed leading data symbol is erased");

  constexpr bool expect_soft = JS8CORE_EXPECT_SOFT_DEMAPPER;
  check(decodes_soft_fixture(false, false) == expect_soft,
        expect_soft ? "pass 2 recovers the marginal frame with the soft metric"
                    : "the legacy metric does not recover the soft pass-2 fixture");
  if (expect_soft) {
    // Strong pilots predict the opposite phase from the one on the data.
    // Replacing ordinary pass 2 with coherent scoring lost this frame.
    check(decodes_soft_fixture(true, false) && decodes_soft_fixture(true, true),
          "coherent retry preserves the established pass-2 recovery");
  }

  return failures ? 1 : 0;
}

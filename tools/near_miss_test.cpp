#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <random>

#include "js8core/decoder.hpp"
#include "js8core/engine.hpp"
#include "src/decoder/decoder_tables.hpp"
#include "src/decoder/ldpc.hpp"
#include "tracking_waveform.hpp"

using namespace js8core;

namespace {

constexpr char frame[] = "Ux2zqX++++++";
constexpr int rate = kJs8RxSampleRate;
constexpr int sps = 384;
constexpr int count = 4 * rate;
auto const& costas = protocol::costas(protocol::CostasType::Modified);
static_assert(EngineConfig{}.ldpc_rescue && EngineConfig{}.aided_redemodulation);
static_assert(DecodeParams{}.ldpc_rescue && DecodeParams{}.aided_redemodulation);

DecodeState fixture(unsigned seed, int data_amplitude, double clock_ppm, double drift_in_tones,
                    bool data_fades = false) {
  test::TrackingSignal signal;
  signal.symbol_seconds = static_cast<double>(sps) / rate;
  signal.base_hz = 1504.0;
  signal.start_seconds = 1350.37 / rate;
  signal.phase_radians = 0.82;
  signal.amplitude = 6000;
  signal.clock_ppm = clock_ppm;
  signal.frequency_slope_hz_s =
      drift_in_tones / (signal.symbol_seconds * signal.symbol_seconds * kJs8NumSymbols);
  legacy_encode(4, costas, frame, signal.tones.data());

  DecodeState state;
  state.samples = test::render_tracking_audio(std::array{test::TrackingWaveform(signal)}, rate, count);
  std::mt19937 rng(seed);
  std::array<double, kJs8NumSymbols> gains;
  gains.fill(1.0);
  if (data_fades) {
    std::mt19937 fading(seed ^ 584392u);
    for (auto& gain : gains) {
      auto const value = fading() % 100;
      gain = value < 10 ? 0.0 : (value < 45 ? 0.3 : 1.2);
    }
  }
  for (int i = 0; i < count; ++i) {
    int const symbol = (i - 1350) / sps;
    double const gain = symbol >= 0 && symbol < kJs8NumSymbols ? gains[symbol] : 1.0;
    double const scale = symbol >= 0 && symbol % 36 < 7 ? 1.0 : gain * data_amplitude / signal.amplitude;
    int const noise = (static_cast<int>(rng() >> 16) - 32768) * 24000 / 65536;
    state.samples[i] = static_cast<int>(state.samples[i] * scale) + noise;
  }
  state.samples.resize(kJs8NtMax * rate);
  state.params.nfa = 1470;
  state.params.nfb = 1550;
  state.params.nsubmodes = 1 << 4;
  state.params.kszI = count;
  state.params.symbol_tracking = SymbolTracking::Disabled;
  state.params.deep_sync = false;
  state.params.syncStats = true;
  return state;
}

struct DecodeRun {
  std::vector<events::Decoded> frames;
  std::vector<events::SyncState> decoded_sync;
  std::vector<std::pair<float, float>> candidate_sync;
};

DecodeRun decode(DecodeState const& state, DecodeControl const& control = {}) {
  DecodeRun run;
  decode_with_control(state, [&](events::Variant const& event) {
    if (auto const* result = std::get_if<events::Decoded>(&event))
      run.frames.push_back(*result);
    if (auto const* sync = std::get_if<events::SyncState>(&event)) {
      if (sync->kind == events::SyncState::Kind::Decoded)
        run.decoded_sync.push_back(*sync);
      else if (sync->kind == events::SyncState::Kind::Candidate)
        run.candidate_sync.emplace_back(sync->frequency, sync->dt);
    }
  }, control);
  return run;
}

bool recovered(DecodeRun const& run) {
  return run.frames.size() == 1 && run.frames.front().type == 4 && run.frames.front().data == frame;
}

}  // namespace

int main() {
  int failures = 0;
  auto check = [&](bool success, char const* label) {
    if (!success) {
      ++failures;
      std::fprintf(stderr, "FAIL near miss: %s\n", label);
    }
  };

  std::array<int, kJs8NumSymbols> tones{};
  legacy_encode(4, costas, frame, tones.data());
  std::array<float, 174> llr{};
  std::mt19937 random(276);
  for (int i = 0; i < 174; ++i) {
    int const tone = tones[i / 3 + (i < 87 ? 7 : 14)];
    float const sign = (tone & (4 >> (i % 3))) ? 1.0f : -1.0f;
    llr[i] = sign * 2.4f + (static_cast<int>(random() >> 16) - 32768) / 8192.0f;
  }
  std::array<std::int8_t, 87> payload{};
  std::array<std::int8_t, 174> codeword{};
  decoder::BpNearMiss near;
  check(decoder::belief_propagation(llr, payload, codeword, 60, true, &near) < 0 &&
        near.failed_checks > 0 && near.failed_checks <= 14, "BP retains a nonzero near miss");
  int unsatisfied = 0;
  for (auto const& parity : decoder::data::kCheckNodes) {
    int sum = 0;
    for (int i = 0; i < parity.count; ++i) sum += near.bits[parity.bits[i]];
    unsatisfied += sum & 1;
  }
  check(near.failed_checks == unsatisfied, "saved tentative word and syndrome agree");

  auto retry = llr;
  for (auto& value : retry) value *= 1.5f;
  check(decoder::belief_propagation(retry, payload, codeword, 150, false) < 0,
        "legacy extended BP misses the frame");
  bool rescued = false;
  for (float scale : {0.85f, 1.15f}) {
    retry = llr;
    for (auto& value : retry) value *= scale;
    rescued |= decoder::belief_propagation(retry, payload, codeword, 90, false) >= 0 &&
        decoder::crc_ok(payload) && decoder::decode_message(payload) == frame;
  }
  check(rescued, "bounded LLR rescaling recovers the frame with a valid CRC");
  DecodeControl exhausted;
  int probes = 0;
  exhausted.optional_allowed = [&] { return ++probes < 3; };
  decoder::BpNearMiss canceled;
  check(decoder::belief_propagation(llr, payload, codeword, 90, false, &canceled, &exhausted) < 0 &&
        canceled.failed_checks == decoder::BpNearMiss{}.failed_checks && probes == 3,
        "mid-run denial discards partial BP diagnostics");

  DecodeControl denied;
  denied.optional_allowed = [] { return false; };
#if JS8CORE_EXPECT_SOFT_DEMAPPER
  auto faded = fixture(1730, 1690, 0.0, 0.0, true);
  faded.params.aided_redemodulation = false;
  check(recovered(decode(faded)), "syndrome-guided rescue recovers the waveform with deep data fades");
  check(decode(faded, denied).frames.empty(), "syndrome-guided rescue respects budget denial");
  faded.params.ldpc_rescue = false;
  check(decode(faded).frames.empty(), "ordinary attempts including OSD miss the faded waveform");
#endif
  auto state = fixture(137, 1510, 2000.0, 0.8);
  auto const aided = decode(state);
  check(recovered(aided), "default re-demodulation recovers the drifting waveform");
  if (recovered(aided)) {
    auto const& decoded = aided.frames.front();
    check(aided.decoded_sync.size() == 1 &&
          aided.decoded_sync[0].frequency == decoded.frequency &&
          std::abs(aided.decoded_sync[0].dt - decoded.xdt - 0.1f) < 0.00001f &&
          std::abs(decoded.frequency - 1507.0f) < 3.0f && decoded.snr > -60,
          "aided frame and sync event use the same refined extraction");
  }
  check(decode(state, denied).frames.empty(), "re-demodulation respects budget denial");
  state.params.aided_redemodulation = false;
  auto const rescue_only = decode(state);
  check(rescue_only.frames.empty(), "rescue alone misses the drifting waveform");
  check(aided.candidate_sync == rescue_only.candidate_sync,
        "reporting an aided frame does not trigger another cancellation pass");
  state.params.ldpc_rescue = false;
  check(decode(state).frames.empty(), "baseline misses the drifting waveform");

  auto strong = fixture(7, 6000, 0.0, 0.0);
  check(recovered(decode(strong, denied)), "ordinary strong decode remains available without budget");
  return failures != 0;
}

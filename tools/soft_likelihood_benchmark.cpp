#include "tracking_waveform.hpp"

#include "js8core/decoder.hpp"
#include "js8core/protocol/constants.hpp"
#include "js8core/protocol/submode.hpp"
#include "js8core/protocol/varicode.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string_view>

using namespace js8core;

namespace {

constexpr int kRate = protocol::kJs8RxSampleRate;

void set_window(DecodeParams& params, protocol::SubmodeId mode, int size) {
  params.nsubmodes = 1 << static_cast<int>(mode);
  switch (mode) {
    case protocol::SubmodeId::A: params.kszA = size; break;
    case protocol::SubmodeId::B: params.kszB = size; break;
    case protocol::SubmodeId::C: params.kszC = size; break;
    case protocol::SubmodeId::E: params.kszE = size; break;
    case protocol::SubmodeId::I: params.kszI = size; break;
  }
}

std::uint64_t input_hash(std::span<std::int16_t const> samples) {
  std::uint64_t hash = 14695981039346656037ull;
  for (auto sample : samples) {
    auto const bits = static_cast<std::uint16_t>(sample);
    hash = (hash ^ (bits & 0xffu)) * 1099511628211ull;
    hash = (hash ^ (bits >> 8)) * 1099511628211ull;
  }
  return hash;
}

bool parse(std::string_view arg, unsigned& number) {
  auto const result = std::from_chars(arg.data(), arg.data() + arg.size(), number);
  return result.ec == std::errc{} && result.ptr == arg.data() + arg.size();
}

}  // namespace

int main(int argc, char** argv) {
  unsigned first = 0, trials = 8;
  if (argc > 3 || (argc >= 2 && !parse(argv[1], first)) ||
      (argc == 3 && !parse(argv[2], trials)) || trials == 0 || trials > 10000 ||
      first > 1000000) {
    std::fprintf(stderr, "usage: %s [first-seed [trials-per-case]]\n", argv[0]);
    return 2;
  }

  struct Case {
    char const* name;
    int bin_snr_db;
    int frames;
  };
  constexpr Case cases[] = {
      {"weak-4", 4, 1}, {"weak-6", 6, 1}, {"weak-8", 8, 1},
      {"strong", 28, 1}, {"collision", 28, 2}, {"noise", 0, 0},
  };
  constexpr double noise_sigma = 1700.0;
  std::puts("mode,case,seed,input_hash,expected,primary,secondary,unexpected,cpu_ms");
  for (auto const& mode : protocol::submodes()) {
    int const size = mode.tx_seconds * kRate;
    DecodeState state;
    state.params.symbol_tracking = SymbolTracking::Disabled;
    state.params.tracked_sic = false;
    state.params.deep_sync = false;
    state.params.coherent_likelihood = false;
    state.params.nfa = 1250;
    state.params.nfb = 1810;
    state.params.nfqso = 1500;
    set_window(state.params, mode.id, size);
    state.samples.resize(protocol::kJs8NtMax * kRate);
    legacy_decode(state, {});  // Exclude one-time allocations from CPU samples.
    auto const& costas = protocol::costas(mode.id == protocol::SubmodeId::A
        ? protocol::CostasType::Original : protocol::CostasType::Modified);

    for (auto const& scenario : cases) {
      for (unsigned seed = first; seed < first + trials; ++seed) {
        char primary_text[20], secondary_text[20];
        std::snprintf(primary_text, sizeof(primary_text), "CQ A%06u", seed);
        std::snprintf(secondary_text, sizeof(secondary_text), "QSL B%06u", seed);
        int consumed = 0;
        auto const primary = protocol::varicode::pack_fast_data_message(primary_text, &consumed);
        auto const secondary = protocol::varicode::pack_fast_data_message(secondary_text, &consumed);
        if (primary.size() != 12 || secondary.size() != 12 || primary == secondary) return 2;

        test::TrackingSignal signal;
        signal.symbol_seconds = static_cast<double>(mode.symbol_samples) / kRate;
        signal.start_seconds = mode.start_delay_ms / 1000.0 + 0.011;
        signal.base_hz = 1500.0 + 0.3 * (static_cast<int>(seed % 9) - 4);
        signal.phase_radians = 0.27 * (seed % 23);
        // At exact alignment the real-sample model gives matched-bin
        // SNR = A² * symbol_samples / (4 * noise_sigma²).
        signal.amplitude = 2.0 * noise_sigma *
            std::sqrt(std::pow(10.0, scenario.bin_snr_db / 10.0) / mode.symbol_samples);
        legacy_encode(4, costas, primary.data(), signal.tones.data());
        auto interferer = signal;
        interferer.base_hz += 3.5 / signal.symbol_seconds;
        interferer.start_seconds += 0.3 * signal.symbol_seconds;
        interferer.phase_radians += 1.2;
        interferer.amplitude *= 0.65;
        legacy_encode(4, costas, secondary.data(), interferer.tones.data());

        std::array const waveforms{test::TrackingWaveform(signal), test::TrackingWaveform(interferer)};
        state.samples = test::render_tracking_audio(
            std::span(waveforms).first(static_cast<std::size_t>(scenario.frames)),
            kRate, size, noise_sigma, 0x93376u + seed);
        auto const hash = input_hash(state.samples);
        state.samples.resize(protocol::kJs8NtMax * kRate);

        bool found_primary = false, found_secondary = false;
        int unexpected = 0;
        auto const start = std::clock();
        legacy_decode(state, [&](events::Variant const& event) {
          auto const* decoded = std::get_if<events::Decoded>(&event);
          if (!decoded) return;
          if (scenario.frames >= 1 && decoded->type == 4 && decoded->data == primary)
            found_primary = true;
          else if (scenario.frames == 2 && decoded->type == 4 && decoded->data == secondary)
            found_secondary = true;
          else
            ++unexpected;
        });
        double const ms = 1000.0 * (std::clock() - start) / CLOCKS_PER_SEC;
        std::printf("%.*s,%s,%u,%016llx,%d,%d,%d,%d,%.3f\n",
                    static_cast<int>(mode.name.size()), mode.name.data(),
                    scenario.name, seed, static_cast<unsigned long long>(hash), scenario.frames,
                    found_primary, found_secondary, unexpected, ms);
      }
    }
  }
}

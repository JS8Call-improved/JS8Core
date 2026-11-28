#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

#include "js8core/decoder.hpp"
#include "js8core/engine.hpp"
#include "js8core/protocol/constants.hpp"
#include "js8core/protocol/costas.hpp"

using namespace js8core;

namespace {

constexpr char frame[] = "Ux2zqX++++++";
constexpr int rate = protocol::kJs8RxSampleRate;
constexpr int sps = 1920;  // Normal: 6.25 Hz tone spacing
constexpr double start_seconds = 0.5;
constexpr double base_hz = 1500.0;
constexpr double tau = 6.28318530717958647692;
// Seed 2 is the lowest of seeds 1 to 20 for which, at amplitudes 700, 1000
// and 1500 (noise sigma 1000), the spread reading decodes its faded frame and
// the decoder with spread_reading off does not (9 of the 20 seeds do).
constexpr unsigned faded_seed = 2;
constexpr double faded_amplitude = 1000.0;
static_assert(DecodeParams{}.spread_reading && EngineConfig{}.spread_reading);

// std::mt19937's output is fixed by the C++ standard; the standard
// distributions are not, so draws are made here from its raw 32-bit words.
double uniform(std::mt19937& rng) {  // in (0, 1)
  return (static_cast<double>(rng()) + 0.5) / 4294967296.0;
}
double gaussian(std::mt19937& rng, double sigma) {  // Box-Muller, one value per pair
  double const radius = std::sqrt(-2.0 * std::log(uniform(rng)));
  return sigma * radius * std::cos(tau * uniform(rng));
}

// A Normal-speed frame through a fast-fading channel: a complex gain made of
// paths with Gaussian Doppler offsets (two-sided spread = 2 sigma), as in the
// Watterson model with one path, plus white noise.
DecodeState fixture(unsigned seed, double amplitude, double spread_hz, bool signal = true) {
  std::array<int, protocol::kJs8NumSymbols> tones{};
  legacy_encode(4, protocol::costas(protocol::CostasType::Original), frame, tones.data());

  std::mt19937 rng(seed);
  double const doppler_sigma = spread_hz > 0.0 ? spread_hz / 2.0 : 1.0;
  constexpr int paths = 32;
  std::array<double, paths> path_hz{};
  std::array<double, paths> path_phase{};
  for (int k = 0; k < paths; ++k) {
    path_hz[static_cast<std::size_t>(k)] = gaussian(rng, doppler_sigma);
    path_phase[static_cast<std::size_t>(k)] = tau * uniform(rng);
  }

  int const total = 15 * rate;
  std::vector<double> audio(static_cast<std::size_t>(total), 0.0);
  if (signal) {
    double carrier = 0.0;
    int const first = static_cast<int>(start_seconds * rate);
    for (int symbol = 0; symbol < protocol::kJs8NumSymbols; ++symbol) {
      double const hz = base_hz + tones[static_cast<std::size_t>(symbol)] * (double(rate) / sps);
      for (int n = 0; n < sps; ++n) {
        int const index = first + symbol * sps + n;
        double const t = double(index) / rate;
        std::complex<double> gain{1.0, 0.0};
        if (spread_hz > 0.0) {
          gain = {0.0, 0.0};
          for (int k = 0; k < paths; ++k)
            gain += std::polar(1.0, tau * path_hz[static_cast<std::size_t>(k)] * t +
                                        path_phase[static_cast<std::size_t>(k)]);
          gain /= std::sqrt(double(paths));
        }
        audio[static_cast<std::size_t>(index)] =
            amplitude * std::real(gain * std::polar(1.0, carrier));
        carrier += tau * hz / rate;
      }
    }
  }
  std::mt19937 noise_rng(seed ^ 0x5eedu);

  DecodeState state;
  state.samples.assign(static_cast<std::size_t>(protocol::kJs8NtMax * rate), 0);
  for (int i = 0; i < total; ++i) {
    double const value = std::clamp(audio[static_cast<std::size_t>(i)] + gaussian(noise_rng, 1000.0),
                                    -32768.0, 32767.0);
    state.samples[static_cast<std::size_t>(i)] = static_cast<std::int16_t>(std::lround(value));
  }
  state.params.nfqso = static_cast<int>(base_hz);
  state.params.nfa = static_cast<int>(base_hz) - 100;
  state.params.nfb = static_cast<int>(base_hz) + 100;
  state.params.nsubmodes = 1;
  state.params.kposA = 0;
  state.params.kszA = total;
  state.params.symbol_tracking = SymbolTracking::Disabled;
  state.params.deep_sync = false;
  return state;
}

std::vector<events::Decoded> decode(DecodeState const& state, DecodeControl const& control = {}) {
  std::vector<events::Decoded> frames;
  decode_with_control(state, [&](events::Variant const& event) {
    if (auto const* result = std::get_if<events::Decoded>(&event)) frames.push_back(*result);
  }, control);
  return frames;
}

bool recovered(std::vector<events::Decoded> const& frames) {
  return frames.size() == 1 && frames.front().type == 4 && frames.front().data == frame &&
         std::abs(frames.front().frequency - base_hz) < 3.0f;
}

}  // namespace

int main() {
  int failures = 0;
  auto check = [&](bool success, char const* label) {
    if (!success) {
      ++failures;
      std::fprintf(stderr, "FAIL spread reading: %s\n", label);
    }
  };

  DecodeControl denied;
  denied.optional_allowed = [] { return false; };

  auto faded = fixture(faded_seed, faded_amplitude, 10.0);
  check(recovered(decode(faded)), "spread reading recovers the 10 Hz fast-fading frame");
  check(decode(faded, denied).empty(), "spread reading respects budget denial");
  faded.params.spread_reading = false;
  check(decode(faded).empty(), "the other attempts and retries miss the fast-fading frame");

  auto noise = fixture(faded_seed, 0.0, 10.0, false);
  check(decode(noise).empty(), "noise alone gives no frame");

  auto steady = fixture(faded_seed, faded_amplitude, 0.0);
  check(recovered(decode(steady, denied)), "ordinary decode of the unfaded frame needs no budget");
  return failures != 0;
}

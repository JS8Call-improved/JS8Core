#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "js8core/decoder.hpp"
#include "js8core/engine.hpp"
#include "js8core/protocol/constants.hpp"
#include "js8core/protocol/costas.hpp"
#include "src/decoder/ldpc.hpp"

using namespace js8core;

namespace {

constexpr int kRate = protocol::kJs8RxSampleRate;
constexpr int kSymbols = protocol::kJs8NumSymbols;
constexpr double kTwoPi = 6.28318530717958647692;

struct ModeCase {
  char name;
  int symbol_samples;
  int tx_seconds;
  double start_s;
  int bit;
  protocol::CostasType costas;
};

std::vector<std::int16_t> synthesize(std::array<int, kSymbols> const& tones,
                                     ModeCase const& mode, double base_hz,
                                     double amplitude, double start_s,
                                     double noise_sigma, std::uint32_t seed) {
  const int total = mode.tx_seconds * kRate;
  std::vector<std::int16_t> samples(
      static_cast<std::size_t>(protocol::kJs8NtMax * kRate), 0);
  const double baud = static_cast<double>(kRate) / mode.symbol_samples;

  std::mt19937 rng(seed);
  std::normal_distribution<double> noise(0.0, 1.0);

  const int start_index = static_cast<int>(start_s * kRate);
  for (int k = 0; k < kSymbols; ++k) {
    const double freq = base_hz + tones[static_cast<std::size_t>(k)] * baud;
    const double phase0 = kTwoPi * freq * start_s;
    for (int n = 0; n < mode.symbol_samples; ++n) {
      const double t = static_cast<double>(n) / kRate;
      const double value = amplitude * std::sin(kTwoPi * freq * t + phase0);
      const int index = start_index + k * mode.symbol_samples + n;
      if (index >= 0 && index < total) {
        samples[static_cast<std::size_t>(index)] =
            static_cast<std::int16_t>(std::lround(value));
      }
    }
  }

  if (noise_sigma > 0.0) {
    for (int i = 0; i < total; ++i) {
      double value = samples[static_cast<std::size_t>(i)] + noise_sigma * noise(rng);
      value = std::clamp(value, -32768.0, 32767.0);
      samples[static_cast<std::size_t>(i)] = static_cast<std::int16_t>(std::lround(value));
    }
  }
  return samples;
}

bool decode_once(std::vector<std::int16_t> const& samples, ModeCase const& mode,
                 double base_hz, std::string& text, int& type) {
  DecodeState state;
  state.samples = samples;
  state.params.utc = 0;
  state.params.nfqso = static_cast<int>(base_hz);
  state.params.nfa = static_cast<int>(base_hz) - 100;
  state.params.nfb = static_cast<int>(base_hz) + 100;
  state.params.nsubmodes = 1 << mode.bit;
  state.params.syncStats = false;

  if (mode.bit == 0) { state.params.kposA = 0; state.params.kszA = mode.tx_seconds * kRate; }
  if (mode.bit == 1) { state.params.kposB = 0; state.params.kszB = mode.tx_seconds * kRate; }
  if (mode.bit == 2) { state.params.kposC = 0; state.params.kszC = mode.tx_seconds * kRate; }
  if (mode.bit == 3) { state.params.kposE = 0; state.params.kszE = mode.tx_seconds * kRate; }
  if (mode.bit == 4) { state.params.kposI = 0; state.params.kszI = mode.tx_seconds * kRate; }

  bool found = false;
  legacy_decode(state, [&](events::Variant const& ev) {
    if (auto const* decoded = std::get_if<events::Decoded>(&ev)) {
      if (!found) {
        text = decoded->data;
        type = decoded->type;
        found = true;
      }
    }
  });
  return found;
}

}  // namespace

bool check_crc_known_answers() {
  // Values produced by the reference JS8 CRC-12 (polynomial 0xC06, final
  // XOR 42). These guard against the encoder and decoder drifting together.
  std::array<std::uint8_t, 11> const a = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55,
                                          0x66, 0x77, 0x88, 0x99, 0xaa};
  std::array<std::uint8_t, 11> const b = {0xde, 0xad, 0xbe, 0xef, 0x01, 0x23,
                                          0x45, 0x67, 0x89, 0xab, 0xcd};
  if (decoder::crc12(a.data(), a.size()) != 0x07e) {
    std::printf("FAIL: CRC-12 known answer A = %03x\n",
                decoder::crc12(a.data(), a.size()));
    return false;
  }
  if (decoder::crc12(b.data(), b.size()) != 0xa35) {
    std::printf("FAIL: CRC-12 known answer B = %03x\n",
                decoder::crc12(b.data(), b.size()));
    return false;
  }
  return true;
}

int main() {
  if (!check_crc_known_answers()) return 1;
  {
    // Search ranges that are inverted or collapse to a single FFT bin
    // must return no candidates rather than divide by zero.
    DecodeState invalid;
    invalid.samples.assign(static_cast<std::size_t>(protocol::kJs8NtMax * kRate), 0);
    invalid.params.nsubmodes = 1;
    invalid.params.kposA = 0;
    invalid.params.kszA = 15 * kRate;
    for (auto [low, high] : {std::pair{2300, 300}, std::pair{1501, 1502}}) {
      invalid.params.nfa = low;
      invalid.params.nfb = high;
      if (legacy_decode(invalid, [](events::Variant const&) {}) != 0) {
        std::printf("FAIL invalid frequency band %d-%d decoded a frame\n", low, high);
        return 1;
      }
    }
  }

  {
    // Validate LDPC on a known-valid frame at saturated log-likelihoods,
    // then force a failed-check iteration with a highly confident bit error.
    std::array<int, kSymbols> tones{};
    legacy_encode(0, protocol::costas(protocol::CostasType::Original),
                  "HELLO-WORLD-", tones.data());
    std::array<float, 174> llr{};
    for (int i = 0; i < 87; ++i) {
      auto const mask = 1 << (2 - i % 3);
      llr[static_cast<std::size_t>(i)] =
          (tones[7 + i / 3] & mask) ? 1000000.0f : -1000000.0f;
      llr[static_cast<std::size_t>(87 + i)] =
          (tones[43 + i / 3] & mask) ? 1000000.0f : -1000000.0f;
    }
    std::array<std::int8_t, 87> payload{};
    std::array<std::int8_t, 174> codeword{};
    if (decoder::belief_propagation(llr, payload, codeword) < 0 ||
        !decoder::crc_ok(payload) ||
        decoder::decode_message(payload) != "HELLO-WORLD-") {
      std::printf("FAIL: saturated valid LDPC frame\n");
      return 1;
    }
    llr[0] = -llr[0];
    int const result = decoder::belief_propagation(llr, payload, codeword);
    if (result < -1 || result > 174) {
      std::printf("FAIL: saturated corrupted LDPC frame\n");
      return 1;
    }
  }

  const std::array<ModeCase, 5> modes = {{
      {'A', 1920, 15, 0.5, 0, protocol::CostasType::Original},
      {'B', 1200, 10, 0.2, 1, protocol::CostasType::Modified},
      {'C', 600, 6, 0.1, 2, protocol::CostasType::Modified},
      {'E', 3840, 30, 0.5, 3, protocol::CostasType::Modified},
      {'I', 384, 4, 0.1, 4, protocol::CostasType::Modified},
  }};

  struct Message {
    char const* text;
    int type;
  };
  const std::array<Message, 3> messages = {{
      {"HELLO-WORLD-", 0},
      {"TEST12345678", 2},
      {"CQ-CQ-CQ-73-", 1},
  }};

  const std::array<double, 3> frequencies = {701.7, 1501.7, 2303.1};

  int failures = 0;
  int cases = 0;

  for (auto const& mode : modes) {
    for (auto const& message : messages) {
      for (double frequency : frequencies) {
        for (double noise : {0.0, 300.0, 700.0}) {
          std::array<int, kSymbols> tones{};
          legacy_encode(message.type, protocol::costas(mode.costas),
                        message.text, tones.data());

          const double start = mode.start_s + 0.013;
          auto samples = synthesize(tones, mode, frequency, 5000.0, start,
                                    noise, 0x1234u + cases);
          std::string text;
          int type = -1;
          ++cases;
          if (!decode_once(samples, mode, frequency, text, type)) {
            std::printf("FAIL mode %c freq %.0f noise %.0f msg '%s': no decode\n",
                        mode.name, frequency, noise, message.text);
            ++failures;
            continue;
          }
          if (text != message.text || type != message.type) {
            std::printf("FAIL mode %c freq %.0f noise %.0f: got '%s' type %d\n",
                        mode.name, frequency, noise, text.c_str(), type);
            ++failures;
          }
        }
      }
    }
  }

  std::printf("%d/%d decode cases passed\n", cases - failures, cases);
  if (failures == 0) std::printf("PASS\n");
  return failures == 0 ? 0 : 1;
}

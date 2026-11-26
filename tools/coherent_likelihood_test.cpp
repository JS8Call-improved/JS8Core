// Unit tests for the coherent data-likelihood path (cleanroom port).
//
// Covers the three stages of src/decoder/soft_likelihoods.cpp:
//   - estimate_disturbance: per-tone noise variance + pooled-variance whitening
//   - estimate_pilot_channel: bounded (omega, beta) phase fit with held-out validation
//   - make_soft_likelihoods: coherent mean when validated, Rician fallback otherwise
//
// A decode-count regression against pinned WAV files runs only when .wav paths are
// passed on the command line (mirroring tools/wav_decode_test.cpp); otherwise it
// is skipped so the test suite stays green without fixtures.

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "js8core/decoder.hpp"
#include "js8core/decoder_state.hpp"
#include "js8core/protocol/costas.hpp"

#include "src/decoder/decoder_tables.hpp"
#include "src/decoder/soft_likelihoods.hpp"

using namespace js8core;
using namespace js8core::decoder;
using namespace js8core::protocol;

namespace {

std::uint32_t read_u32(std::ifstream& in) {
  std::uint8_t bytes[4]{};
  in.read(reinterpret_cast<char*>(bytes), 4);
  return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8) |
         (static_cast<std::uint32_t>(bytes[2]) << 16) | (static_cast<std::uint32_t>(bytes[3]) << 24);
}

std::uint16_t read_u16(std::ifstream& in) {
  std::uint8_t bytes[2]{};
  in.read(reinterpret_cast<char*>(bytes), 2);
  return static_cast<std::uint16_t>(bytes[0] | (bytes[1] << 8));
}

bool load_wav(std::string const& path, std::vector<std::int16_t>& samples) {
  samples.clear();
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) return false;
  auto const file_size = in.tellg();
  if (file_size < 12) return false;
  in.seekg(0);
  char riff[4]{};
  in.read(riff, 4);
  if (std::memcmp(riff, "RIFF", 4) != 0) return false;
  std::uint64_t const riff_end = 8ull + read_u32(in);
  if (riff_end < 12 || riff_end > static_cast<std::uint64_t>(file_size)) return false;
  char wave[4]{};
  in.read(wave, 4);
  if (std::memcmp(wave, "WAVE", 4) != 0) return false;
  bool pcm = false;
  while (in && static_cast<std::uint64_t>(in.tellg()) + 8 <= riff_end) {
    char id[4]{};
    in.read(id, 4);
    if (!in) break;
    std::uint32_t size = read_u32(in);
    std::uint64_t const payload = static_cast<std::uint64_t>(in.tellg());
    if (!in || size > riff_end - payload) return false;
    std::uint64_t const next = payload + size + (size & 1u);
    if (next > riff_end) return false;
    if (std::memcmp(id, "fmt ", 4) == 0) {
      if (size < 16) return false;
      auto const format = read_u16(in);
      auto const channels = read_u16(in);
      auto const rate = read_u32(in);
      auto const byte_rate = read_u32(in);
      auto const alignment = read_u16(in);
      auto const bits = read_u16(in);
      pcm = format == 1 && channels == 1 && rate == 12000 && byte_rate == 24000 &&
            alignment == 2 && bits == 16;
      if (!in || !pcm) return false;
    } else if (std::memcmp(id, "data", 4) == 0) {
      constexpr std::uint32_t max_bytes = 2 * kJs8NtMax * kJs8RxSampleRate;
      if (!pcm || size == 0 || (size & 1u) || size > max_bytes) return false;
      std::vector<unsigned char> bytes(size);
      in.read(reinterpret_cast<char*>(bytes.data()), size);
      if (!in) return false;
      samples.resize(size / 2);
      for (std::size_t i = 0; i < samples.size(); ++i) {
        auto const word = static_cast<std::uint16_t>(bytes[2 * i]) |
                          (static_cast<std::uint16_t>(bytes[2 * i + 1]) << 8);
        samples[i] = static_cast<std::int16_t>(word);
      }
      return true;
    }
    in.seekg(static_cast<std::streamoff>(next));
  }
  return false;
}

std::vector<int> data_positions() {
  std::vector<int> positions;
  for (int s = 7; s < 36; ++s) positions.push_back(s);
  for (int s = 43; s < 72; ++s) positions.push_back(s);
  return positions;
}

std::vector<int> signal_tones() {
  std::vector<int> tones;
  tones.reserve(58);
  for (int d = 0; d < 58; ++d) tones.push_back((d * 5 + 1) & 7);
  return tones;
}

std::complex<double> signal_value(int sample_index, int tone, double omega, double beta,
                                  double amplitude) {
  return std::polar(amplitude, omega * (sample_index - 39) + beta * (tone - 3.5));
}

ToneObservations synthesize_obs(
    CostasArray const& costas, std::vector<int> const& tones, double omega, double beta,
    double amplitude, double noise_std, std::mt19937& rng) {
  std::normal_distribution<double> gauss(0.0, noise_std);
  ToneObservations obs;
  int data_index = 0;
  for (int s = 0; s < data::kChannelSymbols; ++s) {
    int const block = s / 36;
    int const column = s % 36;
    int active = -1;
    if (column < 7) {
      active = costas[block][column];
    } else {
      active = tones[data_index++];
    }
    for (int k = 0; k < data::kToneRows; ++k) {
      if (k == active) {
        auto const value = signal_value(s, k, omega, beta, amplitude);
        obs[s][k] = std::complex<double>(value.real(), value.imag()) +
                    std::complex<double>(gauss(rng), gauss(rng));
      } else {
        obs[s][k] = std::complex<double>(gauss(rng), gauss(rng));
      }
    }
  }
  return obs;
}

std::vector<int> decode_tones(BitLikelihoods const& llrs) {
  std::vector<int> tones(58);
  for (int d = 0; d < 58; ++d) {
    int tone = 0;
    for (int bit = 0; bit < 3; ++bit)
      if (llrs[3 * d + bit] >= 0.0f) tone |= (1 << (2 - bit));
    tones[d] = tone;
  }
  return tones;
}

bool signal_test() {
  CostasArray const& costas = protocol::costas(CostasType::Modified);
  std::vector<int> tones = signal_tones();
  std::mt19937 rng(0x7a3fc1d9u);
  ToneObservations obs = synthesize_obs(costas, tones, /*omega*/0.01, /*beta*/0.3,
                                        /*amplitude*/200.0, /*noise_std*/1.0, rng);

  auto model = estimate_disturbance(obs, costas, /*whiten*/true);
  if (!model || model->amplitude <= 0.0) {
    std::fprintf(stderr, "signal_test: invalid or empty DisturbanceModel\n");
    return false;
  }

  auto channel = estimate_pilot_channel(obs, costas, *model);
  if (!channel) { std::fprintf(stderr, "signal_test: estimate_pilot_channel returned empty\n"); return false; }
  if (!(channel->validation_error < 0.1)) {
    std::fprintf(stderr, "signal_test: validation_error too large: %g\n", channel->validation_error);
    return false;
  }

  auto llrs = make_soft_likelihoods(obs, *model, &*channel);
  if (!llrs) { std::fprintf(stderr, "signal_test: make_soft_likelihoods returned empty\n"); return false; }
  std::vector<int> recovered = decode_tones(*llrs);
  int correct = 0;
  for (int d = 0; d < 58; ++d) if (recovered[d] == tones[d]) ++correct;
  if (correct < 58) { std::fprintf(stderr, "signal_test: only %d/58 symbols recovered\n", correct); return false; }
  if (!(std::abs(model->amplitude - 200.0) < 80.0)) {
    std::fprintf(stderr, "signal_test: amplitude %g off from 200\n", model->amplitude);
    return false;
  }
  std::printf("  signal_test: %d/58 symbols recovered, validation_error=%.4f, amplitude=%.1f\n",
              correct, channel->validation_error, model->amplitude);
  return true;
}

bool noise_only_test() {
  CostasArray const& costas = protocol::costas(CostasType::Modified);
  std::mt19937 rng(0x1001);
  ToneObservations obs;
  std::normal_distribution<double> gauss(0.0, 1.0);
  for (int s = 0; s < data::kChannelSymbols; ++s)
    for (int k = 0; k < data::kToneRows; ++k)
      obs[s][k] = std::complex<double>(gauss(rng), gauss(rng));
  if (estimate_disturbance(obs, costas, true).has_value()) {
    std::fprintf(stderr, "noise_only_test: expected empty DisturbanceModel\n");
    return false;
  }
  if (estimate_pilot_channel(obs, costas, DisturbanceModel{}).has_value()) {
    std::fprintf(stderr, "noise_only_test: expected empty PilotChannel\n");
    return false;
  }
  std::printf("  noise_only_test: empty model and channel as expected\n");
  return true;
}

bool no_pilot_nan_test() {
  CostasArray const& costas = protocol::costas(CostasType::Modified);
  std::vector<int> tones = signal_tones();
  std::mt19937 rng(0x55aa);
  ToneObservations obs = synthesize_obs(costas, tones, 0.01, 0.3, 200.0, 1.0, rng);
  for (int s = 0; s < data::kChannelSymbols; ++s) {
    int const block = s / 36;
    int const column = s % 36;
    if (column < 7)
      obs[s][costas[block][column]] = std::numeric_limits<std::complex<float>>::quiet_NaN();
  }
  if (estimate_disturbance(obs, costas, true).has_value()) {
    std::fprintf(stderr, "no_pilot_nan_test: expected empty DisturbanceModel\n");
    return false;
  }
  if (make_soft_likelihoods(obs, DisturbanceModel{}).has_value()) {
    std::fprintf(stderr, "no_pilot_nan_test: expected empty BitLikelihoods\n");
    return false;
  }
  std::printf("  no_pilot_nan_test: empty model and LLRs as expected\n");
  return true;
}

bool large_phase_gate_test() {
  CostasArray const& costas = protocol::costas(CostasType::Modified);
  std::vector<int> tones = signal_tones();
  std::mt19937 rng(0x99ee);
  ToneObservations obs = synthesize_obs(costas, tones, /*omega*/0.2, /*beta*/0.3, 200.0, 140.0, rng);
  auto model = estimate_disturbance(obs, costas, true);
  if (!model) { std::fprintf(stderr, "large_phase_gate_test: empty model\n"); return false; }
  if (estimate_pilot_channel(obs, costas, *model).has_value()) {
    std::fprintf(stderr, "large_phase_gate_test: expected empty PilotChannel for out-of-range phase\n");
    return false;
  }
  auto llrs = make_soft_likelihoods(obs, *model, nullptr);
  if (!llrs) { std::fprintf(stderr, "large_phase_gate_test: expected Rician fallback LLRs\n"); return false; }
  std::printf("  large_phase_gate_test: pilot fit rejected, Rician fallback produced %zu LLRs\n", llrs->size());
  return true;
}

std::size_t count_frames(std::vector<std::int16_t> const& samples, bool coherent_likelihood) {
  DecodeState state;
  state.params.symbol_tracking = SymbolTracking::Disabled;
  state.params.tracked_sic = false;
  state.params.deep_sync = false;
  state.params.coherent_likelihood = coherent_likelihood;
  state.samples.assign(static_cast<std::size_t>(kJs8NtMax * kJs8RxSampleRate), 0);
  std::copy(samples.begin(), samples.end(), state.samples.begin());
  state.params.nfa = 200;
  state.params.nfb = 2500;
  state.params.nfqso = 1500;
  state.params.utc = 0;
  state.params.nsubmodes = 1;
  state.params.kposA = 0;
  state.params.kszA = static_cast<int>(std::min<std::size_t>(samples.size(), 15 * kJs8RxSampleRate));
  std::size_t count = 0;
  legacy_decode(state, [&](events::Variant const& event) {
    if (std::get_if<events::Decoded>(&event)) ++count;
  });
  return count;
}

int run_decode_regression(int argc, char** argv) {
  std::vector<std::string> wav_files;
  for (int i = 1; i < argc; ++i) {
    std::string_view arg(argv[i]);
    if (arg.size() >= 4 && arg.substr(arg.size() - 4) == ".wav") wav_files.push_back(argv[i]);
  }
  if (wav_files.empty()) {
    std::printf("coherent_likelihood_test: no .wav files given; skipping decode regression\n");
    return 0;
  }
  int failures = 0;
  for (auto const& path : wav_files) {
    std::vector<std::int16_t> samples;
    if (!load_wav(path, samples)) { std::fprintf(stderr, "cannot read %s\n", path.c_str()); ++failures; continue; }
    std::size_t off = count_frames(samples, /*coherent_likelihood*/false);
    std::size_t on = count_frames(samples, /*coherent_likelihood*/true);
    std::printf("  DECODE\t%s\toff=%zu\ton=%zu\n", std::filesystem::path(path).filename().string().c_str(), off, on);
    if (on < off) { std::fprintf(stderr, "%s: coherent-on decoded fewer frames than off\n", path.c_str()); ++failures; }
  }
  return failures;
}

}  // namespace

int main(int argc, char** argv) {
  std::printf("coherent_likelihood_test\n");
  int failures = 0;

  std::printf("[unit]\n");
  failures += signal_test() ? 0 : 1;
  failures += noise_only_test() ? 0 : 1;
  failures += no_pilot_nan_test() ? 0 : 1;
  failures += large_phase_gate_test() ? 0 : 1;

  std::printf("[decode-regression]\n");
  failures += run_decode_regression(argc, argv);

  std::printf("%s\n", failures ? "FAIL" : "PASS");
  return failures ? 1 : 0;
}

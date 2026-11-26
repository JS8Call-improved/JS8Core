#include <algorithm>
#include <array>
#include <filesystem>
#include <cstdint>
#include <cstdio>
#include <chrono>
#include <cstring>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "js8core/decoder.hpp"
#include "js8core/engine.hpp"
#include "js8core/protocol/constants.hpp"

using namespace js8core;

namespace {

std::uint32_t read_u32(std::ifstream& in) {
  std::uint8_t bytes[4]{};
  in.read(reinterpret_cast<char*>(bytes), 4);
  return static_cast<std::uint32_t>(bytes[0]) |
         (static_cast<std::uint32_t>(bytes[1]) << 8) |
         (static_cast<std::uint32_t>(bytes[2]) << 16) |
         (static_cast<std::uint32_t>(bytes[3]) << 24);
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
      pcm = format == 1 && channels == 1 && rate == 12000 &&
            byte_rate == 24000 && alignment == 2 && bits == 16;
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

// Print binary payload bytes as a stable, lossless ASCII key. Text may
// contain characters unsuitable for whitespace-delimited result files.
std::string hex_text(std::string const& payload) {
  constexpr char digits[] = "0123456789abcdef";
  std::string hex;
  hex.reserve(payload.size() * 2);
  for (unsigned char const byte : payload) {
    hex.push_back(digits[byte >> 4]);
    hex.push_back(digits[byte & 0x0f]);
  }
  return hex;
}

bool same_frame(events::Decoded const& a, events::Decoded const& b) {
  return a.data == b.data && a.type == b.type && a.utc == b.utc && a.mode == b.mode &&
      a.snr == b.snr && a.frequency == b.frequency && a.xdt == b.xdt && a.quality == b.quality &&
      a.drift_ms == b.drift_ms && a.capture_drift_ms == b.capture_drift_ms &&
      a.tracking_used == b.tracking_used && a.after_tracked_sic == b.after_tracked_sic;
}

}  // namespace

int main(int argc, char** argv) {
  bool records = false, wrap = false, valid = true, tracked_sic = false, deep_sync = false;
  bool adaptive_sic = false;
  SymbolTracking tracking = SymbolTracking::Disabled;
  int mode_arg = 1;
  while (mode_arg < argc && std::string_view(argv[mode_arg]).starts_with("--") && valid) {
    std::string_view option(argv[mode_arg++]);
    if (option == "--records") records = true;
    else if (option == "--wrap") wrap = true;
    else if (option == "--tracked-sic") tracked_sic = true;
    else if (option == "--adaptive-sic") adaptive_sic = true;
    else if (option == "--deep-sync") deep_sync = true;
    else if (option == "--tracking" && mode_arg < argc) {
      std::string_view value(argv[mode_arg++]);
      if (value == "disabled") tracking = SymbolTracking::Disabled;
      else if (value == "frequency") tracking = SymbolTracking::Frequency;
      else if (value == "timing") tracking = SymbolTracking::Timing;
      else if (value == "both") tracking = SymbolTracking::Both;
      else valid = false;
    } else valid = false;
  }
  if (!valid || argc < mode_arg + 2 ||
      (std::string_view(argv[mode_arg]) != "A" && std::string_view(argv[mode_arg]) != "a" &&
       std::string_view(argv[mode_arg]) != "E" && std::string_view(argv[mode_arg]) != "e")) {
    std::fprintf(stderr,
                 "usage: %s [--records] [--tracking disabled|frequency|timing|both] "
                 "[--wrap] [--tracked-sic] [--adaptive-sic] [--deep-sync] "
                 "<mode A|E> <file.wav>...\n", argv[0]);
    return 2;
  }

  char const mode = argv[mode_arg][0];
  int failures = 0;

  for (int arg = mode_arg + 1; arg < argc; ++arg) {
    std::vector<std::int16_t> samples;
    if (!load_wav(argv[arg], samples)) {
      std::fprintf(stderr, "cannot read %s\n", argv[arg]);
      ++failures;
      continue;
    }

    DecodeState state;
    // Preserve the recorder/comparator's disabled baseline regardless of the
    // production defaults. Explicit CLI switches select the candidate path.
    state.params.symbol_tracking = SymbolTracking::Disabled;
    state.params.tracked_sic = false;
    state.params.adaptive_sic = false;
    state.params.deep_sync = false;
    state.samples.assign(static_cast<std::size_t>(kJs8NtMax * kJs8RxSampleRate), 0);
    // Reject oversized input rather than writing beyond the 60-second ring.
    if (samples.size() > state.samples.size()) {
      std::fprintf(stderr, "WAV longer than decoder ring: %s\n", argv[arg]);
      ++failures;
      continue;
    }
    std::copy(samples.begin(), samples.end(), state.samples.begin());
    state.params.nfa = 200;
    state.params.nfb = 2500;
    state.params.nfqso = 1500;
    state.params.utc = 0;

    int const window_start = wrap ? (kJs8NtMax - 1) * kJs8RxSampleRate : 0;
    if (wrap) std::rotate(state.samples.begin(), state.samples.begin() + kJs8RxSampleRate,
                           state.samples.end());
    if (mode == 'A' || mode == 'a') {
      state.params.nsubmodes = 1 << 0;
      state.params.kposA = window_start;
      state.params.kszA = static_cast<int>(std::min<std::size_t>(samples.size(), 15 * kJs8RxSampleRate));
    } else if (mode == 'E' || mode == 'e') {
      state.params.nsubmodes = 1 << 3;
      state.params.kposE = window_start;
      state.params.kszE = static_cast<int>(std::min<std::size_t>(samples.size(), 30 * kJs8RxSampleRate));
    } else {
      std::fprintf(stderr, "unknown mode %c\n", mode);
      return 2;
    }

    const auto basename = std::filesystem::path(argv[arg]).filename().string();
    std::size_t cancellations = 0;
    auto run = [&](std::vector<events::Decoded>& results) {
      cancellations = 0;
      auto const start = std::chrono::steady_clock::now();
      legacy_decode(state, [&](events::Variant const& event) {
        if (auto const* d = std::get_if<events::Decoded>(&event)) results.push_back(*d);
        if (auto const* f = std::get_if<events::DecodeFinished>(&event)) cancellations = f->tracked_cancellations;
      });
      return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    };
    std::vector<events::Decoded> baseline;
    double const baseline_ms = run(baseline);
    std::vector<events::Decoded> results = baseline;
    double enabled_ms = baseline_ms;
    if (tracking != SymbolTracking::Disabled) {
      state.params.symbol_tracking = tracking;
      results.clear();
      enabled_ms = run(results);
      if (tracked_sic) {
        auto const tracking_only = results;
        state.params.tracked_sic = true;
        results.clear();
        enabled_ms = run(results);
        if (results.size() < tracking_only.size() ||
            !std::equal(tracking_only.begin(), tracking_only.end(), results.begin(), same_frame)) {
          std::fprintf(stderr, "%s: tracked SIC changed the no-cancellation results\n", basename.c_str());
          ++failures;
        }
      }
      bool preserved = results.size() >= baseline.size();
      for (std::size_t i = 0; preserved && i < baseline.size(); ++i) {
        preserved = same_frame(baseline[i], results[i]);
      }
      for (std::size_t i = baseline.size(); preserved && i < results.size(); ++i) {
        auto const& d = results[i];
        preserved = d.tracking_used && std::none_of(results.begin(), results.begin() + i,
            [&](auto const& other) { return d.type == other.type && d.data == other.data; });
        if (d.after_tracked_sic && !cancellations) preserved = false;
      }
      if (!preserved) {
        std::fprintf(stderr, "%s: tracking changed baseline results or appended invalid provenance\n", basename.c_str());
        ++failures;
      }
      auto const tracked = std::count_if(results.begin(), results.end(), [](auto const& d) {
        return d.tracking_used;
      });
      std::fprintf(stderr, "PAIR\t%s\t%zu\t%zu\t%td\t%.3f\t%.3f\n", basename.c_str(),
                   baseline.size(), results.size(), tracked, baseline_ms, enabled_ms);
    }
    if (cancellations > 2 || ((tracking != SymbolTracking::Both || !tracked_sic) && cancellations)) {
      std::fprintf(stderr, "%s: unexpected tracked cancellation count\n", basename.c_str());
      ++failures;
    }
    if (tracked_sic) std::fprintf(stderr, "SIC\t%s\t%zu\n", basename.c_str(), cancellations);
    if (adaptive_sic) {
      auto const previous_count = results.size();
      state.params.adaptive_sic = true;
      results.clear();
      double const adaptive_ms = run(results);
      std::fprintf(stderr, "ADAPTIVE\t%s\t%zu\t%zu\t%.3f\t%.3f\n",
                   basename.c_str(), previous_count, results.size(), enabled_ms, adaptive_ms);
      enabled_ms = adaptive_ms;
    }
    if (deep_sync) {
      auto const previous_cancellations = cancellations;
      auto const before = std::move(results);
      results.clear();
      state.params.deep_sync = true;
      double const deep_ms = run(results);
      bool preserved = cancellations == previous_cancellations && results.size() >= before.size() &&
          std::equal(before.begin(), before.end(), results.begin(), same_frame);
      for (std::size_t i = before.size(); preserved && i < results.size(); ++i) {
        auto const& d = results[i];
        preserved = !d.tracking_used && !d.after_tracked_sic &&
            std::none_of(results.begin(), results.begin() + i, [&](auto const& prior) {
              return d.type == prior.type && d.data == prior.data;
            });
      }
      if (!preserved) {
        std::fprintf(stderr, "%s: deep sync changed existing results or emitted a duplicate\n",
                     basename.c_str());
        ++failures;
      }
      std::fprintf(stderr, "DEEP\t%s\t%zu\t%zu\t%.3f\t%.3f\n",
                   basename.c_str(), before.size(), results.size(), enabled_ms, deep_ms);
    }
    int const count = static_cast<int>(baseline.size());
    for (auto const& decoded : results) {
      if (records) {
        const auto hex = hex_text(decoded.data);
        // Common desktop/JS8Core record layout:
        // file, frame type, mode, SNR dB, audio Hz, DT seconds,
        // quality, hexadecimal UTF-8 payload.
        std::printf("RECORD\t%s\t%d\t%d\t%d\t%.4f\t%.4f\t%.4f\t%s\n",
                    basename.c_str(), decoded.type, decoded.mode, decoded.snr,
                    decoded.frequency, decoded.xdt, decoded.quality, hex.c_str());
      } else {
        std::printf("  %s\n", decoded.data.c_str());
      }
    }

    if (records) {
      std::fprintf(stderr, "%s: %zu JS8Core decodes\n", basename.c_str(), results.size());
    } else {
      std::printf("%s: %zu decodes\n", argv[arg], results.size());
    }
    // The full-record A/B comparator validates exact identities and numeric
    // metadata when --records is requested. Keep native count-only gates
    // separately, using the expected JS8Core counts for the pinned fixtures.
    if (records) continue;

    // A_1_4 and A_2_5 are the same recording; JS8Core's last-try decoder
    // recovers one more frame than the pinned desktop decoder on each.
    struct Known {
      char const* name;
      int expected;
    };
    static constexpr std::array<Known, 9> kReference = {{
        {"A_1_4.wav", 5}, {"A_2_1.wav", 0}, {"A_2_3.wav", 3},
        {"A_2_5.wav", 5}, {"A_2_6.wav", 4}, {"A_2_9.wav", 8},
        {"A_3_3.wav", 3}, {"E_1_1.wav", 1}, {"E_2_1.wav", 1},
    }};
    for (auto const& known : kReference) {
      if (basename != known.name) continue;
      if (count != known.expected) {
        std::fprintf(stderr, "%s: expected reference %d decodes, received %d\n",
                     argv[arg], known.expected, count);
        ++failures;
      }
      break;
    }
  }

  return failures == 0 ? 0 : 1;
}

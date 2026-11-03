#include <algorithm>
#include <array>
#include <filesystem>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
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
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;

  char riff[4]{};
  in.read(riff, 4);
  if (std::memcmp(riff, "RIFF", 4) != 0) return false;
  read_u32(in);
  char wave[4]{};
  in.read(wave, 4);
  if (std::memcmp(wave, "WAVE", 4) != 0) return false;

  std::uint16_t channels = 0;
  std::uint16_t bits = 0;
  std::uint32_t rate = 0;

  while (in) {
    char id[4]{};
    in.read(id, 4);
    if (!in) break;
    std::uint32_t size = read_u32(in);

    if (std::memcmp(id, "fmt ", 4) == 0) {
      read_u16(in);  // format
      channels = read_u16(in);
      rate = read_u32(in);
      read_u32(in);  // byte rate
      read_u16(in);  // block align
      bits = read_u16(in);
      in.seekg(static_cast<std::streamoff>(size) - 16, std::ios::cur);
    } else if (std::memcmp(id, "data", 4) == 0) {
      if (channels != 1 || bits != 16 || rate != 12000) return false;
      samples.resize(size / 2);
      in.read(reinterpret_cast<char*>(samples.data()), size);
      return true;
    } else {
      in.seekg(size, std::ios::cur);
    }
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

}  // namespace

int main(int argc, char** argv) {
  const bool records = argc > 1 && std::strcmp(argv[1], "--records") == 0;
  const int mode_arg = records ? 2 : 1;
  if (argc < mode_arg + 2) {
    std::fprintf(stderr,
                 "usage: %s [--records] <mode A|E> <file.wav>...\n", argv[0]);
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

    int count = 0;
    if (mode == 'A' || mode == 'a') {
      state.params.nsubmodes = 1 << 0;
      state.params.kposA = 0;
      state.params.kszA = 15 * kJs8RxSampleRate;
    } else if (mode == 'E' || mode == 'e') {
      state.params.nsubmodes = 1 << 3;
      state.params.kposE = 0;
      state.params.kszE = 30 * kJs8RxSampleRate;
    } else {
      std::fprintf(stderr, "unknown mode %c\n", mode);
      return 2;
    }

    const auto basename = std::filesystem::path(argv[arg]).filename().string();
    legacy_decode(state, [&](events::Variant const& ev) {
      if (auto const* decoded = std::get_if<events::Decoded>(&ev)) {
        ++count;
        if (records) {
          const auto hex = hex_text(decoded->data);
          // Common desktop/JS8Core record layout:
          // file, frame type, mode, SNR dB, audio Hz, DT seconds,
          // quality, hexadecimal UTF-8 payload.
          std::printf("RECORD\t%s\t%d\t%d\t%d\t%.4f\t%.4f\t%.4f\t%s\n",
                      basename.c_str(), decoded->type, decoded->mode, decoded->snr,
                      decoded->frequency, decoded->xdt, decoded->quality,
                      hex.c_str());
        } else {
          std::printf("  %s\n", decoded->data.c_str());
        }
      }
    });

    if (records) {
      std::fprintf(stderr, "%s: %d JS8Core decodes\n", basename.c_str(), count);
    } else {
      std::printf("%s: %d decodes\n", argv[arg], count);
    }
    // The full-record A/B comparator validates exact identities and numeric
    // metadata when --records is requested. Keep native count-only gates
    // separately, using counts recorded from the pinned desktop revision.
    if (records) continue;

    // Expected desktop counts with this search band and mode configuration.
    // Filename suffixes reflect a different historical decoder invocation.
    struct Known {
      char const* name;
      int expected;
    };
    static constexpr std::array<Known, 9> kReference = {{
        {"A_1_4.wav", 4}, {"A_2_1.wav", 0}, {"A_2_3.wav", 3},
        {"A_2_5.wav", 4}, {"A_2_6.wav", 4}, {"A_2_9.wav", 8},
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

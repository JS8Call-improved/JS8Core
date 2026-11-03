// Test-only adapter for an external, pinned desktop JS8Call checkout.
// It does not distribute or relicense desktop decoder implementation code.
// CI builds this executable separately from the MIT-licensed JS8Core library.
#include <QCoreApplication>
#include <QLoggingCategory>
#include <QDebug>
#include <QByteArrayView>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "JS8_Include/commons.h"
#include "JS8_Mode/JS8.h"

// Production desktop decoder globals, as required by its public interface.
struct dec_data dec_data;
struct specData specData;
std::mutex fftw_mutex;
Q_LOGGING_CATEGORY(decoder_js8, "decoder.js8", QtWarningMsg)

// Qt versions before 6.8 do not expose an unambiguous string_view QDebug
// insertion operator. This local test-build adapter does not change or patch
// the desktop source being measured.
inline QDebug& operator<<(QDebug& stream, std::string_view text) {
  return stream << QByteArrayView(text.data(), static_cast<qsizetype>(text.size()));
}

// Included only in a separate reference executable built from the external
// checkout. No desktop decoder source belongs to the JS8Core deliverable.
#include "JS8_Mode/JS8.cpp"

namespace {

std::uint16_t read16(std::ifstream& stream) {
  unsigned char bytes[2];
  if (!stream.read(reinterpret_cast<char*>(bytes), 2))
    throw std::runtime_error("truncated WAV field");
  return std::uint16_t(bytes[0]) | (std::uint16_t(bytes[1]) << 8);
}

std::uint32_t read32(std::ifstream& stream) {
  unsigned char bytes[4];
  if (!stream.read(reinterpret_cast<char*>(bytes), 4))
    throw std::runtime_error("truncated WAV length");
  return std::uint32_t(bytes[0]) | (std::uint32_t(bytes[1]) << 8) |
         (std::uint32_t(bytes[2]) << 16) | (std::uint32_t(bytes[3]) << 24);
}

std::vector<std::int16_t> read_pcm(std::string const& filename) {
  std::ifstream in(filename, std::ios::binary);
  if (!in) throw std::runtime_error("cannot open WAV: " + filename);
  char magic[4];
  if (!in.read(magic, 4) || std::memcmp(magic, "RIFF", 4))
    throw std::runtime_error("not RIFF: " + filename);
  (void)read32(in);
  if (!in.read(magic, 4) || std::memcmp(magic, "WAVE", 4))
    throw std::runtime_error("not WAVE: " + filename);

  bool format_ok = false;
  while (in) {
    if (!in.read(magic, 4)) break;
    const std::uint32_t chunk = read32(in);
    if (!std::memcmp(magic, "fmt ", 4)) {
      if (chunk < 16) throw std::runtime_error("short WAV format");
      const auto encoding = read16(in);
      const auto channels = read16(in);
      const auto sample_rate = read32(in);
      (void)read32(in);
      (void)read16(in);
      const auto bits = read16(in);
      format_ok = encoding == 1 && channels == 1 &&
                  sample_rate == 12000 && bits == 16;
      in.seekg(static_cast<std::streamoff>(chunk - 16), std::ios::cur);
    } else if (!std::memcmp(magic, "data", 4)) {
      if (!format_ok || chunk % 2) {
        throw std::runtime_error("expected 12-kHz mono PCM16: " + filename);
      }
      std::vector<std::int16_t> pcm(chunk / 2);
      if (!in.read(reinterpret_cast<char*>(pcm.data()), chunk))
        throw std::runtime_error("truncated PCM data: " + filename);
      return pcm;
    } else {
      in.seekg(static_cast<std::streamoff>(chunk), std::ios::cur);
    }
    if (chunk & 1) in.seekg(1, std::ios::cur);
  }
  throw std::runtime_error("missing WAV PCM chunk: " + filename);
}

std::string hex_payload(std::string const& payload) {
  static constexpr char alphabet[] = "0123456789abcdef";
  std::string result;
  result.reserve(payload.size() * 2);
  for (const unsigned char c : payload) {
    result.push_back(alphabet[c >> 4]);
    result.push_back(alphabet[c & 15]);
  }
  return result;
}

std::string file_basename(std::string const& path) {
  auto const slash = path.find_last_of("/\\");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

}  // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  if (argc < 3 || (argv[1][0] != 'A' && argv[1][0] != 'E')) {
    std::fprintf(stderr, "usage: %s <A|E> <WAV>...\n", argv[0]);
    return 2;
  }

  try {
    const char mode = argv[1][0];
    // Reuse FFT planning and decoder state over multiple WAV files.
    std::unique_ptr<DecodeMode<ModeA>> decoder_a;
    std::unique_ptr<DecodeMode<ModeE>> decoder_e;
    if (mode == 'A') decoder_a = std::make_unique<DecodeMode<ModeA>>();
    else decoder_e = std::make_unique<DecodeMode<ModeE>>();

    for (int arg = 2; arg < argc; ++arg) {
      auto pcm = read_pcm(argv[arg]);
      if (pcm.size() > JS8_RX_SAMPLE_SIZE)
        throw std::runtime_error("WAV exceeds the 60-second ring");
      std::fill(std::begin(dec_data.d2), std::end(dec_data.d2), 0);
      std::copy(pcm.begin(), pcm.end(), std::begin(dec_data.d2));
      dec_data.params.nfa = 200;
      dec_data.params.nfb = 2500;
      dec_data.params.nfqso = 1500;
      dec_data.params.newdat = true;
      dec_data.params.syncStats = false;
      dec_data.params.nutc = 0;
      const auto file = file_basename(argv[arg]);
      int count = 0;
      auto on_event = [&](JS8::Event::Variant const& ev) {
        if (const auto* decoded = std::get_if<JS8::Event::Decoded>(&ev)) {
          ++count;
          auto const hex = hex_payload(decoded->data);
          std::printf("RECORD\t%s\t%d\t%d\t%d\t%.4f\t%.4f\t%.4f\t%s\n",
                      file.c_str(), decoded->type, decoded->mode, decoded->snr,
                      decoded->frequency, decoded->xdt, decoded->quality,
                      hex.c_str());
        }
      };
      if (mode == 'A') {
        (*decoder_a)(dec_data, 0, 15 * JS8_RX_SAMPLE_RATE, on_event);
      } else {
        (*decoder_e)(dec_data, 0, 30 * JS8_RX_SAMPLE_RATE, on_event);
      }
      std::fprintf(stderr, "%s: %d desktop decodes\n", file.c_str(), count);
    }
  } catch (std::exception const& error) {
    std::fprintf(stderr, "desktop reference failure: %s\n", error.what());
    return 1;
  }
  return 0;
}

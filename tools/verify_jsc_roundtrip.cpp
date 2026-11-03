// JSC compatibility checks for canonical word/index assignments and the
// independently regenerated dictionary search index.
//
// compress()/decompress() never compute a message CRC (only full-message
// framing does), so this verifier needs only the JSC algorithm + the tables.
// It exercises round-trip correctness of the reproduced JSC data against the
// JS8 41-character alphabet.

#include "js8core/protocol/jsc.hpp"
#include "jsc_data.hpp"  // declares JSC_SIZE / the table externs

#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <vector>

using namespace js8core::protocol::jsc;

static std::vector<bool> concat(std::vector<CodewordPair> const& pairs) {
  std::vector<bool> bits;
  for (auto const& [pw, n] : pairs) bits.insert(bits.end(), pw.begin(), pw.end());
  return bits;
}

int main() {
  std::cout << "JSC_SIZE = " << JSC_SIZE << " (expected 262144)\n";
  if (JSC_SIZE != 262144u) {
    std::cerr << "FAIL: JSC_SIZE mismatch\n";
    return 1;
  }

  std::string const& alphabet = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ+-./?";

  // 1) single-character round-trip through the table.
  int single_ok = 0, single_bad = 0;
  for (char c : alphabet) {
    std::string const in(1, c);
    std::string out = decompress(concat(compress(in)));
    if (out == in) single_ok++;
    else { single_bad++; std::cerr << "single-char FAIL '" << c << "' -> '" << out << "'\n"; }
  }
  std::cout << "single-char round-trip: " << single_ok << "/" << alphabet.size() << " ok\n";

  // 2) random multi-character round-trip.
  std::mt19937 rng(20261031u);
  int rt_ok = 0, rt_bad = 0;
  for (int iter = 0; iter < 30000; ++iter) {
    std::size_t n = static_cast<std::size_t>(rng() % 24) + 1u;
    std::string s;
    for (std::size_t i = 0; i < n; ++i) s += alphabet[rng() % alphabet.size()];
    std::string out = decompress(concat(compress(s)));
    if (out == s) rt_ok++;
    else { rt_bad++; std::cerr << "round-trip FAIL len=" << n << " -> '" << out << "'\n"; }
  }
  std::cout << "multi-char round-trip: " << rt_ok << "/" << (rt_ok + rt_bad) << " ok\n";

  // Golden compatibility digest, captured by comparing the historical
  // two-table lookup with the derived index over 4,004 exact/prefix inputs.
  // Values are on-air dictionary IDs (+1; zero means no match).
  std::uint32_t digest = 2166136261u;
  auto mix = [&digest](std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
      digest ^= static_cast<std::uint8_t>(value >> shift);
      digest *= 16777619u;
    }
  };
  int parity_compared = 0;
  for (std::uint32_t i = 0; i < JSC_SIZE; i += 131) {
    const auto& entry = JSC_MAP[i];
    const std::string candidate(entry.str, static_cast<std::size_t>(entry.size));
    for (auto const& input : {candidate, candidate + "~" + std::to_string(i)}) {
      if (input.find('\0') != std::string::npos) continue;
      bool found = false;
      const auto index = lookup(input.c_str(), &found);
      auto const code = found ? index + 1u : 0u;
      mix(code);
      mix(code);
      ++parity_compared;
    }
  }
  if (parity_compared != 4004 || digest != 0x9e32712du) {
    std::cerr << "JSC canonical lookup digest changed: " << std::hex << digest
              << std::dec << " over " << parity_compared << " inputs\n";
    return 1;
  }
  std::cout << "JSC generated index parity digest PASS\n";

  // Malformed continuation groups must not read beyond base[8].
  Codeword malformed;
  for (int i = 0; i < 8; ++i)
    malformed.insert(malformed.end(), {true, true, true, true});
  malformed.insert(malformed.end(), {false, false, false, false});
  const bool malformed_rejected = decompress(malformed).empty();
  if (!malformed_rejected) std::cerr << "malformed JSC continuation sequence accepted\n";

  bool ok = single_bad == 0 && rt_bad == 0 && malformed_rejected;
  std::cout << (ok ? "PASS" : "FAIL") << "\n";
  return ok ? 0 : 1;
}

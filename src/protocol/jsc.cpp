#include "js8core/protocol/jsc.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <numeric>
#include <string_view>
#include <string>
#include <vector>

#include "jsc_data.hpp"

// The JSC codec maps short text fragments to variable-length bit codes. Every
// code is a sequence of 4-bit "continuation" groups followed by one 5-bit
// terminal group. The terminal group holds the low three bits of the symbol
// index plus a flag that marks a following inter-word space.
//
// The indexed word corpus in jsc_map.cpp.inc is the canonical on-air JSC
// dictionary. The lookup index is derived at runtime from that corpus;
// no copied sorted lookup table or prefix index is required.

namespace js8core::protocol::jsc {
namespace {

constexpr std::uint32_t kGroupBits = 4;
constexpr std::uint32_t kTerminalValues = 7;  // indices below this end a code
constexpr std::uint32_t kContinuationValues = (1u << kGroupBits) - kTerminalValues;
constexpr std::uint32_t kMaxContinuationGroups = 7;

Codeword to_bits(std::uint32_t value, unsigned width) {
  Codeword bits(width, false);
  for (unsigned i = 0; i < width; ++i) {
    bits[width - 1 - i] = (value >> i) & 1u;
  }
  return bits;
}

void append(Codeword& dst, Codeword const& src) {
  dst.insert(dst.end(), src.begin(), src.end());
}

// base[k] is the first symbol index whose code has k continuation groups.
std::uint32_t group_base(std::uint32_t k) {
  std::uint32_t base = 0;
  std::uint32_t span = kTerminalValues;
  for (std::uint32_t i = 0; i < k; ++i) {
    base += span;
    span *= kContinuationValues;
  }
  return base;
}

Codeword build_code(std::uint32_t index, bool trailing_space) {
  // Continuation groups are emitted least-significant first, then reversed so
  // the most-significant group leads the code.
  Codeword continuation;
  std::uint32_t rest = index / kTerminalValues;
  while (rest > 0) {
    --rest;
    append(continuation, to_bits(rest % kContinuationValues + kTerminalValues,
                                 kGroupBits));
    rest /= kContinuationValues;
  }

  Codeword code;
  for (std::size_t i = continuation.size(); i >= kGroupBits; i -= kGroupBits) {
    code.insert(code.end(), continuation.begin() + (i - kGroupBits),
                continuation.begin() + i);
  }

  std::uint32_t terminal =
      (index % kTerminalValues) * 2u + (trailing_space ? 1u : 0u);
  append(code, to_bits(terminal, kGroupBits + 1));
  return code;
}

// Parses the 4/5-bit stream into symbol groups and records which groups carry
// an inter-word separator flag.
struct Parsed {
  std::vector<std::uint32_t> groups;
  std::vector<std::size_t> separators;
};

Parsed parse_groups(Codeword const& bits) {
  Parsed parsed;
  std::size_t pos = 0;
  while (pos + kGroupBits <= bits.size()) {
    std::uint32_t group = 0;
    for (std::uint32_t i = 0; i < kGroupBits; ++i) {
      group = (group << 1) | (bits[pos + i] ? 1u : 0u);
    }
    parsed.groups.push_back(group);
    pos += kGroupBits;

    if (group < kTerminalValues) {
      if (pos < bits.size() && bits[pos]) {
        parsed.separators.push_back(parsed.groups.size() - 1);
      }
      ++pos;  // separator flag bit, present even when zero
    }
  }
  return parsed;
}

}  // namespace

Codeword codeword(std::uint32_t index, bool separate, std::uint32_t /*bytesize*/,
                  std::uint32_t /*s*/, std::uint32_t /*c*/) {
  return build_code(index, separate);
}

std::vector<CodewordPair> compress(std::string const& text) {
  std::vector<CodewordPair> encoded;

  std::size_t word_start = 0;
  while (true) {
    const std::size_t space = text.find(' ', word_start);
    const std::size_t word_end =
        (space == std::string::npos) ? text.size() : space;
    const bool last_word = (word_end == text.size());

    std::string word = text.substr(word_start, word_end - word_start);
    bool literal_space = false;
    if (word.empty() && !last_word) {
      // A run of spaces is encoded as a standalone space symbol.
      word = " ";
      literal_space = true;
    }

    while (!word.empty()) {
      bool found = false;
      const std::uint32_t index = lookup(word, &found);
      if (!found) break;

      const JscTuple& entry = JSC_MAP[index];
      word.erase(0, static_cast<std::size_t>(entry.size));

      const bool completes_word = word.empty();
      const bool add_space =
          completes_word && !literal_space && !last_word;

      encoded.emplace_back(
          build_code(index, add_space),
          static_cast<std::uint32_t>(entry.size + (add_space ? 1 : 0)));
    }

    if (last_word) break;
    word_start = word_end + 1;
  }

  return encoded;
}

std::string decompress(Codeword const& bits) {
  const Parsed parsed = parse_groups(bits);
  auto const& groups = parsed.groups;

  std::string text;
  std::size_t next_separator = 0;
  std::size_t pos = 0;

  while (pos < groups.size()) {
    std::uint32_t continuation = 0;
    std::uint32_t count = 0;

    while (pos + count < groups.size() &&
           groups[pos + count] >= kTerminalValues) {
      if (count >= kMaxContinuationGroups) return {};
      continuation =
          continuation * kContinuationValues +
          (groups[pos + count] - kTerminalValues);
      if (continuation >= JSC_SIZE) return {};
      ++count;
    }

    if (pos + count >= groups.size()) return {};

    const std::uint64_t index =
        static_cast<std::uint64_t>(continuation) * kTerminalValues +
        groups[pos + count] + group_base(count);
    if (index >= JSC_SIZE) break;

    text.append(JSC_MAP[index].str, static_cast<std::size_t>(JSC_MAP[index].size));

    if (next_separator < parsed.separators.size() &&
        parsed.separators[next_separator] == pos + count) {
      text.push_back(' ');
      ++next_separator;
    }

    pos += count + 1;
  }

  return text;
}

bool exists(std::string const& w, std::uint32_t* pIndex) {
  bool found = false;
  const std::uint32_t index = lookup(w, &found);
  if (pIndex) *pIndex = index;
  return found && JSC_MAP[index].size == static_cast<int>(w.size());
}

// Rebuild the text -> on-air dictionary-index search table from the one
// immutable index -> text mapping. No second 262,144-element source table is
// needed, and the compressed codeword assignments remain identical.
namespace {
struct DictionaryIndex {
  std::vector<std::uint32_t> by_text;
  std::size_t max_length = 0;

  DictionaryIndex() {
    by_text.resize(JSC_SIZE);
    std::iota(by_text.begin(), by_text.end(), 0u);
    for (auto const index : by_text)
      max_length = std::max(max_length,
          static_cast<std::size_t>(JSC_MAP[index].size));
    std::sort(by_text.begin(), by_text.end(),
              [](std::uint32_t lhs, std::uint32_t rhs) {
      auto const a = std::string_view(
          JSC_MAP[lhs].str, static_cast<std::size_t>(JSC_MAP[lhs].size));
      auto const b = std::string_view(
          JSC_MAP[rhs].str, static_cast<std::size_t>(JSC_MAP[rhs].size));
      return a == b ? lhs < rhs : a < b;
    });
  }
};

DictionaryIndex const& dictionary_index() {
  static const DictionaryIndex index;
  return index;
}
}  // namespace

std::uint32_t lookup(const char* text, bool* ok) {
  if (ok) *ok = false;
  if (!text || !text[0]) return 0;
  auto const input = std::string_view(text);
  auto const& index = dictionary_index();
  for (std::size_t size = std::min(input.size(), index.max_length);
       size > 0; --size) {
    auto const wanted = input.substr(0, size);
    auto const it = std::lower_bound(
        index.by_text.begin(), index.by_text.end(), wanted,
        [](std::uint32_t candidate, std::string_view query) {
          auto const& entry = JSC_MAP[candidate];
          return std::string_view(entry.str,
                   static_cast<std::size_t>(entry.size)) < query;
        });
    if (it == index.by_text.end()) continue;
    auto const& entry = JSC_MAP[*it];
    if (std::string_view(entry.str, static_cast<std::size_t>(entry.size)) == wanted) {
      if (ok) *ok = true;
      return *it;
    }
  }
  return 0;
}

std::uint32_t lookup(std::string const& w, bool* ok) {
  return lookup(w.c_str(), ok);
}

}  // namespace js8core::protocol::jsc

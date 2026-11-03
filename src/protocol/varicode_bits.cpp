#include "js8core/protocol/varicode.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#define CRCPP_INCLUDE_ESOTERIC_CRC_DEFINITIONS
#define CRCPP_USE_CPP11
#include <vendor/CRCpp/CRC.h>

#include "varicode_internal.hpp"
#include "varicode_tables.hpp"

namespace js8core::protocol::varicode {

// ---------------------------------------------------------------------------
// Text escaping and whitespace helpers
// ---------------------------------------------------------------------------

std::string escape(std::string const& text) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(text.size() * 6);
  for (unsigned char byte : text) {
    if (byte < 0x80) {
      out.push_back(static_cast<char>(byte));
      continue;
    }
    out += "\\U";
    out.push_back(kHex[(byte >> 12) & 0x0F]);
    out.push_back(kHex[(byte >> 8) & 0x0F]);
    out.push_back(kHex[(byte >> 4) & 0x0F]);
    out.push_back(kHex[byte & 0x0F]);
  }
  return out;
}

namespace {

int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

std::string unescape(std::string const& text) {
  std::string out;
  out.reserve(text.size());
  std::size_t pos = 0;
  while (pos < text.size()) {
    if (text[pos] == '\\' && pos + 5 < text.size() &&
        (text[pos + 1] == 'u' || text[pos + 1] == 'U')) {
      int code = 0;
      bool valid = true;
      for (std::size_t i = 0; i < 4; ++i) {
        const int digit = hex_value(text[pos + 2 + i]);
        if (digit < 0) {
          valid = false;
          break;
        }
        code = code * 16 + digit;
      }
      if (valid) {
        // The legacy helper consumes any four-digit escape but only emits
        // ASCII. Non-ASCII escapes are discarded, not left as literals.
        if (code < 0x80) out.push_back(static_cast<char>(code));
        pos += 6;
        continue;
      }
    }
    out.push_back(text[pos]);
    ++pos;
  }
  return out;
}

std::string rstrip(std::string const& text) {
  return detail::trim_right(text);
}

std::string lstrip(std::string const& text) {
  return detail::trim_left(text);
}

// ---------------------------------------------------------------------------
// Checksums
// ---------------------------------------------------------------------------

std::string checksum16(std::string const& input) {
  const auto crc = CRC::Calculate(input.data(), input.size(),
                                  CRC::CRC_16_KERMIT());
  std::string packed = pack16bits(static_cast<std::uint16_t>(crc));
  if (packed.size() < 3) packed.append(3 - packed.size(), ' ');
  return packed;
}

bool checksum16_valid(std::string const& checksum, std::string const& input) {
  const auto crc = CRC::Calculate(input.data(), input.size(),
                                  CRC::CRC_16_KERMIT());
  return pack16bits(static_cast<std::uint16_t>(crc)) == checksum;
}

std::string checksum32(std::string const& input) {
  const auto crc = CRC::Calculate(input.data(), input.size(),
                                  CRC::CRC_32_BZIP2());
  std::string packed = pack32bits(static_cast<std::uint32_t>(crc));
  if (packed.size() < 6) packed.append(6 - packed.size(), ' ');
  return packed;
}

bool checksum32_valid(std::string const& checksum, std::string const& input) {
  const auto crc = CRC::Calculate(input.data(), input.size(),
                                  CRC::CRC_32_BZIP2());
  return pack32bits(static_cast<std::uint32_t>(crc)) == checksum;
}

// ---------------------------------------------------------------------------
// Huffman coding
// ---------------------------------------------------------------------------

HuffTable default_huff_table() { return tables::kHuffman; }

HuffEncoded huff_encode(HuffTable const& table, std::string const& text) {
  std::vector<std::string> keys;
  keys.reserve(table.size());
  for (auto const& entry : table) keys.push_back(entry.first);

  std::sort(keys.begin(), keys.end(), [](std::string const& a,
                                         std::string const& b) {
    if (a.size() != b.size()) return a.size() > b.size();
    return a > b;
  });

  HuffEncoded encoded;
  std::size_t pos = 0;
  while (pos < text.size()) {
    bool matched = false;
    for (auto const& key : keys) {
      if (text.compare(pos, key.size(), key) != 0) continue;
      const std::string& code = table.at(key);
      std::vector<bool> bits;
      bits.reserve(code.size());
      for (char c : code) bits.push_back(c == '1');
      encoded.emplace_back(static_cast<int>(key.size()), std::move(bits));
      pos += key.size();
      matched = true;
      break;
    }
    if (!matched) ++pos;
  }
  return encoded;
}

namespace {

struct HuffNode {
  int child[2] = {-1, -1};
  char symbol = 0;
  bool terminal = false;
};

std::vector<HuffNode> build_huffman_tree(HuffTable const& table) {
  std::vector<HuffNode> nodes(1);
  for (auto const& entry : table) {
    int node = 0;
    for (char bit : entry.second) {
      const int branch = (bit == '1') ? 1 : 0;
      if (nodes[static_cast<std::size_t>(node)].child[branch] < 0) {
        nodes[static_cast<std::size_t>(node)].child[branch] =
            static_cast<int>(nodes.size());
        nodes.emplace_back();
      }
      node = nodes[static_cast<std::size_t>(node)].child[branch];
    }
    HuffNode& leaf = nodes[static_cast<std::size_t>(node)];
    leaf.symbol = entry.first.empty() ? '\0' : entry.first.front();
    leaf.terminal = true;
  }
  return nodes;
}

}  // namespace

std::string huff_decode(HuffTable const& table, std::vector<bool> const& bits) {
  const std::vector<HuffNode> tree = build_huffman_tree(table);
  std::string text;
  int node = 0;
  for (bool bit : bits) {
    const int next = tree[static_cast<std::size_t>(node)].child[bit ? 1 : 0];
    if (next < 0) break;
    node = next;
    if (tree[static_cast<std::size_t>(node)].terminal) {
      text.push_back(tree[static_cast<std::size_t>(node)].symbol);
      node = 0;
    }
  }
  return text;
}

std::unordered_set<std::string> huff_valid_chars(HuffTable const& table) {
  std::unordered_set<std::string> out;
  for (auto const& entry : table) out.insert(entry.first);
  return out;
}

// ---------------------------------------------------------------------------
// Bit and byte conversion
// ---------------------------------------------------------------------------

std::vector<bool> bytes_to_bits(char* bitvec, int n) {
  std::vector<bool> bits;
  bits.reserve(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) bits.push_back(bitvec[i] == 0x01);
  return bits;
}

std::vector<bool> str_to_bits(std::string const& bitvec) {
  std::vector<bool> bits;
  bits.reserve(bitvec.size());
  for (char c : bitvec) bits.push_back(c == '1');
  return bits;
}

std::string bits_to_str(std::vector<bool> const& bitvec) {
  std::string out;
  out.reserve(bitvec.size());
  for (bool bit : bitvec) out.push_back(bit ? '1' : '0');
  return out;
}

std::vector<bool> int_to_bits(std::uint64_t value, int expected) {
  std::vector<bool> bits;
  while (value != 0) {
    bits.push_back((value & 1u) != 0);
    value >>= 1;
  }
  std::reverse(bits.begin(), bits.end());
  if (expected > 0) {
    while (static_cast<int>(bits.size()) < expected) {
      bits.insert(bits.begin(), false);
    }
  }
  return bits;
}

std::uint64_t bits_to_int(std::vector<bool> value) {
  std::uint64_t result = 0;
  for (bool bit : value) result = (result << 1) | (bit ? 1u : 0u);
  return result;
}

std::uint64_t bits_to_int(std::vector<bool>::const_iterator start, int n) {
  std::uint64_t result = 0;
  for (int i = 0; i < n; ++i) {
    result = (result << 1) | (*(start + i) ? 1u : 0u);
  }
  return result;
}

std::vector<bool> bits_list_to_bits(std::vector<std::vector<bool>>& list) {
  std::size_t total = 0;
  for (auto const& entry : list) total += entry.size();
  std::vector<bool> out;
  out.reserve(total);
  for (auto const& entry : list) out.insert(out.end(), entry.begin(), entry.end());
  return out;
}

// ---------------------------------------------------------------------------
// Radix packing helpers
// ---------------------------------------------------------------------------

namespace {

std::size_t index_in(std::string_view alphabet, char c) {
  return alphabet.find(c);
}

}  // namespace

std::uint8_t unpack5bits(std::string const& value) {
  return static_cast<std::uint8_t>(index_in(tables::kAlphabet, value.at(0)));
}

std::string pack5bits(std::uint8_t packed) {
  return std::string(1, tables::kAlphabet.at(packed % 32));
}

std::uint8_t unpack6bits(std::string const& value) {
  return static_cast<std::uint8_t>(index_in(tables::kAlphabet, value.at(0)));
}

std::string pack6bits(std::uint8_t packed) {
  return std::string(1, tables::kAlphabet.at(packed % 41));
}

std::uint16_t unpack16bits(std::string const& value) {
  if (value.size() < 3) return 0;
  const int a = static_cast<int>(index_in(tables::kAlphabet, value[0]));
  const int b = static_cast<int>(index_in(tables::kAlphabet, value[1]));
  const int c = static_cast<int>(index_in(tables::kAlphabet, value[2]));
  const int unpacked =
      tables::kAlphabetBase * tables::kAlphabetBase * a +
      tables::kAlphabetBase * b + c;
  if (unpacked > ((1 << 16) - 1)) return 0;
  return static_cast<std::uint16_t>(unpacked & ((1 << 16) - 1));
}

std::string pack16bits(std::uint16_t packed) {
  constexpr int kBase = tables::kAlphabetBase;
  std::string out;
  out.reserve(3);
  std::uint16_t tmp = packed / (kBase * kBase);
  out.push_back(tables::kAlphabet.at(tmp));
  tmp = (packed - tmp * (kBase * kBase)) / kBase;
  out.push_back(tables::kAlphabet.at(tmp));
  tmp = packed % kBase;
  out.push_back(tables::kAlphabet.at(tmp));
  return out;
}

std::uint32_t unpack32bits(std::string const& value) {
  if (value.size() < 6) return 0;
  return (static_cast<std::uint32_t>(unpack16bits(value.substr(0, 3))) << 16) |
         unpack16bits(value.substr(3, 3));
}

std::string pack32bits(std::uint32_t packed) {
  const auto high = static_cast<std::uint16_t>((packed & 0xFFFF0000u) >> 16);
  const auto low = static_cast<std::uint16_t>(packed & 0xFFFFu);
  return pack16bits(high) + pack16bits(low);
}

std::uint64_t unpack64bits(std::string const& value) {
  if (value.size() < 12) return 0;
  return (static_cast<std::uint64_t>(unpack32bits(value.substr(0, 6))) << 32) |
         unpack32bits(value.substr(6, 6));
}

std::string pack64bits(std::uint64_t packed) {
  const auto high =
      static_cast<std::uint32_t>((packed & 0xFFFFFFFF00000000ull) >> 32);
  const auto low = static_cast<std::uint32_t>(packed & 0xFFFFFFFFull);
  return pack32bits(high) + pack32bits(low);
}

namespace detail {

bool try_unpack72bits(std::string const& value, std::uint64_t* output,
                      std::uint8_t* remainder) {
  if (output) *output = 0;
  if (remainder) *remainder = 0;
  // The public Android-port decoder consumes the first twelve symbols, even
  // when the caller supplies trailing bytes. Validate only that frame prefix.
  // Keep rejecting invalid symbols inside the actual twelve-symbol payload.
  if (value.size() < 12) return false;

  std::uint64_t decoded = 0;
  for (int i = 0; i < 10; ++i) {
    auto const index = index_in(tables::kAlphabet72,
                                value[static_cast<std::size_t>(i)]);
    if (index >= 64) return false;
    decoded |= static_cast<std::uint64_t>(index) << (58 - 6 * i);
  }
  auto const high_index = index_in(tables::kAlphabet72, value[10]);
  auto const low_index = index_in(tables::kAlphabet72, value[11]);
  // Only the first 64 alphabet symbols fit in a six-bit wire digit.
  // The remaining punctuation characters are reserved/non-canonical.
  if (high_index >= 64 || low_index >= 64) return false;

  auto const high = static_cast<std::uint8_t>(high_index);
  auto const low = static_cast<std::uint8_t>(low_index);
  decoded |= static_cast<std::uint64_t>(high >> 2);
  if (output) *output = decoded;
  if (remainder) {
    *remainder = static_cast<std::uint8_t>(((high & 0x03u) << 6) | low);
  }
  return true;
}

}  // namespace detail

std::uint64_t unpack72bits(std::string const& value, std::uint8_t* pRem) {
  std::uint64_t decoded = 0;
  (void)detail::try_unpack72bits(value, &decoded, pRem);
  return decoded;
}

std::string pack72bits(std::uint64_t value, std::uint8_t rem) {
  std::array<char, 12> packed{};

  const auto rem_high =
      static_cast<std::uint8_t>(((value & 0x0Fu) << 2) | (rem >> 6));
  const auto rem_low = static_cast<std::uint8_t>(rem & 0x3Fu);
  value >>= 4;

  packed[11] = tables::kAlphabet72.at(rem_low);
  packed[10] = tables::kAlphabet72.at(rem_high);

  for (int i = 0; i < 10; ++i) {
    packed[static_cast<std::size_t>(9 - i)] =
        tables::kAlphabet72.at(static_cast<std::size_t>(value & 0x3Fu));
    value >>= 6;
  }

  return std::string(packed.data(), packed.size());
}

}  // namespace js8core::protocol::varicode

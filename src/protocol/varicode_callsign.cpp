#include "js8core/protocol/varicode.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "varicode_internal.hpp"
#include "varicode_tables.hpp"

namespace js8core::protocol::varicode {
namespace {

std::uint32_t alphabet_index(char c) {
  return static_cast<std::uint32_t>(tables::kAlphanumeric.find(c));
}

bool is_upper_or_space(char c) {
  return c == ' ' || std::isupper(static_cast<unsigned char>(c)) != 0;
}

bool is_upper_digit_or_space(char c) {
  return c == ' ' || std::isalnum(static_cast<unsigned char>(c)) != 0;
}

bool valid_callsign_block(std::string const& text) {
  if (text.size() != 6) return false;
  if (!is_upper_digit_or_space(text[0]) ||
      !is_upper_digit_or_space(text[1])) {
    return false;
  }
  if (std::isdigit(static_cast<unsigned char>(text[2])) == 0) return false;
  return is_upper_or_space(text[3]) && is_upper_or_space(text[4]) &&
         is_upper_or_space(text[5]);
}

}  // namespace

std::uint32_t pack_alphanumeric22(std::string const& value, bool is_flag) {
  std::string padded = value;
  while (padded.size() < 4) padded.push_back(' ');

  std::uint32_t packed = alphabet_index(padded[0]);
  packed = 37 * packed + alphabet_index(padded[1]);
  packed = 27 * packed + alphabet_index(padded[2]) - 10;
  packed = 27 * packed + alphabet_index(padded[3]) - 10;
  if (is_flag) packed |= (1u << 21);
  return packed;
}

std::string unpack_alphanumeric22(std::uint32_t packed, bool* is_flag) {
  if (is_flag) *is_flag = (packed & (1u << 21)) != 0;
  packed &= ~(1u << 21);

  char word[4];
  std::uint32_t tmp = packed % 27 + 10;
  word[3] = tables::kAlphanumeric.at(tmp);
  packed /= 27;
  tmp = packed % 27 + 10;
  word[2] = tables::kAlphanumeric.at(tmp);
  packed /= 27;
  tmp = packed % 37;
  word[1] = tables::kAlphanumeric.at(tmp);
  packed /= 37;
  word[0] = tables::kAlphanumeric.at(packed);
  return std::string(word, 4);
}

std::uint64_t pack_alphanumeric50(std::string const& value) {
  std::string clean;
  clean.reserve(value.size() + 3);
  for (char c : value) {
    if (tables::kAlphanumeric.find(c) != std::string_view::npos) {
      clean.push_back(c);
    }
  }

  if (clean.size() > 3 && clean[3] != '/') clean.insert(clean.begin() + 3, ' ');
  if (clean.size() > 7 && clean[7] != '/') clean.insert(clean.begin() + 7, ' ');
  while (clean.size() < 11) clean.push_back(' ');

  constexpr std::uint64_t kRadix[11] = {39, 38, 38, 2, 38, 38,
                                        38, 2,  38, 38, 38};

  std::uint64_t packed = 0;
  for (std::size_t i = 0; i < 11; ++i) {
    std::uint64_t digit = 0;
    if (i == 3 || i == 7) {
      digit = (clean[i] == '/') ? 1u : 0u;
    } else {
      const auto index = tables::kAlphanumeric.find(clean[i]);
      digit = index == std::string_view::npos ? 0u
                                              : static_cast<std::uint64_t>(index);
    }
    packed = packed * kRadix[i] + digit;
  }
  return packed;
}

std::string unpack_alphanumeric50(std::uint64_t packed) {
  constexpr std::uint64_t kRadix[11] = {39, 38, 38, 2, 38, 38,
                                        38, 2,  38, 38, 38};

  char word[11];
  for (int i = 10; i >= 0; --i) {
    const std::uint64_t digit = packed % kRadix[i];
    packed /= kRadix[i];
    if (i == 3 || i == 7) {
      word[i] = digit != 0 ? '/' : ' ';
    } else {
      word[i] = tables::kAlphanumeric.at(
          static_cast<std::size_t>(digit % tables::kAlphanumeric.size()));
    }
  }

  std::string out(word, 11);
  out.erase(std::remove(out.begin(), out.end(), ' '), out.end());
  return out;
}

std::uint32_t pack_callsign(std::string const& value, bool* p_portable) {
  const auto& base = tables::base_calls();
  if (auto it = base.find(value); it != base.end()) {
    return static_cast<std::uint32_t>(it->second);
  }

  std::string callsign = value;
  if (callsign.size() > 2 &&
      callsign.compare(callsign.size() - 2, 2, "/P") == 0) {
    callsign.resize(callsign.size() - 2);
    if (p_portable) *p_portable = true;
  }

  // Legacy Swaziland allocations use abbreviated prefixes.
  if (callsign.rfind("3DA0", 0) == 0) {
    callsign = "3D0" + callsign.substr(4);
  }
  if (callsign.rfind("3X", 0) == 0 && callsign.size() > 2 &&
      std::isalpha(static_cast<unsigned char>(callsign[2])) != 0) {
    callsign = "Q" + callsign.substr(2);
  }

  if (callsign.size() < 2 || callsign.size() > 6) return 0;

  std::vector<std::string> candidates{callsign};
  switch (callsign.size()) {
    case 2:
      candidates.push_back(" " + callsign + "   ");
      break;
    case 3:
      candidates.push_back(" " + callsign + "  ");
      candidates.push_back(callsign + "   ");
      break;
    case 4:
      candidates.push_back(" " + callsign + " ");
      candidates.push_back(callsign + "  ");
      break;
    case 5:
      candidates.push_back(" " + callsign);
      candidates.push_back(callsign + " ");
      break;
    default:
      break;
  }

  std::string block;
  for (auto const& candidate : candidates) {
    if (candidate.size() >= 6 && valid_callsign_block(candidate)) {
      block = candidate;
      break;
    }
  }
  if (block.size() < 6) return 0;

  std::uint32_t packed = alphabet_index(block[0]);
  packed = 36 * packed + alphabet_index(block[1]);
  packed = 10 * packed + alphabet_index(block[2]);
  packed = 27 * packed + alphabet_index(block[3]) - 10;
  packed = 27 * packed + alphabet_index(block[4]) - 10;
  packed = 27 * packed + alphabet_index(block[5]) - 10;
  return packed;
}

std::string unpack_callsign(std::uint32_t value, bool portable) {
  for (auto const& entry : tables::base_calls()) {
    if (entry.second == static_cast<int>(value)) return entry.first;
  }

  char word[6];
  std::uint32_t tmp = value % 27 + 10;
  word[5] = tables::kAlphanumeric.at(tmp);
  value /= 27;
  tmp = value % 27 + 10;
  word[4] = tables::kAlphanumeric.at(tmp);
  value /= 27;
  tmp = value % 27 + 10;
  word[3] = tables::kAlphanumeric.at(tmp);
  value /= 27;
  tmp = value % 10;
  word[2] = tables::kAlphanumeric.at(tmp);
  value /= 10;
  tmp = value % 36;
  word[1] = tables::kAlphanumeric.at(tmp);
  value /= 36;
  word[0] = tables::kAlphanumeric.at(value);

  std::string callsign(word, 6);
  if (callsign.rfind("3D0", 0) == 0) {
    callsign = "3DA0" + callsign.substr(3);
  }
  if (callsign.rfind("Q", 0) == 0 && callsign.size() > 1 &&
      std::isalpha(static_cast<unsigned char>(callsign[1])) != 0) {
    callsign = "3X" + callsign.substr(1);
  }

  std::string trimmed = detail::trim_left(detail::trim_right(callsign));
  if (portable) trimmed += "/P";
  return trimmed;
}

std::string deg2grid(float dlong, float dlat) {
  if (dlong < -180) dlong += 360;
  if (dlong > 180) dlong -= 360;

  char grid[6];
  int nlong = static_cast<int>(60.0 * (180.0 - dlong) / 5);
  int n1 = nlong / 240;
  int n2 = (nlong - 240 * n1) / 24;
  int n3 = nlong - 240 * n1 - 24 * n2;
  grid[0] = static_cast<char>('A' + n1);
  grid[2] = static_cast<char>('0' + n2);
  grid[4] = static_cast<char>('a' + n3);

  int nlat = static_cast<int>(60.0 * (dlat + 90) / 2.5);
  n1 = nlat / 240;
  n2 = (nlat - 240 * n1) / 24;
  n3 = nlat - 240 * n1 - 24 * n2;
  grid[1] = static_cast<char>('A' + n1);
  grid[3] = static_cast<char>('0' + n2);
  grid[5] = static_cast<char>('a' + n3);
  return std::string(grid, 6);
}

std::pair<float, float> grid2deg(std::string const& grid) {
  std::string g = grid;
  if (g.size() < 6) g = grid.substr(0, 4) + "mm";
  if (g.size() < 6) g.resize(6, 'm');

  g[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(g[0])));
  g[1] = static_cast<char>(std::toupper(static_cast<unsigned char>(g[1])));
  g[4] = static_cast<char>(std::tolower(static_cast<unsigned char>(g[4])));
  g[5] = static_cast<char>(std::tolower(static_cast<unsigned char>(g[5])));

  const int nlong = 180 - 20 * (g[0] - 'A');
  const int n20d = 2 * (g[2] - '0');
  const float xminlong = 5 * (g[4] - 'a' + 0.5f);
  const float dlong = nlong - n20d - xminlong / 60.0f;

  const int nlat = -90 + 10 * (g[1] - 'A') + g[3] - '0';
  const float xminlat = 2.5f * (g[5] - 'a' + 0.5f);
  const float dlat = nlat + xminlat / 60.0f;

  return {dlong, dlat};
}

std::uint16_t pack_grid(std::string const& value) {
  const std::string grid = detail::trim_left(detail::trim_right(value));
  if (grid.size() < 4) return static_cast<std::uint16_t>((1 << 15) - 1);

  const auto degrees = grid2deg(grid.substr(0, 4));
  const int ilong = static_cast<int>(degrees.first);
  const int ilat = static_cast<int>(degrees.second + 90);
  return static_cast<std::uint16_t>(((ilong + 180) / 2) * 180 + ilat);
}

std::string unpack_grid(std::uint16_t value) {
  const int base_grid = 180 * 180;
  if (value > base_grid) return {};

  const float dlat = value % 180 - 90;
  const float dlong = value / 180 * 2 - 180 + 2;
  return deg2grid(dlong, dlat).substr(0, 4);
}

}  // namespace js8core::protocol::varicode

#include "varicode_internal.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string_view>

#include "varicode_tables.hpp"
#include "varicode_policy.hpp"

namespace js8core::protocol::varicode::detail {
namespace {

bool is_space(char c) {
  return std::isspace(static_cast<unsigned char>(c)) != 0;
}

bool is_upper_or_digit(char c) {
  return std::isdigit(static_cast<unsigned char>(c)) != 0 ||
         std::isupper(static_cast<unsigned char>(c)) != 0;
}

std::size_t skip_space(std::string const& text, std::size_t pos) {
  while (pos < text.size() && is_space(text[pos])) ++pos;
  return pos;
}

bool equals_ci(std::string const& text, std::size_t pos,
               std::string_view needle) {
  if (pos + needle.size() > text.size()) return false;
  for (std::size_t i = 0; i < needle.size(); ++i) {
    const auto a = static_cast<unsigned char>(text[pos + i]);
    const auto b = static_cast<unsigned char>(needle[i]);
    if (std::toupper(a) != std::toupper(b)) return false;
  }
  return true;
}

bool grid4_at(std::string const& text, std::size_t pos) {
  if (pos + 4 > text.size()) return false;
  for (int i = 0; i < 2; ++i) {
    const char c = static_cast<char>(
        std::toupper(static_cast<unsigned char>(text[pos + static_cast<std::size_t>(i)])));
    if (c < 'A' || c > 'R') return false;
  }
  return std::isdigit(static_cast<unsigned char>(text[pos + 2])) != 0 &&
         std::isdigit(static_cast<unsigned char>(text[pos + 3])) != 0;
}

std::size_t grid_length_at(std::string const& text, std::size_t pos) {
  if (!grid4_at(text, pos)) return 0;
  if (pos + 6 <= text.size()) {
    bool sub = true;
    for (std::size_t i = 4; i < 6; ++i) {
      const char c = static_cast<char>(
          std::toupper(static_cast<unsigned char>(text[pos + i])));
      if (c < 'A' || c > 'X') sub = false;
    }
    if (sub) return 6;
  }
  return 4;
}

bool is_callsign_char(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '@' ||
         c == '/';
}

constexpr std::string_view kPunctuationCommands[] = {
    "AGN?",      "QSL?",     "HW CPY?",   "MSG TO:",    "SNR?",
    "INFO?",     "GRID?",    "STATUS?",   "QUERY MSGS?","HEARING?",
};

constexpr std::string_view kWordCommands[] = {
    "STATUS",  "HEARING", "QUERY CALL", "QUERY MSGS", "QUERY",
    "CMD",     "MSG",     "NACK",       "ACK",        "73",
    "YES",     "NO",      "HEARTBEAT SNR", "SNR",     "QSL",
    "RR",      "SK",      "FB",         "INFO",       "GRID",
    "DIT DIT",
};

}  // namespace

std::string trim_left(std::string const& text) {
  const auto first = std::find_if_not(
      text.begin(), text.end(),
      [](unsigned char c) { return std::isspace(c) != 0; });
  return std::string(first, text.end());
}

std::string trim_right(std::string const& text) {
  const auto last = std::find_if_not(
      text.rbegin(), text.rend(),
      [](unsigned char c) { return std::isspace(c) != 0; }).base();
  return std::string(text.begin(), last);
}

std::string upper(std::string const& text) {
  std::string out;
  out.reserve(text.size());
  for (char c : text) {
    out.push_back(static_cast<char>(
        std::toupper(static_cast<unsigned char>(c))));
  }
  return out;
}

std::string format_snr(int snr) {
  if (snr < -60 || snr > 60) return {};
  char buffer[8]{};
  std::snprintf(buffer, sizeof(buffer), "%s%0*d",
                snr >= 0 ? "+" : "", snr < 0 ? 3 : 2, snr);
  return buffer;
}

std::uint8_t pack_num_qtstyle(std::string const& num, bool* ok) {
  try {
    int value = std::stoi(num);
    value = std::clamp(value, -30, 31);
    if (ok) *ok = true;
    return static_cast<std::uint8_t>(value + 31);
  } catch (...) {
    if (ok) *ok = false;
    return 0;
  }
}

int mwatts_to_dbm(int mwatts) {
  for (const auto& reference : policy::kPowerReferences) {
    if (reference.milliwatts >= mwatts) return reference.dbm;
  }
  return policy::kPowerReferences.back().dbm;
}

int dbm_to_mwatts(int dbm) {
  auto const item = std::lower_bound(
      policy::kPowerReferences.begin(), policy::kPowerReferences.end(),
      dbm, [](const auto& reference, int target) {
        return reference.dbm < target;
      });
  return item == policy::kPowerReferences.end()
             ? policy::kPowerReferences.back().milliwatts
             : item->milliwatts;
}

bool has_letter_digit_pair(std::string const& text) {
  for (std::size_t i = 1; i < text.size(); ++i) {
    const bool prev_digit =
        std::isdigit(static_cast<unsigned char>(text[i - 1])) != 0;
    const bool curr_digit =
        std::isdigit(static_cast<unsigned char>(text[i])) != 0;
    const bool prev_letter =
        std::isupper(static_cast<unsigned char>(text[i - 1])) != 0;
    const bool curr_letter =
        std::isupper(static_cast<unsigned char>(text[i])) != 0;
    if ((prev_digit && curr_letter) || (prev_letter && curr_digit)) return true;
  }
  return false;
}

bool has_base_callsign_shape(std::string const& text) {
  // A normal callsign has one or two prefix characters, a district digit,
  // zero to three suffix letters, and optionally "/P". A longer suffix
  // belongs in the compound callsign encoding, not the 28-bit base call.
  std::string_view core(text);
  if (core.size() >= 2 && core.substr(core.size() - 2) == "/P") {
    core.remove_suffix(2);
  }
  const auto is_letter = [](char c) { return c >= 'A' && c <= 'Z'; };
  const auto is_digit = [](char c) { return c >= '0' && c <= '9'; };
  const auto suffix_letters = [&](std::size_t begin) {
    return std::all_of(core.begin() + static_cast<std::ptrdiff_t>(begin),
                       core.end(), is_letter);
  };
  if (core.size() >= 2 && core.size() <= 5 &&
      is_upper_or_digit(core[0]) && is_digit(core[1]) &&
      suffix_letters(2)) return true;
  return core.size() >= 3 && core.size() <= 6 &&
         is_upper_or_digit(core[0]) && is_upper_or_digit(core[1]) &&
         is_digit(core[2]) && suffix_letters(3);
}

bool has_compound_callsign_shape(std::string const& text) {
  // Independently recognize the legacy three-field compound grammar:
  // optional leading @, one initial character, then 0..2 / 0..3 / 0..3
  // characters with optional slash separators. Only the first character
  // can be '@'; later fields allow letters, digits, and slashes.
  if (text.empty() || !is_upper_or_digit(text.back())) return false;
  const auto field_valid = [&](std::size_t first, std::size_t last) {
    if (last > text.size()) return false;
    for (std::size_t i = first; i < last; ++i) {
      if (!is_upper_or_digit(text[i]) && text[i] != '/') return false;
    }
    return true;
  };
  for (std::size_t optional_at = 0; optional_at <= 1; ++optional_at) {
    if (optional_at && text[0] != '@') continue;
    std::size_t const start = optional_at;
    if (start >= text.size()) continue;
    char const first_char = text[start];
    if (!is_upper_or_digit(first_char) && first_char != '/' &&
        first_char != '@') continue;

    std::size_t const after_first = start + 1;
    for (std::size_t first = 0; first <= 2; ++first) {
      if (!field_valid(after_first, after_first + first)) continue;
      std::size_t const second_start = after_first + first;
      for (std::size_t slash1 = 0; slash1 <= 1; ++slash1) {
        if (slash1 && (second_start >= text.size() ||
                       text[second_start] != '/')) continue;
        std::size_t const second = second_start + slash1;
        for (std::size_t middle = 0; middle <= 3; ++middle) {
          if (!field_valid(second, second + middle)) continue;
          std::size_t const third_start = second + middle;
          for (std::size_t slash2 = 0; slash2 <= 1; ++slash2) {
            if (slash2 && (third_start >= text.size() ||
                           text[third_start] != '/')) continue;
            std::size_t const third = third_start + slash2;
            for (std::size_t tail = 0; tail <= 3; ++tail) {
              if (third + tail == text.size() &&
                  field_valid(third, third + tail)) return true;
            }
          }
        }
      }
    }
  }
  return false;
}

bool match_command(std::string const& text, std::size_t pos,
                   std::string& command, std::size_t& end) {
  const std::size_t n = text.size();

  const auto attempt = [&](std::size_t at) -> bool {
    for (auto const& candidate : kPunctuationCommands) {
      if (equals_ci(text, at, candidate)) {
        end = at + candidate.size();
        return true;
      }
    }
    for (auto const& candidate : kWordCommands) {
      if (!equals_ci(text, at, candidate)) continue;
      const std::size_t after = at + candidate.size();
      if (after == n || text[after] == ' ') {
        end = after;
        return true;
      }
    }
    if (at < n && (text[at] == '?' || text[at] == '>' || text[at] == ' ')) {
      end = at + 1;
      return true;
    }
    return false;
  };

  if (pos < n && is_space(text[pos])) {
    if (attempt(pos + 1)) {
      command = text.substr(pos, end - pos);
      return true;
    }
  }
  if (attempt(pos)) {
    command = text.substr(pos, end - pos);
    return true;
  }
  return false;
}

bool parse_heartbeat(std::string const& text, ParsedMessage& out) {
  const std::size_t n = text.size();
  std::size_t pos = skip_space(text, 0);

  if (equals_ci(text, pos, "@ALLCALL") && pos + 8 < n &&
      is_space(text[pos + 8])) {
    pos = skip_space(text, pos + 8);
  } else if (equals_ci(text, pos, "@HB") && pos + 3 < n &&
             is_space(text[pos + 3])) {
    pos = skip_space(text, pos + 3);
  }

  static constexpr std::string_view kTypes[] = {
      "CQ CQ CQ", "CQ DX", "CQ QRP", "CQ CONTEST", "CQ FIELD", "CQ FD",
      "CQ CQ",    "CQ",    "HB ALT", "HB",         "HEARTBEAT",
  };

  bool found = false;
  for (auto const& type : kTypes) {
    if (!equals_ci(text, pos, type)) continue;
    if (type == "HEARTBEAT") {
      std::size_t after = pos + type.size();
      const std::size_t spaced = skip_space(text, after);
      if (spaced > after && equals_ci(text, spaced, "SNR")) continue;
    }
    out.type = text.substr(pos, type.size());
    pos += type.size();
    found = true;
    break;
  }
  if (!found) return false;

  const std::size_t grid = skip_space(text, pos);
  if (grid > pos && grid4_at(text, grid)) {
    out.grid = text.substr(grid, 4);
  }
  return true;
}

bool parse_compound(std::string const& text, ParsedMessage& out) {
  const std::size_t n = text.size();
  std::size_t pos = skip_space(text, 0);
  if (pos >= n || text[pos] != '`') return false;
  ++pos;

  const std::size_t call_start = pos;
  while (pos < n && is_callsign_char(text[pos])) ++pos;
  if (pos == call_start) return false;
  out.callsign = text.substr(call_start, pos - call_start);

  const std::size_t grid_start = skip_space(text, pos);
  if (grid_start > pos) {
    const std::size_t length = grid_length_at(text, grid_start);
    if (length != 0) {
      out.grid = text.substr(grid_start, length);
      pos = grid_start + length;
    }
  }

  std::string command;
  std::size_t command_end = pos;
  if (match_command(text, pos, command, command_end)) {
    out.command = command;
    pos = command_end;
  }

  const std::size_t number_start = pos;
  std::size_t cursor = skip_space(text, pos);
  if (cursor < n && (text[cursor] == '+' || text[cursor] == '-')) ++cursor;
  const std::size_t digits = cursor;
  while (cursor < n &&
         std::isdigit(static_cast<unsigned char>(text[cursor])) != 0 &&
         cursor - digits < 3) {
    ++cursor;
  }
  if (cursor > digits) {
    out.number = text.substr(number_start, cursor - number_start);
    pos = cursor;
  }

  return skip_space(text, pos) == n;
}

bool parse_directed(std::string const& text, ParsedMessage& out) {
  const std::size_t n = text.size();
  std::size_t pos = skip_space(text, 0);

  const std::size_t call_start = pos;
  while (pos < n && is_callsign_char(text[pos])) ++pos;
  if (pos == call_start) return false;
  out.callsign = text.substr(call_start, pos - call_start);

  if (pos < n && text[pos] == ':') ++pos;

  std::string command;
  std::size_t command_end = pos;
  if (match_command(text, pos, command, command_end)) {
    out.command = command;
    out.command_end = command_end;
  } else {
    out.command.clear();
    out.command_end = pos;
  }
  return true;
}

}  // namespace js8core::protocol::varicode::detail

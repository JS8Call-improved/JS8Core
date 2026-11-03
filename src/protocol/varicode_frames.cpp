#include "js8core/protocol/varicode.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "js8core/protocol/jsc.hpp"
#include "varicode_internal.hpp"
#include "varicode_tables.hpp"

namespace js8core::protocol::varicode {
namespace {

using tables::FrameType;

Backend g_backend;
std::mutex g_backend_mutex;

Backend backend_snapshot() {
  std::lock_guard<std::mutex> lock(g_backend_mutex);
  return g_backend;
}

bool starts_with(std::string const& text, std::string_view prefix) {
  return text.size() >= prefix.size() &&
         text.compare(0, prefix.size(), prefix) == 0;
}

std::string pack_huffman_frame(std::string const& text, int* n) {
  constexpr int kFrameBits = 72;
  const HuffTable table = default_huff_table();
  const auto valid = huff_valid_chars(table);

  for (char ch : text) {
    const char upper_ch = static_cast<char>(
        std::toupper(static_cast<unsigned char>(ch)));
    if (valid.count(std::string(1, upper_ch)) == 0) {
      if (n) *n = 0;
      return {};
    }
  }

  std::vector<bool> bits{true, false};
  int chars_used = 0;
  for (auto const& entry : huff_encode(table, text)) {
    if (static_cast<int>(bits.size() + entry.second.size()) >= kFrameBits) break;
    bits.insert(bits.end(), entry.second.begin(), entry.second.end());
    chars_used += entry.first;
  }

  const int padding = kFrameBits - static_cast<int>(bits.size());
  for (int i = 0; i < padding; ++i) bits.push_back(i != 0);

  const auto value = bits_to_int(bits.begin(), 64);
  const auto rem = static_cast<std::uint8_t>(bits_to_int(bits.begin() + 64, 8));
  if (n) *n = chars_used;
  return pack72bits(static_cast<std::uint64_t>(value), rem);
}

std::string pack_jsc_frame(std::string const& text, int* n) {
  constexpr int kFrameBits = 72;
  const std::string upper_text = detail::upper(text);

  std::vector<bool> bits{true, true};
  int chars_used = 0;
  for (auto const& entry : jsc::compress(upper_text)) {
    if (static_cast<int>(bits.size() + entry.first.size()) < kFrameBits) {
      bits.insert(bits.end(), entry.first.begin(), entry.first.end());
      chars_used += static_cast<int>(entry.second);
      continue;
    }
    break;
  }

  const int padding = kFrameBits - static_cast<int>(bits.size());
  for (int i = 0; i < padding; ++i) bits.push_back(i != 0);

  const auto value = bits_to_int(bits.begin(), 64);
  const auto rem = static_cast<std::uint8_t>(bits_to_int(bits.begin() + 64, 8));
  if (n) *n = chars_used;
  return pack72bits(static_cast<std::uint64_t>(value), rem);
}

std::vector<bool> unpack_72_to_bits(std::string const& text) {
  std::uint8_t rem = 0;
  std::uint64_t value = 0;
  if (!detail::try_unpack72bits(text, &value, &rem)) return {};
  std::vector<bool> bits = int_to_bits(value, 64);
  const std::vector<bool> tail = int_to_bits(rem, 8);
  bits.insert(bits.end(), tail.begin(), tail.end());
  return bits;
}

void strip_padding(std::vector<bool>& bits) {
  for (int i = static_cast<int>(bits.size()) - 1; i >= 0; --i) {
    if (!bits[static_cast<std::size_t>(i)]) {
      bits.erase(bits.begin() + i, bits.end());
      return;
    }
  }
}

std::size_t snr_number_end(std::string const& text, std::size_t pos) {
  const std::size_t begin = pos;
  if (pos < text.size() &&
      std::isspace(static_cast<unsigned char>(text[pos])) != 0) {
    ++pos;
  }
  if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) ++pos;

  if (pos + 1 < text.size() &&
      std::isdigit(static_cast<unsigned char>(text[pos + 1])) != 0) {
    const char first = text[pos];
    const char second = text[pos + 1];
    if ((first == '3' && (second == '0' || second == '1')) ||
        (first >= '0' && first <= '2')) {
      return pos + 2;
    }
  }
  if (pos < text.size() &&
      std::isdigit(static_cast<unsigned char>(text[pos])) != 0) {
    return pos + 1;
  }
  return begin;
}

}  // namespace

void set_backend(Backend const& backend) {
  std::lock_guard<std::mutex> lock(g_backend_mutex);
  g_backend = backend;
}

std::string pack_heartbeat_message(std::string const& text,
                                   std::string const& callsign, int* n) {
  detail::ParsedMessage parsed;
  if (!detail::parse_heartbeat(text, parsed)) {
    if (auto backend = backend_snapshot(); backend.pack_heartbeat_message) {
      return backend.pack_heartbeat_message(text, callsign, n);
    }
    if (n) *n = 0;
    return {};
  }

  if (callsign.empty()) {
    if (n) *n = 0;
    return {};
  }

  const bool alt = starts_with(parsed.type, "CQ");
  std::uint16_t extra = tables::kMaxGrid;
  if (!parsed.grid.empty()) extra = pack_grid(parsed.grid);

  std::uint8_t number = 0;
  for (auto const& entry : (alt ? tables::kCqTypes : tables::kHeartbeatTypes)) {
    if (entry.second == parsed.type) {
      number = static_cast<std::uint8_t>(entry.first);
      break;
    }
  }
  if (alt) extra |= (1 << 15);

  const auto frame = pack_compound_frame(
      callsign, static_cast<std::uint8_t>(FrameType::Heartbeat), extra, number);
  if (n) *n = static_cast<int>(text.size());
  return frame;
}

std::vector<std::string> unpack_heartbeat_message(std::string const& text,
                                                  std::uint8_t* pType,
                                                  bool* isAlt,
                                                  std::uint8_t* pBits3) {
  std::uint8_t type = static_cast<std::uint8_t>(FrameType::Heartbeat);
  std::uint16_t number = tables::kMaxGrid;
  std::uint8_t bits3 = 0;

  auto unpacked = unpack_compound_frame(text, &type, &number, &bits3);
  if (unpacked.empty() ||
      type != static_cast<std::uint8_t>(FrameType::Heartbeat)) {
    return {};
  }

  unpacked.push_back(unpack_grid(number & ((1 << 15) - 1)));
  if (isAlt) *isAlt = (number & (1 << 15)) != 0;
  if (pType) *pType = type;
  if (pBits3) *pBits3 = bits3;
  return unpacked;
}

std::string pack_compound_message(std::string const& text, int* n) {
  detail::ParsedMessage parsed;
  if (!detail::parse_compound(text, parsed)) {
    if (auto backend = backend_snapshot(); backend.pack_compound_message) {
      return backend.pack_compound_message(text, n);
    }
    if (n) *n = 0;
    return {};
  }

  if (parsed.callsign.empty()) {
    if (n) *n = 0;
    return {};
  }

  const bool valid_grid = !parsed.grid.empty();

  int command_value = 0;
  bool valid_command = false;
  for (auto const& entry : tables::kDirectedCommands) {
    if (entry.key == parsed.command) {
      command_value = entry.value;
      valid_command = is_command_allowed(parsed.command);
      break;
    }
  }

  auto type = static_cast<std::uint8_t>(FrameType::Compound);
  std::uint16_t extra = tables::kMaxGrid;

  if (valid_command) {
    bool packed_number = false;
    const auto number =
        detail::pack_num_qtstyle(detail::trim_left(parsed.number), nullptr);
    extra = static_cast<std::uint16_t>(
        tables::kUserGridStart +
        pack_cmd(static_cast<std::uint8_t>(command_value), number,
                 &packed_number));
    type = static_cast<std::uint8_t>(FrameType::CompoundDirected);
  } else if (valid_grid) {
    extra = pack_grid(parsed.grid);
  }

  const auto frame = pack_compound_frame(parsed.callsign, type, extra, 0);
  if (n) *n = static_cast<int>(text.size());
  return frame;
}

std::vector<std::string> unpack_compound_message(std::string const& text,
                                                 std::uint8_t* pType,
                                                 std::uint16_t* pNum,
                                                 std::uint8_t* pBits3) {
  std::uint8_t type = static_cast<std::uint8_t>(FrameType::Compound);
  std::uint16_t extra = tables::kMaxGrid;
  std::uint8_t bits3 = 0;

  auto unpacked = unpack_compound_frame(text, &type, &extra, &bits3);
  if (unpacked.empty() ||
      (type != static_cast<std::uint8_t>(FrameType::Compound) &&
       type != static_cast<std::uint8_t>(FrameType::CompoundDirected))) {
    return {};
  }

  if (extra <= tables::kBaseGridCount) {
    unpacked.push_back(" " + unpack_grid(extra));
  } else if (tables::kUserGridStart <= extra && extra < tables::kMaxGrid) {
    std::uint8_t number = 0;
    const auto command =
        unpack_cmd(static_cast<std::uint8_t>(extra - tables::kUserGridStart),
                   &number);
    for (auto const& entry : tables::kDirectedCommands) {
      if (entry.value != command) continue;
      unpacked.push_back(std::string(entry.key));
      if (is_snr_command(std::string(entry.key))) {
        unpacked.push_back(detail::format_snr(static_cast<int>(number) - 31));
      }
      break;
    }
  }

  if (pType) *pType = type;
  if (pBits3) *pBits3 = bits3;
  if (pNum) *pNum = extra;
  return unpacked;
}

std::string pack_compound_frame(std::string const& callsign, std::uint8_t type,
                                std::uint16_t num, std::uint8_t bits3) {
  if (type == static_cast<std::uint8_t>(FrameType::Data) ||
      type == static_cast<std::uint8_t>(FrameType::Directed)) {
    return {};
  }

  const auto packed_callsign = pack_alphanumeric50(callsign);
  if (packed_callsign == 0) return {};

  const auto packed_11 = static_cast<std::uint16_t>((num & (0x7FFu << 5)) >> 5);
  const auto packed_5 = static_cast<std::uint8_t>(num & 0x1Fu);
  const auto packed_8 = static_cast<std::uint8_t>((packed_5 << 3) | bits3);

  std::vector<bool> bits = int_to_bits(type, 3);
  const std::vector<bool> call_bits = int_to_bits(packed_callsign, 50);
  const std::vector<bool> extra_bits = int_to_bits(packed_11, 11);
  bits.insert(bits.end(), call_bits.begin(), call_bits.end());
  bits.insert(bits.end(), extra_bits.begin(), extra_bits.end());

  return pack72bits(bits_to_int(bits), packed_8);
}

std::vector<std::string> unpack_compound_frame(std::string const& text,
                                               std::uint8_t* pType,
                                               std::uint16_t* pNum,
                                               std::uint8_t* pBits3) {
  std::vector<std::string> unpacked;
  if (text.size() < 12 || text.find(' ') != std::string::npos) return unpacked;

  std::uint8_t packed_8 = 0;
  std::uint64_t value = 0;
  if (!detail::try_unpack72bits(text, &value, &packed_8)) return {};
  const std::vector<bool> bits = int_to_bits(value, 64);

  const auto packed_5 = static_cast<std::uint8_t>(packed_8 >> 3);
  const auto packed_3 = static_cast<std::uint8_t>(packed_8 & ((1 << 3) - 1));
  const auto flag =
      static_cast<std::uint8_t>(bits_to_int(bits.begin(), 3));

  if (flag != static_cast<std::uint8_t>(FrameType::Heartbeat) &&
      flag != static_cast<std::uint8_t>(FrameType::Compound) &&
      flag != static_cast<std::uint8_t>(FrameType::CompoundDirected)) {
    return unpacked;
  }

  const auto packed_callsign = bits_to_int(bits.begin() + 3, 50);
  const auto packed_11 = static_cast<std::uint16_t>(bits_to_int(bits.begin() + 53, 11));

  const auto callsign = unpack_alphanumeric50(packed_callsign);
  const auto num = static_cast<std::uint16_t>((packed_11 << 5) | packed_5);

  if (pType) *pType = flag;
  if (pNum) *pNum = num;
  if (pBits3) *pBits3 = packed_3;

  unpacked.push_back(callsign);
  unpacked.push_back("");
  return unpacked;
}

std::string pack_directed_message(std::string const& text,
                                  std::string const& mycall, std::string* pTo,
                                  bool* pToCompound, std::string* pCmd,
                                  std::string* pNum, int* n) {
  detail::ParsedMessage parsed;
  if (!detail::parse_directed(text, parsed)) {
    if (auto backend = backend_snapshot(); backend.pack_directed_message) {
      return backend.pack_directed_message(text, mycall, pTo, pToCompound,
                                           pCmd, pNum, n);
    }
    if (n) *n = 0;
    return {};
  }

  std::string from = mycall;
  bool portable_from = false;
  if (is_compound_callsign(from)) from = "<....>";
  const auto packed_from = pack_callsign(from, &portable_from);

  const std::string to = parsed.callsign;
  const std::string command = parsed.command;
  if (command.empty()) {
    if (n) *n = 0;
    return {};
  }

  std::size_t consumed = parsed.command_end;
  std::string number;
  if (command.size() >= 3 &&
      command.compare(command.size() - 3, 3, "SNR") == 0) {
    const std::size_t end = snr_number_end(text, consumed);
    if (end != consumed) {
      number = text.substr(consumed, end - consumed);
      consumed = end;
    }
  }

  bool to_compound = false;
  const bool valid_to =
      to != mycall && is_valid_callsign(to, &to_compound);
  if (!valid_to) {
    if (n) *n = 0;
    return {};
  }

  if (pTo) *pTo = to;
  if (pToCompound) *pToCompound = to_compound;

  const std::string packed_to_name = to_compound ? "<....>" : to;

  if (!is_command_allowed(command) &&
      !is_command_allowed(detail::trim_left(command))) {
    if (n) *n = 0;
    return {};
  }

  bool portable_to = false;
  const auto packed_to = pack_callsign(packed_to_name, &portable_to);
  if (packed_from == 0 || packed_to == 0) {
    if (n) *n = 0;
    return {};
  }

  std::uint8_t packed_command = 0;
  for (auto const& entry : tables::kDirectedCommands) {
    if (entry.key == command || entry.key == (" " + command)) {
      packed_command = static_cast<std::uint8_t>(entry.value);
      break;
    }
  }

  bool number_ok = false;
  const auto packed_number =
      detail::pack_num_qtstyle(detail::trim_left(number), &number_ok);
  if (pCmd) *pCmd = command;
  if (number_ok && pNum) *pNum = number;

  const auto flag = static_cast<std::uint8_t>(FrameType::Directed);
  const auto extra = static_cast<std::uint8_t>(
      ((portable_from ? 1 : 0) << 7) | ((portable_to ? 1 : 0) << 6) |
      packed_number);

  std::vector<bool> bits = int_to_bits(flag, 3);
  const std::vector<bool> from_bits = int_to_bits(packed_from, 28);
  const std::vector<bool> to_bits = int_to_bits(packed_to, 28);
  const std::vector<bool> command_bits =
      int_to_bits(static_cast<std::uint8_t>(packed_command % 32), 5);
  bits.insert(bits.end(), from_bits.begin(), from_bits.end());
  bits.insert(bits.end(), to_bits.begin(), to_bits.end());
  bits.insert(bits.end(), command_bits.begin(), command_bits.end());

  if (n) *n = static_cast<int>(consumed);
  return pack72bits(bits_to_int(bits), extra);
}

std::vector<std::string> unpack_directed_message(std::string const& text,
                                                 std::uint8_t* pType) {
  std::vector<std::string> out;
  if (text.size() < 12 || text.find(' ') != std::string::npos) return out;

  std::uint8_t extra = 0;
  std::uint64_t value = 0;
  if (!detail::try_unpack72bits(text, &value, &extra)) return {};
  const std::vector<bool> bits = int_to_bits(value, 64);

  const auto flag = static_cast<std::uint8_t>(bits_to_int(bits.begin(), 3));
  if (flag != static_cast<std::uint8_t>(FrameType::Directed)) return out;

  const auto packed_from =
      static_cast<std::uint32_t>(bits_to_int(bits.begin() + 3, 28));
  const auto packed_to =
      static_cast<std::uint32_t>(bits_to_int(bits.begin() + 31, 28));
  const auto packed_command =
      static_cast<std::uint8_t>(bits_to_int(bits.begin() + 59, 5));

  const bool portable_from = ((extra >> 7) & 1) == 1;
  const bool portable_to = ((extra >> 6) & 1) == 1;
  extra = static_cast<std::uint8_t>(extra % 64);

  const auto from = unpack_callsign(packed_from, portable_from);
  const auto to = unpack_callsign(packed_to, portable_to);

  std::string command;
  for (auto const& entry : tables::kDirectedCommands) {
    if (entry.value == packed_command % 32) {
      command = std::string(entry.key);
      break;
    }
  }

  if (!from.empty()) out.push_back(from);
  if (!to.empty()) out.push_back(to);
  if (!command.empty()) out.push_back(command);

  if (extra != 0) {
    if (!command.empty() && is_snr_command(command)) {
      out.push_back(detail::format_snr(static_cast<int>(extra) - 31));
    } else {
      out.push_back(std::to_string(static_cast<int>(extra) - 31));
    }
  }

  if (pType) *pType = flag;
  return out;
}

std::string pack_data_message(std::string const& text, int* n) {
  int huffman_chars = 0;
  int jsc_chars = 0;
  const auto huffman = pack_huffman_frame(detail::upper(text), &huffman_chars);
  const auto jsc_frame = pack_jsc_frame(text, &jsc_chars);

  if (huffman_chars > jsc_chars) {
    if (n) *n = huffman_chars;
    return huffman;
  }
  if (n) *n = jsc_chars;
  return jsc_frame;
}

std::string unpack_data_message(std::string const& text) {
  if (text.size() < 12 || text.find(' ') != std::string::npos) return {};

  std::vector<bool> bits = unpack_72_to_bits(text);
  if (bits.empty() || !bits.front()) return {};
  bits.erase(bits.begin());
  if (bits.empty()) return {};

  const bool compressed = bits.front();
  bits.erase(bits.begin());
  strip_padding(bits);
  if (bits.empty()) return {};

  return compressed ? jsc::decompress(bits)
                    : huff_decode(default_huff_table(), bits);
}

std::string pack_fast_data_message(std::string const& text, int* n) {
  constexpr int kFrameBits = 72;
  const std::string upper_text = detail::upper(text);

  std::vector<bool> bits;
  int chars_used = 0;
  for (auto const& entry : jsc::compress(upper_text)) {
    if (static_cast<int>(bits.size() + entry.first.size()) < kFrameBits) {
      bits.insert(bits.end(), entry.first.begin(), entry.first.end());
      chars_used += static_cast<int>(entry.second);
      continue;
    }
    break;
  }

  const int padding = kFrameBits - static_cast<int>(bits.size());
  for (int i = 0; i < padding; ++i) bits.push_back(i != 0);

  const auto value = bits_to_int(bits.begin(), 64);
  const auto rem = static_cast<std::uint8_t>(bits_to_int(bits.begin() + 64, 8));
  if (n) *n = chars_used;
  return pack72bits(static_cast<std::uint64_t>(value), rem);
}

std::string unpack_fast_data_message(std::string const& text) {
  if (text.size() < 12 || text.find(' ') != std::string::npos) return {};
  std::vector<bool> bits = unpack_72_to_bits(text);
  strip_padding(bits);
  return jsc::decompress(bits);
}

std::vector<std::pair<std::string, int>> build_message_frames(
    std::string const& mycall, std::string const& mygrid,
    std::string const& selectedCall, std::string const& text,
    bool forceIdentify, bool forceData, int submode, MessageInfo* pInfo) {
  const bool compound_caller = is_compound_callsign(mycall);
  std::vector<std::pair<std::string, int>> all_frames;

  std::string line = text;
  std::vector<std::pair<std::string, int>> line_frames;
  bool has_directed = false;
  bool has_data = forceData;

  // Forced data carries an exact payload; automatic identification would
  // change it.
  if (forceData) forceIdentify = false;

  const std::string own_prefix = mycall + ":";
  if (starts_with(line, own_prefix) || starts_with(line, mycall + " ")) {
    line = detail::trim_left(line.substr(mycall.size() + 1));
  }

  if (!selectedCall.empty() && !starts_with(line, selectedCall) &&
      !starts_with(line, "`") && !forceData) {
    const bool starts_base =
        starts_with(line, "@ALLCALL") || starts_with(line, "CQ") ||
        starts_with(line, "HB");
    const auto separator = line.find_first_of(" :");
    const std::string first_token = line.substr(0, separator);
    bool token_compound = false;
    const bool starts_callsign =
        is_valid_callsign(first_token, &token_compound) &&
        first_token.size() > 3;
    if (!starts_base && !starts_callsign) {
      const std::string joiner = starts_with(line, " ") ? "" : " ";
      line = selectedCall + joiner + line;
    }
  }

  while (!line.empty()) {
    std::string frame;
    bool use_beacon = false;
    bool use_compound = false;
    bool use_directed = false;
    bool use_data = false;

    int beacon_length = 0;
    const auto beacon_frame =
        pack_heartbeat_message(line, mycall, &beacon_length);

    int compound_length = 0;
    const auto compound_frame = pack_compound_message(line, &compound_length);

    int directed_length = 0;
    std::string directed_command;
    std::string directed_to;
    std::string directed_number;
    bool directed_to_compound = false;
    const auto directed_frame = pack_directed_message(
        line, mycall, &directed_to, &directed_to_compound, &directed_command,
        &directed_number, &directed_length);

    if (forceIdentify && line_frames.empty() && selectedCall.empty() &&
        directed_to.empty() && beacon_length == 0 && compound_length == 0 &&
        line.find(mycall) == std::string::npos) {
      line = mycall + ": " + line;
    }

    int data_length = 0;
    const bool fast_data = submode != 0;
    const auto data_frame = fast_data ? pack_fast_data_message(line, &data_length)
                                      : pack_data_message(line, &data_length);

    if (!has_directed && !has_data && beacon_length > 0) {
      use_beacon = true;
      frame = beacon_frame;
    } else if (!has_directed && !has_data && directed_length > 0) {
      use_directed = true;
      has_directed = true;
      frame = directed_frame;
    } else if (!has_directed && !has_data && compound_length > 0) {
      use_compound = true;
      frame = compound_frame;
    } else if (data_length > 0) {
      use_data = true;
      has_data = true;
      frame = data_frame;
    }

    if (use_beacon) {
      line_frames.push_back({frame, 0});
      line = line.substr(static_cast<std::size_t>(beacon_length));
    }
    if (use_compound) {
      line_frames.push_back({frame, 0});
      line = line.substr(static_cast<std::size_t>(compound_length));
    }
    if (use_directed) {
      bool use_standard_frame = true;

      if (compound_caller || directed_to_compound) {
        const std::string identity =
            "`" + mycall + " " + mygrid;
        const auto identity_frame = pack_compound_message(identity, nullptr);
        if (!identity_frame.empty()) line_frames.push_back({identity_frame, 0});
      }

      if (compound_caller || directed_to_compound) {
        const std::string compound_message =
            "`" + directed_to + directed_command + directed_number;
        const auto compound_directed =
            pack_compound_message(compound_message, nullptr);
        if (!compound_directed.empty()) {
          line_frames.push_back({compound_directed, 0});
          use_standard_frame = false;
        }
      }

      if (use_standard_frame) line_frames.push_back({frame, 0});

      line = line.substr(static_cast<std::size_t>(directed_length));

      // Frame construction also buffers recognized space-bearing commands;
      // this rule does not change the public is_command_buffered() result.
      const bool buffered =
          is_command_buffered(directed_command) ||
          (directed_command.find(' ') != std::string::npos &&
           is_command_allowed(directed_command));
      if (buffered && !line.empty()) {
        line = detail::trim_left(line);
        const bool skip_aprs_checksum =
            detail::upper(directed_to) == "@APRSIS" &&
            (directed_command == " MSG" || directed_command == " MSG TO:");
        const int checksum_size =
            skip_aprs_checksum ? 0 : is_command_checksummed(directed_command);
        if (checksum_size == 32) {
          line = line + " " + checksum32(line);
        } else if (checksum_size == 16) {
          line = line + " " + checksum16(line);
        }
      }

      if (pInfo) {
        pInfo->dir_cmd = directed_command;
        pInfo->dir_to = directed_to;
        pInfo->dir_num = directed_number;
      }
    }
    if (use_data) {
      line_frames.push_back({frame, fast_data ? 4 : 0});
      line = line.substr(static_cast<std::size_t>(data_length));
    }
    // A message may contain characters that no frame encoder accepts.
    // Never spin indefinitely on unchanged input (including unencodable UTF-8).
    if (!use_beacon && !use_compound && !use_directed && !use_data) {
      return {};
    }
  }

  if (!line_frames.empty()) {
    line_frames.front().second |= 1;
    line_frames.back().second |= 2;
  }
  all_frames.insert(all_frames.end(), line_frames.begin(), line_frames.end());
  return all_frames;
}

}  // namespace js8core::protocol::varicode

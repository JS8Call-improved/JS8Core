#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace js8core::protocol::varicode::detail {

std::string trim_left(std::string const& text);
std::string trim_right(std::string const& text);
std::string upper(std::string const& text);

// Strict frame boundary: a malformed 72-bit wire string is never treated as
// an all-zero payload. The public unpack72bits() API remains source-compatible.
bool try_unpack72bits(std::string const& text, std::uint64_t* value,
                      std::uint8_t* remainder);

std::string format_snr(int snr);
std::uint8_t pack_num_qtstyle(std::string const& num, bool* ok);
int mwatts_to_dbm(int mwatts);
int dbm_to_mwatts(int dbm);

bool has_letter_digit_pair(std::string const& text);
bool has_base_callsign_shape(std::string const& text);
bool has_compound_callsign_shape(std::string const& text);

struct ParsedMessage {
  std::string type;
  std::string grid;
  std::string callsign;
  std::string command;
  std::string number;
  std::size_t command_end = 0;
};

bool match_command(std::string const& text, std::size_t pos, std::string& command,
                   std::size_t& end);
bool parse_heartbeat(std::string const& text, ParsedMessage& out);
bool parse_compound(std::string const& text, ParsedMessage& out);
bool parse_directed(std::string const& text, ParsedMessage& out);

}  // namespace js8core::protocol::varicode::detail

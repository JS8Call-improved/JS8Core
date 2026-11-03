#include "js8core/protocol/varicode.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>

#include "varicode_internal.hpp"
#include "varicode_policy.hpp"
#include "varicode_tables.hpp"

namespace js8core::protocol::varicode {
namespace {

int lookup_command(std::string const& command, bool* found) {
  for (auto const& entry : tables::kDirectedCommands) {
    if (entry.key == command) {
      if (found) *found = true;
      return entry.value;
    }
  }
  if (found) *found = false;
  return 0;
}

bool is_valid_compound_callsign(std::string const& callsign) {
  const auto slashes =
      std::count(callsign.begin(), callsign.end(), '/');
  if (static_cast<long>(callsign.size()) - slashes > 9) return false;

  if (const auto slash = callsign.find('/'); slash != std::string::npos) {
    const auto& base = tables::base_calls();
    return base.find(callsign.substr(0, slash)) == base.end();
  }

  if (!callsign.empty() && callsign.front() == '@') return true;
  return callsign.size() > 2 &&
         detail::has_letter_digit_pair(callsign);
}

}  // namespace

std::uint8_t pack_num(std::string const& num, bool* ok) {
  try {
    // Legacy numeric helpers accept an integer prefix with trailing text.
    const int value = std::stoi(num);
    if (value < -31 || value > 31) throw std::out_of_range("num");
    if (ok) *ok = true;
    return static_cast<std::uint8_t>((value + 64) & 0x7F);
  } catch (...) {
    if (ok) *ok = false;
    return 0;
  }
}

std::uint8_t pack_pwr(std::string const& pwr, bool* ok) {
  try {
    // Legacy API quantizes the integer prefix and narrows to one octet.
    const int dbm = std::stoi(pwr);
    const int milliwatts = detail::dbm_to_mwatts(dbm);
    if (ok) *ok = true;
    return static_cast<std::uint8_t>(milliwatts);
  } catch (...) {
    if (ok) *ok = false;
    return 0;
  }
}

// The compound-directed auxiliary field stores one byte. Standard commands
// occupy 0x00..0x7f. The protocol reserves two consecutive 64-byte regions for
// numeric signal reports. The region identifies the command while the offset
// identifies its six-bit numeric payload. This is an on-air format definition,
// not a translation of the desktop command-packing implementation.
namespace {
struct NumberedCommandRegion {
  std::uint8_t command_id;
  std::uint8_t first_octet;
};
constexpr unsigned kRegionWidth = 64;
constexpr unsigned kDirectCommandSpace = 128;
constexpr std::array<NumberedCommandRegion, 2> kNumberedRegions{{
    {25, 0x80}, // SNR
    {29, 0xc0}, // HEARTBEAT SNR
}};
}  // namespace

std::uint8_t pack_cmd(std::uint8_t cmd, std::uint8_t num,
                      bool* p_packed_num) {
  auto region = std::find_if(
      kNumberedRegions.begin(), kNumberedRegions.end(),
      [cmd](NumberedCommandRegion entry) { return entry.command_id == cmd; });
  const bool carries_number = region != kNumberedRegions.end();
  if (p_packed_num != nullptr) *p_packed_num = carries_number;

  if (!carries_number) {
    return static_cast<std::uint8_t>(cmd % kDirectCommandSpace);
  }
  const auto octet = static_cast<unsigned>(region->first_octet) +
                     static_cast<unsigned>(num) % kRegionWidth;
  return static_cast<std::uint8_t>(octet);
}

std::uint8_t unpack_cmd(std::uint8_t value, std::uint8_t* p_num) {
  auto region = std::find_if(
      kNumberedRegions.begin(), kNumberedRegions.end(),
      [value](NumberedCommandRegion entry) {
        const unsigned byte = value;
        const unsigned first = entry.first_octet;
        return byte >= first && byte - first < kRegionWidth;
      });

  if (region == kNumberedRegions.end()) {
    if (p_num != nullptr) *p_num = 0;
    return value;
  }

  if (p_num != nullptr) {
    *p_num = static_cast<std::uint8_t>(value - region->first_octet);
  }
  return region->command_id;
}

bool is_snr_command(std::string const& cmd) {
  bool found = false;
  const int value = lookup_command(cmd, &found);
  return found && tables::kSnrCommands.count(value) != 0;
}

bool is_command_allowed(std::string const& cmd) {
  bool found = false;
  const int value = lookup_command(cmd, &found);
  return found && policy::accepts_directed_command(value);
}

bool is_command_buffered(std::string const& cmd) {
  bool found = false;
  const int value = lookup_command(cmd, &found);
  // Keep the public ID-based predicate separate from framing policy.
  return found && policy::buffers_following_data(value);
}

int is_command_checksummed(std::string const& cmd) {
  bool found = false;
  const int value = lookup_command(cmd, &found);
  return found ? policy::trailing_checksum_bits(value) : 0;
}

bool is_command_autoreply(std::string const& cmd) {
  bool found = false;
  const int value = lookup_command(cmd, &found);
  return found && policy::requests_response(value);
}

bool is_valid_callsign(std::string const& callsign, bool* p_is_compound) {
  if (tables::base_calls().find(callsign) != tables::base_calls().end()) {
    if (p_is_compound) *p_is_compound = false;
    return true;
  }

  if (detail::has_base_callsign_shape(callsign)) {
    if (p_is_compound) *p_is_compound = false;
    return callsign.size() > 2 &&
           detail::has_letter_digit_pair(callsign);
  }

  if (detail::has_compound_callsign_shape(callsign)) {
    const bool valid = is_valid_compound_callsign(callsign);
    if (p_is_compound) *p_is_compound = valid;
    return valid;
  }

  if (p_is_compound) *p_is_compound = false;
  return false;
}

bool is_compound_callsign(std::string const& callsign) {
  bool compound = false;
  return is_valid_callsign(callsign, &compound) && compound;
}

bool is_group_allowed(std::string const& group) {
  return tables::base_calls().find(group) != tables::base_calls().end();
}

}  // namespace js8core::protocol::varicode

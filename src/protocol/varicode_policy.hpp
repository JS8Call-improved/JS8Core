#pragma once

#include <array>
#include <cstddef>

namespace js8core::protocol::varicode::policy {

// The command IDs themselves belong to the JS8 wire format. These decisions
// are application behavior layered on top of those IDs; they are deliberately
// kept separate from the bitstream and codebook tables.
//
// The inclusive range is the set of directed command IDs recognized by the
// interoperable implementation, including the -1 generic call/heartbeat ID.
constexpr bool accepts_directed_command(int id) noexcept {
  return id >= -1 && id <= 31;
}

// Which incoming operations expect an automatic response. These are grouped
// by their purpose rather than carried as the desktop application's set.
constexpr bool requests_response(int id) noexcept {
  switch (id) {
    case 0:   // SNR?
    case 3:   // HEARING?
    case 4:   // GRID?
    case 6:   // STATUS?
    case 16:  // INFO?
    case 30:  // AGN?
    case 11:  // QUERY
    case 12:  // QUERY MSGS / QUERY MSGS?
    case 13:  // QUERY CALL
    case 9:   // MSG
    case 10:  // MSG TO:
    case 2:   // NACK
    case 14:  // ACK
      return true;
    default:
      return false;
  }
}

// ID-specific buffering for the public API. The message frame builder
// independently buffers recognized commands whose spelling contains a space.
constexpr bool buffers_following_data(int id) noexcept {
  switch (id) {
    case 5:   // relay (" >")
    case 9:   // MSG
    case 10:  // MSG TO:
    case 11:  // QUERY
    case 12:  // QUERY MSGS
    case 13:  // QUERY CALL
    case 15:  // GRID
    case 24:  // CMD
      return true;
    default:
      return false;
  }
}

// Message framing policy: checksum length is measured in bits. A buffered
// GRID message does not carry this checksum, whereas the commands below do.
constexpr int trailing_checksum_bits(int id) noexcept {
  switch (id) {
    case 5: case 9: case 10: case 11:
    case 12: case 13: case 24:
      return 16;
    default:
      return 0;
  }
}

// Convert dBm to the JS8 UI/reference power ladder. This is derived from
// powers of ten and the 1/2/5 series (not an independently meaningful wire
// index). A value with step r = 0/1/2 means 0/3/7 dB above the decade.
struct PowerReference {
  int dbm;
  int milliwatts;
};

consteval std::array<PowerReference, 19> make_power_references() {
  std::array<PowerReference, 19> result{};
  int decade_mw = 1;
  for (std::size_t index = 0; index < result.size(); ++index) {
    if (index != 0 && index % 3 == 0) decade_mw *= 10;
    const auto step = index % 3;
    const int magnitude = step == 0 ? 1 : step == 1 ? 2 : 5;
    const int dbm = static_cast<int>(index / 3) * 10 +
                    (step == 0 ? 0 : step == 1 ? 3 : 7);
    result[index] = {dbm, decade_mw * magnitude};
  }
  return result;
}

inline constexpr auto kPowerReferences = make_power_references();
static_assert(kPowerReferences.front().dbm == 0 &&
              kPowerReferences.front().milliwatts == 1 &&
              kPowerReferences.back().dbm == 60 &&
              kPowerReferences.back().milliwatts == 1000000);

}  // namespace js8core::protocol::varicode::policy

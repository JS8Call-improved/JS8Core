#include "../src/protocol/varicode_internal.hpp"
#include "../src/protocol/varicode_policy.hpp"
#include "js8core/protocol/varicode.hpp"

#include <array>
#include <cstdio>
#include <string>

namespace varicode = js8core::protocol::varicode;

namespace {
struct ExpectedBehavior {
  const char* command;
  bool allowed;
  bool autoreply;
  bool buffered;
  int checksum_bits;
};

// Reference Android-port public policy: only listed command IDs are buffered.
// Space-bearing command continuation is a separate frame-building rule.
constexpr std::array<ExpectedBehavior, 38> kBehaviors{{
    {" SNR?", true, true, false, 0},
    {"?", true, true, false, 0},
    {" HEARTBEAT", true, false, false, 0},
    {" HB", true, false, false, 0},
    {" CQ", true, false, false, 0},
    {" NACK", true, true, false, 0},
    {" HEARING?", true, true, false, 0},
    {" GRID?", true, true, false, 0},
    {" STATUS?", true, true, false, 0},
    {" MSG", true, true, true, 16},
    {" MSG TO:", true, true, true, 16},
    {" QUERY", true, true, true, 16},
    {" QUERY MSGS", true, true, true, 16},
    {" QUERY MSGS?", true, true, true, 16},
    {" QUERY CALL", true, true, true, 16},
    {" ACK", true, true, false, 0},
    {" INFO?", true, true, false, 0},
    {" AGN?", true, true, false, 0},
    {" GRID", true, false, true, 0},
    {" CMD", true, false, true, 16},
    {">", true, false, true, 16},
    {" STATUS", true, false, false, 0},
    {" HEARING", true, false, false, 0},
    {" INFO", true, false, false, 0},
    {" FB", true, false, false, 0},
    {" SNR", true, false, false, 0},
    {" HEARTBEAT SNR", true, false, false, 0},
    {" 73", true, false, false, 0},
    {"  ", true, false, false, 0},
    {" NOT-A-COMMAND", false, false, false, 0},
    {" QSL?", true, false, false, 0},
    {" QSL", true, false, false, 0},
    {" SK", true, false, false, 0},
    {" DIT DIT", true, false, false, 0},
    {" YES", true, false, false, 0},
    {" >", false, false, false, 0},
    {" SNR? ", false, false, false, 0},
    {"SNR?", false, false, false, 0},
}};

struct PowerVector { int dbm; int mw; };
constexpr std::array<PowerVector, 19> kExpectedPower{{
    {0, 1}, {3, 2}, {7, 5}, {10, 10}, {13, 20},
    {17, 50}, {20, 100}, {23, 200}, {27, 500}, {30, 1000},
    {33, 2000}, {37, 5000}, {40, 10000}, {43, 20000},
    {47, 50000}, {50, 100000}, {53, 200000}, {57, 500000},
    {60, 1000000},
}};
}  // namespace

int main() {
  int failed = 0;
  for (auto const& expected : kBehaviors) {
    std::string const cmd(expected.command);
    bool const valid =
        varicode::is_command_allowed(cmd) == expected.allowed &&
        varicode::is_command_autoreply(cmd) == expected.autoreply &&
        varicode::is_command_buffered(cmd) == expected.buffered &&
        varicode::is_command_checksummed(cmd) == expected.checksum_bits;
    if (!valid) {
      std::fprintf(stderr, "FAIL: command policy for '%s'\n", expected.command);
      ++failed;
    }
  }

  for (std::size_t i = 0; i < kExpectedPower.size(); ++i) {
    auto const& actual = varicode::policy::kPowerReferences[i];
    auto const& expected = kExpectedPower[i];
    const bool valid = actual.dbm == expected.dbm &&
                       actual.milliwatts == expected.mw &&
                       varicode::detail::dbm_to_mwatts(expected.dbm) == expected.mw &&
                       varicode::detail::mwatts_to_dbm(expected.mw) == expected.dbm;
    if (!valid) {
      std::fprintf(stderr, "FAIL: reference power level %zu\n", i);
      ++failed;
    }
  }

  // Preserve old upward quantization and endpoint clamping semantics.
  if (varicode::detail::dbm_to_mwatts(4) != 5 ||
      varicode::detail::dbm_to_mwatts(100) != 1000000 ||
      varicode::detail::mwatts_to_dbm(3) != 7 ||
      varicode::detail::mwatts_to_dbm(2000000) != 60) {
    std::fputs("FAIL: power rounding/clamping\n", stderr);
    ++failed;
  }

  if (failed == 0)
    std::printf("PASS: %zu application policy vectors and %zu power values\n",
                kBehaviors.size(), kExpectedPower.size());
  return failed == 0 ? 0 : 1;
}

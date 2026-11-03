#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "js8core/protocol/varicode.hpp"

// Only wire identities and wire-visible quantization belong here. Application
// actions (autoreply, buffering, presentation power values, checksum policy)
// are specified in varicode_policy.hpp instead.

namespace js8core::protocol::varicode::tables {

inline constexpr std::string_view kAlphabet =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ+-./?";
inline constexpr std::string_view kAlphabet72 =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz-+/?.";
inline constexpr std::string_view kAlphanumeric =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ /@";
inline constexpr int kAlphabetBase = 41;

inline const HuffTable kHuffman = {
    {" ", "01"},       {"E", "100"},     {"T", "1101"},   {"A", "0011"},
    {"O", "11111"},    {"I", "11100"},   {"N", "10111"},  {"S", "10100"},
    {"H", "00011"},    {"R", "00000"},   {"D", "111011"}, {"L", "110011"},
    {"C", "110001"},   {"U", "101101"},  {"M", "101011"}, {"W", "001011"},
    {"F", "001001"},   {"G", "000101"},  {"Y", "000011"}, {"P", "1111011"},
    {"B", "1111001"},  {".", "1110100"}, {"V", "1100101"},{"K", "1100100"},
    {"-", "1100001"},  {"+", "1100000"}, {"?", "1011001"},{"!", "1011000"},
    {"\"", "1010101"}, {"X", "1010100"}, {"0", "0010101"},{"J", "0010100"},
    {"1", "0010001"},  {"Q", "0010000"}, {"2", "0001001"},{"Z", "0001000"},
    {"3", "0000101"},  {"5", "0000100"}, {"4", "11110101"},{"9", "11110100"},
    {"8", "11110001"}, {"6", "11110000"},{"7", "11101011"},{"/", "11101010"},
};

inline const std::unordered_set<int> kSnrCommands = {25, 29};

struct DirectedCommand {
  std::string_view key;
  int value;
};

inline const std::vector<DirectedCommand> kDirectedCommands = {
    {" HEARTBEAT", -1},   {" HB", -1},
    {" CQ", -1},          {" SNR?", 0},
    {"?", 0},             {" DIT DIT", 1},
    {" HEARING?", 3},     {" GRID?", 4},
    {">", 5},             {" STATUS?", 6},
    {" STATUS", 7},       {" HEARING", 8},
    {" MSG", 9},          {" MSG TO:", 10},
    {" QUERY", 11},       {" QUERY MSGS", 12},
    {" QUERY MSGS?", 12}, {" QUERY CALL", 13},
    {" GRID", 15},        {" INFO?", 16},
    {" INFO", 17},        {" FB", 18},
    {" HW CPY?", 19},     {" SK", 20},
    {" RR", 21},          {" QSL?", 22},
    {" QSL", 23},         {" CMD", 24},
    {" SNR", 25},         {" NO", 26},
    {" YES", 27},         {" 73", 28},
    {" NACK", 2},         {" ACK", 14},
    {" HEARTBEAT SNR", 29}, {" AGN?", 30},
    {"  ", 31},           {" ", 31},
};

inline constexpr int kBaseCallCount = 37 * 36 * 10 * 27 * 27 * 27;

inline const std::unordered_map<std::string, int>& base_calls() {
  static const std::unordered_map<std::string, int> map = [] {
    std::unordered_map<std::string, int> out;
    const std::vector<std::string> keys = {
        "<....>",     "@ALLCALL",   "@JS8NET",    "@DX/NA",     "@DX/SA",
        "@DX/EU",     "@DX/AS",     "@DX/AF",     "@DX/OC",     "@DX/AN",
        "@REGION/1",  "@REGION/2",  "@REGION/3",  "@GROUP/0",   "@GROUP/1",
        "@GROUP/2",   "@GROUP/3",   "@GROUP/4",   "@GROUP/5",   "@GROUP/6",
        "@GROUP/7",   "@GROUP/8",   "@GROUP/9",   "@COMMAND",   "@CONTROL",
        "@NET",       "@NTS",       "@RESERVE/0", "@RESERVE/1", "@RESERVE/2",
        "@RESERVE/3", "@RESERVE/4", "@APRSIS",    "@RAGCHEW",   "@JS8",
        "@EMCOMM",    "@ARES",      "@MARS",      "@AMRRON",    "@RACES",
        "@RAYNET",    "@RADAR",     "@SKYWARN",   "@CQ",        "@HB",
        "@QSO",       "@QSOPARTY",  "@CONTEST",   "@FIELDDAY",  "@SOTA",
        "@IOTA",      "@POTA",      "@QRP",       "@QRO",
    };
    for (std::size_t i = 0; i < keys.size(); ++i) {
      out.emplace(keys[i], kBaseCallCount + static_cast<int>(i) + 1);
    }
    return out;
  }();
  return map;
}

inline const std::vector<std::pair<int, std::string>> kCqTypes = {
    {0, "CQ CQ CQ"}, {1, "CQ DX"},     {2, "CQ QRP"}, {3, "CQ CONTEST"},
    {4, "CQ FIELD"}, {5, "CQ FD"},     {6, "CQ CQ"},  {7, "CQ"},
};

inline const std::vector<std::pair<int, std::string>> kHeartbeatTypes = {
    {0, "HB"}, {1, "HB"}, {2, "HB"}, {3, "HB"},
    {4, "HB"}, {5, "HB"}, {6, "HB"}, {7, "HB"},
};

enum class FrameType : std::uint8_t {
  Heartbeat = 0,
  Compound = 1,
  CompoundDirected = 2,
  Directed = 3,
  Data = 4,
  DataCompressed = 6,
};

inline constexpr std::uint16_t kBaseGridCount = 180 * 180;
inline constexpr std::uint16_t kUserGridStart =
    static_cast<std::uint16_t>(kBaseGridCount + 10);
inline constexpr std::uint16_t kMaxGrid =
    static_cast<std::uint16_t>((1 << 15) - 1);

}  // namespace js8core::protocol::varicode::tables

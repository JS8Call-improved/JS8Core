#pragma once

namespace js8core::protocol {

// Compact UTC time-of-day encoding used by existing JS8 consumers.
// Format: HHMMSS in base ten; neither separator nor timezone is stored.
constexpr int code_time(int hour, int minute, int second) noexcept {
  return (hour * 100 + minute) * 100 + second;
}

struct HourMinuteSecond {
  int hour;
  int minute;
  int second;
};

constexpr HourMinuteSecond decode_time(int packed) noexcept {
  return HourMinuteSecond{packed / 10000, (packed / 100) % 100, packed % 100};
}

}  // namespace js8core::protocol

#pragma once

#include <cmath>
#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <unordered_map>

namespace js8core::decoder {

// Frame identity is shared by event deduplication and the nearby-report guard.
struct Frame {
  int type;
  std::string text;

  bool operator==(Frame const&) const noexcept = default;

  struct Hash {
    std::size_t operator()(Frame const& frame) const noexcept {
      std::size_t const a = std::hash<int>{}(frame.type);
      std::size_t const b = std::hash<std::string>{}(frame.text);
      return a ^ (b + 0x9e3779b9u + (a << 6) + (a >> 2));
    }
  };

  using Map = std::unordered_map<Frame, int, Hash>;
};

struct ReportedFrame {
  float frequency;
  float offset;
  Frame frame;
};

// An OSD candidate near a different reported frame may be a subtraction
// residual. An exact nearby match takes precedence over any conflict.
inline bool different_frame_near(std::span<ReportedFrame const> reported,
                                 float frequency, float offset, Frame const& candidate) {
  constexpr float kLastTryNearHz = 3.0f;
  constexpr float kLastTryNearSeconds = 0.05f;
  bool near = false;
  for (auto const& entry : reported) {
    if (std::abs(frequency - entry.frequency) > kLastTryNearHz ||
        std::abs(offset - entry.offset) > kLastTryNearSeconds) {
      continue;
    }
    if (entry.frame == candidate) return false;
    near = true;
  }
  return near;
}

}  // namespace js8core::decoder

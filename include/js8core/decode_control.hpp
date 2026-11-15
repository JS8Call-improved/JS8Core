#pragma once

#include <functional>

namespace js8core {

// Per-call cooperative controls. Callbacks run on the decoder worker, must be
// nonblocking, and should use a monotonic clock. Empty controls allow all work.
struct DecodeControl {
  std::function<bool()> cancel_requested;
  std::function<bool()> optional_allowed;

  bool is_canceled() const noexcept {
    try { return cancel_requested && cancel_requested(); }
    catch (...) { return true; }
  }
  bool allow_optional() const noexcept {
    if (is_canceled()) return false;
    try { return !optional_allowed || optional_allowed(); }
    catch (...) { return false; }
  }
};

}  // namespace js8core

#include "js8core/protocol/submode.hpp"
#include "js8core/decoder.hpp"
#include "js8core/version.hpp"

static_assert(js8core::EngineConfig{}.symbol_tracking == js8core::SymbolTracking::Both);
static_assert(js8core::EngineConfig{}.tracked_sic);

int main() {
  if (js8core::version() != JS8CORE_VERSION_STRING) return 4;
  if (js8core::protocol::submodes().empty()) return 1;
  auto const legacy_binding = &js8core::legacy_decode;
  js8core::DecodeState state; // No active submodes: no sample buffer is required.
  if (legacy_binding(state, {}) != 0) return 2;
  js8core::DecodeControl control;
  control.cancel_requested = [] { return true; };
  bool canceled = false;
  js8core::decode_with_control(state, [&](auto const& event) {
    if (auto summary = std::get_if<js8core::events::DecodeFinished>(&event)) canceled = summary->canceled;
  }, control);
  return canceled ? 0 : 3;
}

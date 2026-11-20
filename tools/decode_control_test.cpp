#include "tracking_waveform.hpp"
#include "js8core/decoder.hpp"
#include "src/decoder/symbol_tracking.hpp"
#include "src/decoder/tracked_reference.hpp"

#include <cstdio>
#include <stdexcept>

using namespace js8core;

int main() {
  int failures = 0, checks = 0;
  auto check = [&](bool okay, char const* name) {
    ++checks;
    if (!okay) { std::fprintf(stderr, "FAIL control: %s\n", name); ++failures; }
  };
  test::TrackingSignal signal;
  signal.symbol_seconds = 384.0 / 12000;
  signal.start_seconds = 0.113037;
  auto const& costas = protocol::costas(protocol::CostasType::Modified);
  legacy_encode(2, costas, "HELLO-WORLD-", signal.tones.data());
  DecodeState state;
  state.samples = test::render_tracking_audio(std::array{test::TrackingWaveform(signal)}, 12000, 48000, 300, 5);
  state.samples.resize(720000);
  state.params.nfa = 1400; state.params.nfb = 1800; state.params.nfqso = 1500;
  state.params.nsubmodes = 1 << 4; state.params.kszI = 48000;
  auto run = [&](DecodeControl const& control) {
    std::pair<std::vector<events::Decoded>, events::DecodeFinished> result;
    decode_with_control(state, [&](auto const& event) {
      if (auto d = std::get_if<events::Decoded>(&event)) result.first.push_back(*d);
      if (auto f = std::get_if<events::DecodeFinished>(&event)) result.second = *f;
    }, control);
    return result;
  };
  auto baseline = run({});
  check(!baseline.first.empty(), "ordinary positive fixture decodes");
  auto const legacy_binding = &legacy_decode;
  std::vector<events::Decoded> compatible;
  legacy_binding(state, [&](auto const& event) {
    if (auto d = std::get_if<events::Decoded>(&event)) compatible.push_back(*d);
  });
  check(compatible.size() == baseline.first.size() && !compatible.empty() &&
        compatible.front().data == baseline.first.front().data,
        "legacy function-pointer API remains unambiguous and equivalent");
  DecodeControl denied;
  int probes = 0;
  denied.optional_allowed = [&] { return ++probes != 1; }; // A denial must remain latched.
  auto limited = run(denied);
  check(limited.second.optional_work_skipped && !limited.second.canceled, "optional denial reported separately");
  check(probes == 1, "optional work cannot resume after a transient denial");
  check(limited.first.size() == baseline.first.size(), "ordinary results survive optional denial");
  if (!baseline.first.empty() && !limited.first.empty()) {
    auto const& a = baseline.first.front(); auto const& b = limited.first.front();
    check(a.data == b.data && a.type == b.type && a.snr == b.snr && a.xdt == b.xdt &&
          a.frequency == b.frequency && a.quality == b.quality, "ordinary metadata unchanged");
  }
  DecodeControl canceled;
  canceled.cancel_requested = [] { return true; };
  auto stopped = run(canceled);
  check(stopped.first.empty() && stopped.second.canceled && stopped.second.tracked_cancellations == 0,
        "canceled snapshots publish no frames or residual commits");
  int cancel_probes = 0;
  canceled.cancel_requested = [&] { return ++cancel_probes == 20; };
  auto transient_cancel = run(canceled);
  check(transient_cancel.first.empty() && transient_cancel.second.canceled && cancel_probes == 20,
        "mid-search cancellation remains latched even if the caller would re-enable work");
  DecodeControl throws;
  throws.optional_allowed = []() -> bool { throw std::runtime_error("optional policy"); };
  auto failed_optional = run(throws);
  check(failed_optional.second.optional_work_skipped && !failed_optional.first.empty(), "optional exception fails closed");
  throws.cancel_requested = []() -> bool { throw std::runtime_error("cancel policy"); };
  check(run(throws).second.canceled, "cancel exception fails closed");

  std::vector<std::complex<float>> channel(1100, {1, 0});
  int calls = 0;
  DecodeControl expire;
  expire.optional_allowed = [&] { return ++calls < 12; };
  auto estimate = decoder::estimate_pilot_tracking(channel, 375, 12, 40, costas, &expire);
  check(!estimate.frequency_valid && !estimate.timing_valid && calls >= 12, "incomplete pilot search discarded");
  decoder::TrackedTrajectory trajectory{12000, 384, 1200, 1500, 0, 0, 0};
  calls = 0;
  auto ref = decoder::make_tracked_reference(signal.tones, trajectory, 48000, &expire);
  check(ref.empty() && calls >= 12, "reference rendering stops cooperatively");
  auto complete = decoder::make_tracked_reference(signal.tones, trajectory, 48000);
  std::vector<float> audio(48000, 1000);
  auto original = audio;
  calls = 0;
  auto residual = decoder::cancel_tracked_reference(audio, complete, 384, &expire);
  check(residual.residual.empty() && audio == original && calls >= 12, "interrupted cancellation never commits partial audio");
  for (std::size_t i = 0; i < audio.size(); ++i) audio[i] = 5000 * complete[i].imag();
  original = audio;
  calls = 0;
  expire.optional_allowed = [&] { return ++calls < 100; };
  auto interrupted_output = decoder::cancel_tracked_reference(audio, complete, 384, &expire);
  check(interrupted_output.residual.empty() && audio == original && calls >= 100,
        "late cancellation discards a partially assembled residual");
  decoder::PilotTrackingEstimate correction;
  correction.frequency_valid = true;
  correction.frequency_at_start_hz = 1.0;
  calls = 0;
  expire.optional_allowed = [&] { return ++calls < 10; };
  auto extracted = decoder::extract_tracked_symbols(channel, 375, 12, 40, correction, &expire);
  check(extracted.empty() && calls >= 10, "interrupted symbol extraction discards partial output");
  std::printf("%d decode admission/cancellation checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}

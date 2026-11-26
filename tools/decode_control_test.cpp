#include "tracking_waveform.hpp"
#include "js8core/decoder.hpp"
#include "src/decoder/decode_quality.hpp"
#include "src/decoder/frame_history.hpp"
#include "src/decoder/symbol_tracking.hpp"
#include "src/decoder/tracked_reference.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <stdexcept>
#include <tuple>

using namespace js8core;

int main() {
  int failures = 0, checks = 0;
  auto check = [&](bool okay, char const* name) {
    ++checks;
    if (!okay) { std::fprintf(stderr, "FAIL control: %s\n", name); ++failures; }
  };
  using decoder::hard_errors_acceptable;
  check(hard_errors_acceptable(6, 39, 2.0f) &&
        !hard_errors_acceptable(6, 40, 2.0f),
        "OSD shares the late-pass hard-error limit");
  check(hard_errors_acceptable(6, 35, 1.999f) &&
        !hard_errors_acceptable(6, 36, 1.999f) &&
        hard_errors_acceptable(6, 36, 2.0f),
        "OSD shares the low-sync hard-error limit only below 2.0");
  check(!hard_errors_acceptable(6, -1, 2.0f),
        "OSD failure is never acceptable");
  check(hard_errors_acceptable(4, 30, 2.0f) &&
        !hard_errors_acceptable(4, 31, 2.0f) &&
        hard_errors_acceptable(5, 31, 2.0f),
        "the 30-error cap remains specific to pass 4");
  check(hard_errors_acceptable(2, 59, 2.0f) &&
        !hard_errors_acceptable(2, 60, 2.0f),
        "early passes keep their existing hard-error limit");

  decoder::Frame const candidate{4, "Ux2zqX++++++"};
  decoder::Frame const other_type{5, candidate.text};
  decoder::Frame const other_text{candidate.type, "HELLO-WORLD-"};
  using decoder::ReportedFrame;
  using decoder::different_frame_near;
  check(different_frame_near(std::array{ReportedFrame{1503.0f, 0.05f, other_type}},
                             1500.0f, 0.0f, candidate),
        "same text with a different type conflicts at the nearby window boundary");
  check(different_frame_near(std::array{ReportedFrame{1500.0f, 0.0f, other_text}},
                             1500.0f, 0.0f, candidate),
        "same type with different text still conflicts");
  check(!different_frame_near(std::array{ReportedFrame{1500.0f, 0.0f, other_type},
                                         ReportedFrame{1501.0f, 0.01f, candidate}},
                              1500.0f, 0.0f, candidate),
        "an exact nearby frame match overrides a conflict");
  check(!different_frame_near(std::array{ReportedFrame{1504.0f, 0.0f, other_type},
                                         ReportedFrame{1500.0f, 0.051f, other_type}},
                              1500.0f, 0.0f, candidate),
        "reports outside either nearby limit do not conflict");
  check(different_frame_near(std::array{ReportedFrame{1500.0f, 0.0f, other_type},
                                        ReportedFrame{1504.0f, 0.0f, candidate}},
                             1500.0f, 0.0f, candidate),
        "an exact match outside the nearby window does not override a conflict");

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
  using CandidateTrace = std::vector<std::tuple<float, float, int>>;
  auto run = [](DecodeState const& state, DecodeControl const& control,
                CandidateTrace* candidates = nullptr) {
    std::pair<std::vector<events::Decoded>, events::DecodeFinished> result;
    decode_with_control(state, [&](auto const& event) {
      if (auto d = std::get_if<events::Decoded>(&event)) result.first.push_back(*d);
      if (auto f = std::get_if<events::DecodeFinished>(&event)) result.second = *f;
      if (auto s = std::get_if<events::SyncState>(&event);
          candidates && s && s->kind == events::SyncState::Kind::Candidate)
        candidates->emplace_back(s->frequency, s->dt, s->sync.candidate);
    }, control);
    return result;
  };
  auto baseline = run(state, {});
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
  auto limited = run(state, denied);
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
  auto stopped = run(state, canceled);
  check(stopped.first.empty() && stopped.second.canceled && stopped.second.tracked_cancellations == 0,
        "canceled snapshots publish no frames or residual commits");
  int cancel_probes = 0;
  canceled.cancel_requested = [&] { return ++cancel_probes == 20; };
  auto transient_cancel = run(state, canceled);
  check(transient_cancel.first.empty() && transient_cancel.second.canceled && cancel_probes == 20,
        "mid-search cancellation remains latched even if the caller would re-enable work");
  DecodeControl throws;
  throws.optional_allowed = []() -> bool { throw std::runtime_error("optional policy"); };
  auto failed_optional = run(state, throws);
  check(failed_optional.second.optional_work_skipped && !failed_optional.first.empty(), "optional exception fails closed");
  throws.cancel_requested = []() -> bool { throw std::runtime_error("cancel policy"); };
  check(run(state, throws).second.canceled, "cancel exception fails closed");

  // A weak fast-data frame ("LAST TRY") needs OSD; a stronger frame in the
  // same window must still decode after OSD is denied or interrupted.
  constexpr char weak_frame[] = "Ux2zqX++++++";
  auto weak = signal;
  weak.amplitude = 3400;
  legacy_encode(4, costas, weak_frame, weak.tones.data());
  auto strong = signal;
  strong.base_hz = 1700;
  strong.amplitude = 7500;
  DecodeState osd_state;
  osd_state.params = state.params;
  osd_state.params.nfa = 1470;
  osd_state.params.nfb = 1740;
  osd_state.params.syncStats = true;
  osd_state.params.symbol_tracking = SymbolTracking::Disabled;
  osd_state.params.deep_sync = false;
  osd_state.samples = test::render_tracking_audio(
      std::array{test::TrackingWaveform(weak), test::TrackingWaveform(strong)}, 12000, 48000);
  // Use the generator's bits directly so the noise is identical across standard libraries.
  std::mt19937 rng(43);
  for (auto& sample : osd_state.samples) {
    int const noise = (static_cast<int>(rng() >> 16) - 32768) * 51000 / 65536;
    sample = static_cast<std::int16_t>(std::clamp(sample + noise, -32768, 32767));
  }
  osd_state.samples.resize(720000);

  auto contains = [](auto const& result, char const* text, int type) {
    return std::any_of(result.first.begin(), result.first.end(), [&](auto const& frame) {
      return frame.data == text && frame.type == type;
    });
  };
  CandidateTrace osd_candidates, bp_candidates;
  auto osd_baseline = run(osd_state, {}, &osd_candidates);
  check(contains(osd_baseline, weak_frame, 4) && contains(osd_baseline, "HELLO-WORLD-", 2) &&
        !osd_baseline.second.optional_work_skipped && !osd_baseline.second.canceled,
        "admitted OSD recovers the weak frame alongside the ordinary frame");
  DecodeControl osd_limit;
  osd_limit.optional_allowed = [] { return false; };
  auto osd_denied = run(osd_state, osd_limit, &bp_candidates);
  check(!contains(osd_denied, weak_frame, 4) && contains(osd_denied, "HELLO-WORLD-", 2) &&
        osd_denied.second.optional_work_skipped && !osd_denied.second.canceled,
        "OSD entry denial does not stop ordinary decoding");
  check(std::count_if(bp_candidates.begin(), bp_candidates.end(), [](auto const& candidate) {
          return std::abs(std::get<0>(candidate) - 1500.0f) < 5.0f;
        }) >= 2, "ordinary subtraction leads to another search of the weak candidate");
  check(osd_candidates == bp_candidates,
        "reporting the weak OSD frame leaves subsequent candidate searches unchanged");
  int osd_probes = 0;
  osd_limit.optional_allowed = [&] { return ++osd_probes != 16; };
  auto osd_interrupted = run(osd_state, osd_limit);
  check(osd_probes == 16 && !contains(osd_interrupted, weak_frame, 4) &&
        contains(osd_interrupted, "HELLO-WORLD-", 2) &&
        osd_interrupted.second.optional_work_skipped && !osd_interrupted.second.canceled,
        "mid-search OSD denial is latched; no weak frame is published");

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

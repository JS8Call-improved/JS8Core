#include "tracking_waveform.hpp"

#include <charconv>
#include <chrono>
#include <cstdio>
#include <string_view>

#include "js8core/decoder.hpp"
#include "js8core/protocol/constants.hpp"
#include "js8core/protocol/submode.hpp"

using namespace js8core;

static_assert(EngineConfig{}.symbol_tracking == SymbolTracking::Both);
static_assert(EngineConfig{}.tracked_sic);
static_assert(DecodeParams{}.symbol_tracking == SymbolTracking::Both);
static_assert(DecodeParams{}.tracked_sic);

namespace {
struct Impairment {
  char const* name;
  double offset_hz = 0.0;
  double drift_hz_s = 0.0;
  double clock_ppm = 0.0;
  double frequency_wander_hz = 0.0;
  double timing_wander_s = 0.0;
  double fade_depth = 0.0;
  bool interferer = false;
  bool noise_only = false;
  double drift_tones_per_frame = 0.0;
  double noise_sigma = 1200.0;
  double amplitude = 5000.0;
  double base_hz = 1501.7;
  bool single_tone = false;
  double other_amplitude_ratio = 2.0;
  double other_tone_offset = 2.0;
  double other_drift_hz_s = -0.2;
};

constexpr Impairment kCases[] = {
    {"stationary"},
    {"offset-positive", 1.7},
    {"offset-negative", -1.7},
    {"drift-positive", 0.0, 0.5},
    {"drift-negative", 0.0, -0.5},
    {"clock-positive", 0.0, 0.0, 1500.0},
    {"clock-negative", 0.0, 0.0, -1500.0},
    {"wander", 0.0, 0.0, 0.0, 1.0, 0.004},
    {"fade", 0.0, 0.3, 300.0, 0.0, 0.0, 0.9},
    {"interference", 0.0, 0.3, 300.0, 0.0, 0.0, 0.0, true},
    {"noise-only", 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, false, true},
    {.name = "tracking-drift", .drift_tones_per_frame = 1.4, .noise_sigma = 10000.0,
     .amplitude = 4500.0, .base_hz = 1508.125},
    {.name = "tracking-clock", .clock_ppm = 5000.0, .noise_sigma = 5000.0, .amplitude = 1500.0},
    {.name = "single-tone", .single_tone = true},
    {.name = "tracking-overlap", .interferer = true, .drift_tones_per_frame = 1.4,
     .noise_sigma = 5000.0, .amplitude = 4500.0, .base_hz = 1508.125,
     .other_amplitude_ratio = 0.35, .other_tone_offset = 0.7, .other_drift_hz_s = 0.0},
};

void set_window(DecodeParams& p, protocol::SubmodeId id, int samples, int start = 0) {
  switch (id) {
    case protocol::SubmodeId::A: p.kszA = samples; p.kposA = start; break;
    case protocol::SubmodeId::B: p.kszB = samples; p.kposB = start; break;
    case protocol::SubmodeId::C: p.kszC = samples; p.kposC = start; break;
    case protocol::SubmodeId::E: p.kszE = samples; p.kposE = start; break;
    case protocol::SubmodeId::I: p.kszI = samples; p.kposI = start; break;
  }
}
}  // namespace

int main(int argc, char** argv) {
  bool sweep = false, valid = true, require_tracked = false, wrap = false;
  bool tracked_sic = false, require_cancellation = false, require_followup = false;
  bool library_defaults = false;
  int seeds = 1;
  std::string_view selected_case;
  std::string_view selected_mode;
  SymbolTracking tracking = SymbolTracking::Disabled;
  for (int arg = 1; arg < argc && valid; ++arg) {
    std::string_view option(argv[arg]);
    if (option == "--sweep") {
      sweep = true;
      seeds = 3;
      if (arg + 1 < argc && !std::string_view(argv[arg + 1]).starts_with("--")) {
        std::string_view value(argv[++arg]);
        auto result = std::from_chars(value.data(), value.data() + value.size(), seeds);
        valid = result.ec == std::errc{} && result.ptr == value.data() + value.size() &&
                seeds >= 1 && seeds <= 1000;
      }
    } else if (option == "--require-tracked") require_tracked = true;
    else if (option == "--wrap") wrap = true;
    else if (option == "--tracked-sic") tracked_sic = true;
    else if (option == "--require-cancellation") require_cancellation = true;
    else if (option == "--require-followup") require_followup = true;
    else if (option == "--library-defaults") library_defaults = true;
    else if (option == "--mode" && arg + 1 < argc) {
      selected_mode = argv[++arg];
      valid = selected_mode.size() == 1 && std::string_view("ABCEI").find(selected_mode) != std::string_view::npos;
    }
    else if (option == "--case" && arg + 1 < argc) {
      selected_case = argv[++arg];
      valid = std::any_of(std::begin(kCases), std::end(kCases), [&](auto const& c) {
        return selected_case == c.name;
      });
    } else if (option == "--tracking" && arg + 1 < argc) {
      std::string_view value(argv[++arg]);
      if (value == "both") tracking = SymbolTracking::Both;
      else if (value == "frequency") tracking = SymbolTracking::Frequency;
      else if (value == "timing") tracking = SymbolTracking::Timing;
      else if (value == "disabled") tracking = SymbolTracking::Disabled;
      else valid = false;
    } else valid = false;
  }
  if (library_defaults) {
    tracking = DecodeParams{}.symbol_tracking;
    tracked_sic = DecodeParams{}.tracked_sic;
  }
  if (!valid) {
    std::fprintf(stderr, "Usage: %s [--sweep [seed-count: 1..1000]] "
                        "[--tracking disabled|frequency|timing|both] "
                        "[--case name] [--require-tracked] [--wrap] [--tracked-sic] "
                        "[--require-cancellation] [--require-followup] [--mode A|B|C|E|I] "
                        "[--library-defaults]\n", argv[0]);
    return 2;
  }

  constexpr int rate = protocol::kJs8RxSampleRate;
  constexpr char message[] = "HELLO-WORLD-";
  constexpr char other_message[] = "TEST12345678";
  int failures = 0, trials = 0, recovered = 0, tracked_total = 0;
  std::size_t cancellation_total = 0;
  int followup_total = 0;
  std::puts("mode,case,seed,base_hz,offset_hz,drift_hz_s,clock_ppm,frequency_wander_hz,"
            "timing_wander_s,fade_depth,amplitude,noise_sigma,baseline_decoded,expected_decoded,"
            "tracked_frames,baseline_preserved,unexpected_frames,baseline_ms,decode_ms,"
            "tracking_only_decoded,other_decoded,tracked_cancellations,after_sic_frames,"
            "tracking_only_ms,tracking_only_other_decoded");
  for (auto const& mode : protocol::submodes()) {
    if (!selected_mode.empty() && selected_mode != mode.name) continue;
    for (auto const& impairment : kCases) {
      if (!selected_case.empty() && selected_case != impairment.name) continue;
      if (selected_case.empty() && !sweep &&
          std::string_view(impairment.name) != "stationary" && !impairment.noise_only)
        continue;
      for (int seed = 0; seed < seeds; ++seed) {
        test::TrackingSignal signal;
        signal.symbol_seconds = static_cast<double>(mode.symbol_samples) / rate;
        signal.start_seconds = mode.start_delay_ms / 1000.0 + 0.013037;
        signal.base_hz = impairment.base_hz;
        signal.amplitude = impairment.amplitude;
        signal.phase_radians = seed * 0.71;
        signal.frequency_offset_hz = impairment.offset_hz;
        signal.frequency_slope_hz_s = impairment.drift_hz_s;
        signal.frequency_slope_hz_s += impairment.drift_tones_per_frame /
            (79.0 * signal.symbol_seconds * signal.symbol_seconds);
        signal.clock_ppm = impairment.clock_ppm;
        signal.frequency_wander_hz = impairment.frequency_wander_hz;
        signal.timing_wander_seconds = impairment.timing_wander_s;
        signal.fade_depth = impairment.fade_depth;
        signal.fade_center_s = 39.5 * signal.symbol_seconds;
        signal.fade_width_s = 5.0 * signal.symbol_seconds;
        auto const costas = protocol::costas(mode.id == protocol::SubmodeId::A
            ? protocol::CostasType::Original : protocol::CostasType::Modified);
        legacy_encode(2, costas, message, signal.tones.data());
        if (impairment.single_tone) signal.tones.fill(3);
        std::vector<test::TrackingWaveform> waves;
        if (!impairment.noise_only) waves.emplace_back(signal);
        if (impairment.interferer) {
          auto other = signal;
          other.base_hz += impairment.other_tone_offset / signal.symbol_seconds;
          other.start_seconds += 0.03;
          other.amplitude *= impairment.other_amplitude_ratio;
          other.clock_ppm = -200.0;
          other.frequency_slope_hz_s = impairment.other_drift_hz_s;
          legacy_encode(1, costas, other_message, other.tones.data());
          waves.emplace_back(other);
        }
        DecodeState state;
        // This first run is intentionally the historical disabled baseline,
        // independent of the library's enabled configuration defaults.
        state.params.symbol_tracking = SymbolTracking::Disabled;
        state.params.tracked_sic = false;
        double const noise = impairment.noise_sigma;
        state.samples = test::render_tracking_audio(waves, rate, mode.tx_seconds * rate,
                                                    noise, 0x51a700u + seed);
        state.samples.resize(protocol::kJs8NtMax * rate, 0);
        if (wrap) std::rotate(state.samples.begin(), state.samples.begin() + rate, state.samples.end());
        state.params.nfqso = 1502;
        state.params.nfa = 1350;
        state.params.nfb = 1850;
        state.params.nsubmodes = 1 << static_cast<int>(mode.id);
        set_window(state.params, mode.id, mode.tx_seconds * rate,
                   wrap ? (protocol::kJs8NtMax - 1) * rate : 0);
        std::size_t cancellations = 0;
        auto run = [&](std::vector<events::Decoded>& results) {
          cancellations = 0;
          auto const start = std::chrono::steady_clock::now();
          legacy_decode(state, [&](events::Variant const& event) {
            if (auto const* d = std::get_if<events::Decoded>(&event)) results.push_back(*d);
            if (auto const* f = std::get_if<events::DecodeFinished>(&event)) cancellations = f->tracked_cancellations;
          });
          return std::chrono::duration<double, std::milli>(
              std::chrono::steady_clock::now() - start).count();
        };
        std::vector<events::Decoded> baseline;
        double const baseline_ms = run(baseline);
        std::vector<events::Decoded> results = baseline;
        double ms = baseline_ms;
        if (tracking != SymbolTracking::Disabled) {
          state.params.symbol_tracking = tracking;
          results.clear();
          ms = run(results);
        }
        auto const tracking_only = results;
        double const tracking_ms = ms;
        bool preserved = true;
        if (tracked_sic) {
          state.params.tracked_sic = true;
          results.clear();
          ms = run(results);
          preserved = results.size() >= tracking_only.size();
          for (std::size_t i = 0; preserved && i < tracking_only.size(); ++i) {
            auto const& a = tracking_only[i];
            auto const& b = results[i];
            preserved = a.data == b.data && a.type == b.type && a.utc == b.utc &&
                a.snr == b.snr && a.xdt == b.xdt && a.frequency == b.frequency &&
                a.quality == b.quality && a.mode == b.mode &&
                a.tracking_used == b.tracking_used && !b.after_tracked_sic;
          }
        }
        bool const baseline_found = std::any_of(baseline.begin(), baseline.end(), [&](auto const& d) {
          return d.data == message && d.type == 2;
        });
        bool found = false, other_found = false;
        bool const tracking_found = std::any_of(tracking_only.begin(), tracking_only.end(), [&](auto const& d) {
          return !impairment.noise_only && !impairment.single_tone && d.data == message && d.type == 2;
        });
        bool const tracking_other_found = std::any_of(tracking_only.begin(), tracking_only.end(), [&](auto const& d) {
          return impairment.interferer && d.data == other_message && d.type == 1;
        });
        int unexpected = 0, tracked = 0, after_sic = 0;
        for (auto const& original : baseline)
          preserved &= std::any_of(results.begin(), results.end(), [&](auto const& d) {
            return !d.tracking_used && d.data == original.data && d.type == original.type &&
                d.utc == original.utc && d.snr == original.snr && d.xdt == original.xdt &&
                d.frequency == original.frequency && d.quality == original.quality && d.mode == original.mode;
          });
        for (auto const& d : results) {
          if (tracked && !d.tracking_used) preserved = false;
          tracked += d.tracking_used;
          after_sic += d.after_tracked_sic;
          if (d.after_tracked_sic && (!d.tracking_used || !cancellations)) preserved = false;
          if (!impairment.noise_only && !impairment.single_tone &&
              d.data == message && d.type == 2) found = true;
          else if (impairment.interferer && d.data == other_message && d.type == 1) other_found = true;
          else ++unexpected;
        }
        if (cancellations > 2 || ((tracking != SymbolTracking::Both || !tracked_sic) && cancellations)) preserved = false;
        std::printf("%.*s,%s,%d,%.3f,%.3f,%.3f,%.1f,%.3f,%.6f,%.3f,%.1f,%.1f,%d,%d,%d,%d,%d,%.3f,%.3f,%d,%d,%zu,%d,%.3f,%d\n",
            static_cast<int>(mode.name.size()), mode.name.data(), impairment.name, seed,
            signal.base_hz, impairment.offset_hz, signal.frequency_slope_hz_s, impairment.clock_ppm,
            impairment.frequency_wander_hz, impairment.timing_wander_s,
            impairment.fade_depth, signal.amplitude, noise, baseline_found, found, tracked, preserved,
            unexpected, baseline_ms, ms, tracking_found, other_found, cancellations, after_sic, tracking_ms,
            tracking_other_found);
        ++trials;
        recovered += found;
        tracked_total += tracked;
        cancellation_total += cancellations;
        followup_total += after_sic;
        // Impaired reception rates are measurements, not acceptance targets.
        // Stationary compatibility and any unexpected payload are hard checks.
        if (!preserved || unexpected || (std::string_view(impairment.name) == "stationary" && !found))
          ++failures;
      }
    }
  }
  if (require_tracked && tracked_total == 0) {
    std::fprintf(stderr, "FAIL: no frame exercised successful tracked decoding\n");
    ++failures;
  }
  if ((require_cancellation && cancellation_total == 0) || (require_followup && followup_total == 0)) {
    std::fprintf(stderr, "FAIL: required tracked cancellation/follow-up was not exercised\n");
    ++failures;
  }
  std::fprintf(stderr, "%d trials, %d target recoveries, %d tracked frames, %zu cancellations, "
                       "%d follow-up frames, %d failing checks\n",
               trials, recovered, tracked_total, cancellation_total, followup_total, failures);
  return failures ? 1 : 0;
}

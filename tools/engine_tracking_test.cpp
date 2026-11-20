#include "tracking_waveform.hpp"
#include "js8core/decoder.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <span>
#include <string>

using namespace js8core;
using namespace std::chrono_literals;

namespace {
struct Observer {
  std::mutex mutex;
  std::condition_variable cv;
  std::vector<events::Decoded> frames;
  std::vector<events::DecodeFinished> finishes;
  std::vector<int> masks;
  bool block = false, entered = false, release = false;
  bool block_spectrum = false, spectrum_entered = false;
  int returned = 0;
  int alignments = 0;
  std::vector<std::string> errors;

  EngineCallbacks callbacks() {
    EngineCallbacks c;
    c.on_event = [this](auto const& event) {
      std::unique_lock lock(mutex);
      if (block_spectrum && !spectrum_entered && std::holds_alternative<events::Spectrum>(event)) {
        spectrum_entered = true; cv.notify_all();
        cv.wait_for(lock, 20s, [&] { return release; });
      }
      if (auto d = std::get_if<events::Decoded>(&event)) frames.push_back(*d);
      if (auto f = std::get_if<events::DecodeFinished>(&event)) finishes.push_back(*f);
      if (auto s = std::get_if<events::DecodeStarted>(&event)) masks.push_back(s->submodes);
      cv.notify_all();
    };
    c.on_error = [this](std::string_view error) {
      std::lock_guard lock(mutex); errors.emplace_back(error); cv.notify_all();
    };
    c.on_log = [this](LogLevel, std::string_view text) {
      if (text.find("Ring buffer aligned to UTC minute") != std::string_view::npos) {
        std::lock_guard lock(mutex); ++alignments; return;
      }
      if (text.find("legacy_decode returned:") != std::string_view::npos) {
        std::lock_guard lock(mutex); ++returned; cv.notify_all(); return;
      }
      if (text.find("Calling legacy_decode:") == std::string_view::npos) return;
      std::unique_lock lock(mutex);
      if (!block || entered) return;
      entered = true; cv.notify_all();
      cv.wait_for(lock, 20s, [&] { return release; });
    };
    return c;
  }
  bool wait_finished(std::size_t count) {
    std::unique_lock lock(mutex);
    return cv.wait_for(lock, 20s, [&] { return finishes.size() >= count || !errors.empty(); }) && errors.empty();
  }
  bool wait_blocked() {
    std::unique_lock lock(mutex);
    return cv.wait_for(lock, 10s, [&] { return entered; });
  }
  void unblock() { { std::lock_guard lock(mutex); release = true; } cv.notify_all(); }
  bool wait_spectrum() {
    std::unique_lock lock(mutex);
    return cv.wait_for(lock, 10s, [&] { return spectrum_entered; });
  }
  bool wait_returned() {
    std::unique_lock lock(mutex);
    return cv.wait_for(lock, 20s, [&] { return returned > 0; });
  }
};

SteadyTimePoint origin() {
  auto const steady = SteadyClock::now();
  auto const wall = SystemClock::now();
  auto const ms = std::chrono::duration_cast<std::chrono::milliseconds>(wall.time_since_epoch()).count();
  auto const minute = SystemClock::time_point(std::chrono::milliseconds(ms - ms % 60000 + 10));
  return steady + std::chrono::duration_cast<SteadyDuration>(minute - wall);
}

std::vector<std::int16_t> waveform(bool overlap, char const* message = "HELLO-WORLD-", unsigned seed = 1) {
  test::TrackingSignal strong;
  strong.symbol_seconds = 384.0 / 12000;
  strong.start_seconds = 0.113037;
  strong.base_hz = overlap ? 1508.125 : 1501.7;
  strong.amplitude = overlap ? 4500 : 5000;
  strong.phase_radians = seed * 0.71;
  strong.frequency_slope_hz_s = overlap ? 1.4 / (79 * strong.symbol_seconds * strong.symbol_seconds) : 0;
  auto const& costas = protocol::costas(protocol::CostasType::Modified);
  legacy_encode(2, costas, message, strong.tones.data());
  std::vector<test::TrackingWaveform> waves{test::TrackingWaveform(strong)};
  if (overlap) {
    auto weak = strong;
    weak.start_seconds += 0.03;
    weak.base_hz += 0.7 / strong.symbol_seconds;
    weak.amplitude *= 0.35;
    weak.clock_ppm = -200;
    weak.frequency_slope_hz_s = 0;
    legacy_encode(1, costas, "TEST12345678", weak.tones.data());
    waves.emplace_back(weak);
  }
  return test::render_tracking_audio(waves, 12000, 48000, overlap ? 5000 : 300, 0x51a700u + seed);
}

template <typename Submit>
bool feed_to(Submit submit, std::span<std::int16_t const> audio, SteadyTimePoint start,
          int first = 120, int last = 39000) {
  for (int i = first; i < last;) {
    int const n = std::min(512, last - i);
    auto const stamp = start + std::chrono::duration_cast<SteadyDuration>(
        std::chrono::duration<double>((i - first) / 12000.0));
    AudioInputBuffer buffer{std::as_bytes(audio.subspan(i, n)), {12000, 1, SampleType::Int16}, stamp};
    if (!submit(buffer)) return false;
    i += n;
  }
  return true;
}

bool feed(Js8Engine& engine, std::span<std::int16_t const> audio, SteadyTimePoint start,
          int first = 120, int last = 39000) {
  return feed_to([&](AudioInputBuffer const& b) { return engine.submit_capture(b); }, audio, start, first, last);
}

class RetainingInput final : public AudioInput {
 public:
  std::vector<AudioInputHandler> buffers;
  std::vector<AudioErrorHandler> errors;
  bool start(AudioStreamParams const&, AudioInputHandler buffer, AudioErrorHandler error) override {
    buffers.push_back(std::move(buffer)); errors.push_back(std::move(error)); return true;
  }
  void stop() override {} // Models a callback copy already in flight at stop().
};

bool retired_adapter_callbacks() {
  Observer o;
  RetainingInput input;
  EngineConfig config;
  config.sample_rate_hz = 12000; config.submodes = 1 << 4; config.optional_decode_budget_ms = 10000;
  EngineDependencies deps; deps.audio_in = &input;
  auto engine = make_engine(config, o.callbacks(), deps);
  if (!engine->start()) return false;
  auto const retired_buffer = input.buffers.front();
  auto const retired_error = input.errors.front();
  engine->stop();
  if (!engine->start()) return false;
  int before = 0;
  { std::lock_guard lock(o.mutex); before = o.alignments; }
  auto old = waveform(false);
  AudioInputBuffer buffer{std::as_bytes(std::span(old).subspan(120, 512)),
                          {12000, 1, SampleType::Int16}, origin()};
  retired_buffer(buffer);
  retired_error("retired input callback");
  {
    std::lock_guard lock(o.mutex);
    if (o.alignments != before || !o.errors.empty()) return false;
  }
  auto fresh = waveform(false, "CQ-CQ-CQ-73-");
  auto const current = input.buffers.back();
  if (!feed_to([&](AudioInputBuffer const& b) { current(b); return true; }, fresh, origin()) ||
      !o.wait_finished(1)) return false;
  engine->stop();
  std::lock_guard lock(o.mutex);
  return o.errors.empty() && std::any_of(o.frames.begin(), o.frames.end(), [](auto const& d) {
    return d.data == "CQ-CQ-CQ-73-";
  });
}

bool positive_and_restart() {
  Observer o;
  EngineConfig config;
  config.sample_rate_hz = 12000;
  config.submodes = 1 << 4;
  config.optional_decode_budget_ms = 10000; // Instrumented builds are deliberately slower.
  auto engine = make_engine(config, o.callbacks(), {});
  bool strong = false, weak = false;
  // normal_distribution is not bit-identical across C++ standard libraries.
  // Exercise a fixed seed pool rather than requiring one marginal noise draw.
  for (unsigned seed = 0; seed < 8 && (!strong || !weak); ++seed) {
    { std::lock_guard lock(o.mutex); o.frames.clear(); o.finishes.clear(); }
    auto data = waveform(true, "HELLO-WORLD-", seed);
    if (!engine->start() || !feed(*engine, data, origin()) || !o.wait_finished(1)) return false;
    engine->stop();
    strong = false; weak = false;
    {
      std::lock_guard lock(o.mutex);
      for (auto const& d : o.frames) {
        strong |= d.data == "HELLO-WORLD-" && d.type == 2 && d.mode == 8 && d.tracking_used;
        weak |= d.data == "TEST12345678" && d.type == 1 && d.mode == 8 && d.tracking_used && d.after_tracked_sic;
      }
    }
  }
  if (!strong || !weak) return false;
  { std::lock_guard lock(o.mutex); o.frames.clear(); o.finishes.clear(); }
  auto data = waveform(false, "CQ-CQ-CQ-73-");
  if (!engine->start() || !feed(*engine, data, origin()) || !o.wait_finished(1)) return false;
  engine->stop();
  std::lock_guard lock(o.mutex);
  return std::any_of(o.frames.begin(), o.frames.end(), [](auto const& d) { return d.data == "CQ-CQ-CQ-73-"; }) &&
      std::none_of(o.frames.begin(), o.frames.end(), [](auto const& d) { return d.data == "HELLO-WORLD-" || d.data == "TEST12345678"; });
}

bool invalidation(bool restart) {
  Observer o; o.block = true;
  EngineConfig config;
  config.sample_rate_hz = 12000; config.submodes = 1 << 4; config.optional_decode_budget_ms = 10000;
  auto engine = make_engine(config, o.callbacks(), {});
  auto start = origin();
  auto old = waveform(true);
  if (!engine->start() || !feed(*engine, old, start) || !o.wait_blocked()) { o.unblock(); return false; }
  if (restart) { engine->stop(); if (!engine->start()) { o.unblock(); return false; } }
  auto fresh = waveform(false, "CQ-CQ-CQ-73-");
  // A trusted forward timestamp gap forces a new capture epoch without restart.
  bool const fed = feed(*engine, fresh, start + 60s);
  o.unblock();
  if (!fed || !o.wait_finished(1)) return false;
  engine->stop();
  std::lock_guard lock(o.mutex);
  return o.errors.empty() && std::any_of(o.frames.begin(), o.frames.end(), [](auto const& d) { return d.data == "CQ-CQ-CQ-73-"; }) &&
      std::none_of(o.frames.begin(), o.frames.end(), [](auto const& d) { return d.data == "HELLO-WORLD-" || d.data == "TEST12345678"; });
}

bool deadline_and_pressure(bool pressure, bool overrun = false) {
  Observer o; o.block = pressure;
  EngineConfig config;
  config.sample_rate_hz = 12000;
  config.submodes = pressure ? 15 : 1 << 4;
  config.optional_decode_budget_ms = pressure ? 10000 : 0;
  auto engine = make_engine(config, o.callbacks(), {});
  if (!engine->start()) return false;
  auto start = origin();
  if (!pressure) {
    auto data = waveform(false);
    if (!feed(*engine, data, start) || !o.wait_finished(1)) return false;
    engine->stop();
    std::lock_guard lock(o.mutex);
    return o.finishes[0].optional_work_skipped && std::any_of(o.frames.begin(), o.frames.end(), [](auto const& d) {
      return d.data == "HELLO-WORLD-" && !d.tracking_used;
    });
  }
  int const seconds = overrun ? 61 : 32;
  std::vector<std::int16_t> crowded(seconds * 12000);
  for (std::size_t i = 0; i < crowded.size(); ++i)
    crowded[i] = static_cast<std::int16_t>(1500 * (std::sin(test::kWaveTau * 1000 * i / 12000) +
        std::sin(test::kWaveTau * 1500 * i / 12000) + std::sin(test::kWaveTau * 1900 * i / 12000)));
  if (!feed(*engine, crowded, start, 120, 50000) || !o.wait_blocked()) { o.unblock(); return false; }
  auto const began = SteadyClock::now();
  bool const fed = feed(*engine, crowded, start + std::chrono::duration_cast<SteadyDuration>(
      std::chrono::duration<double>((50000 - 120) / 12000.0)), 50000, static_cast<int>(crowded.size()));
  double const capture_ms = std::chrono::duration<double, std::milli>(SteadyClock::now() - began).count();
  o.unblock();
  if (!fed || !o.wait_finished(overrun ? 1 : 2)) return false;
  engine->stop();
  std::lock_guard lock(o.mutex);
  std::printf("%d-second accelerated MULTI load: capture %.1f ms, decode requests completed %zu\n", seconds, capture_ms, o.finishes.size());
  return o.errors.empty() && o.frames.empty() && o.finishes.size() == (overrun ? 1u : 2u) &&
      (overrun || o.finishes[0].optional_work_skipped) &&
      o.masks.size() == o.finishes.size() && o.masks.back() == 15;
}

bool elapsed_deadline() {
  Observer o; o.block = true;
  EngineConfig config;
  config.sample_rate_hz = 12000; config.submodes = 1 << 4; config.optional_decode_budget_ms = 5;
  auto engine = make_engine(config, o.callbacks(), {});
  auto data = waveform(false);
  if (!engine->start() || !feed(*engine, data, origin()) || !o.wait_blocked()) { o.unblock(); return false; }
  {
    std::unique_lock lock(o.mutex);
    // Timed wait deliberately expires the real monotonic budget; no queued work.
    o.cv.wait_until(lock, SteadyClock::now() + 20ms, [] { return false; });
  }
  o.unblock();
  if (!o.wait_finished(1)) return false;
  engine->stop();
  std::lock_guard lock(o.mutex);
  return o.finishes[0].optional_work_skipped && !o.finishes[0].canceled &&
      std::any_of(o.frames.begin(), o.frames.end(), [](auto const& d) { return d.data == "HELLO-WORLD-"; });
}

bool queued_event_invalidation() {
  Observer o; o.block_spectrum = true;
  EngineConfig config;
  config.sample_rate_hz = 12000; config.submodes = 1 << 4; config.optional_decode_budget_ms = 10000;
  auto engine = make_engine(config, o.callbacks(), {});
  auto const start = origin();
  auto old = waveform(false);
  auto const rest = start + std::chrono::duration_cast<SteadyDuration>(std::chrono::duration<double>(512.0 / 12000));
  // Hold serial event delivery before any decode is due, then let a complete
  // old-epoch decode finish and queue its results behind the spectrum callback.
  if (!engine->start() || !feed(*engine, old, start, 120, 632) || !o.wait_spectrum() ||
      !feed(*engine, old, rest, 632, 39000) || !o.wait_returned()) { o.unblock(); return false; }
  auto fresh = waveform(false, "CQ-CQ-CQ-73-");
  bool const fed = feed(*engine, fresh, start + 60s);
  o.unblock();
  if (!fed || !o.wait_finished(1)) return false;
  engine->stop();
  std::lock_guard lock(o.mutex);
  return o.errors.empty() && o.finishes.size() == 1 &&
      std::any_of(o.frames.begin(), o.frames.end(), [](auto const& d) { return d.data == "CQ-CQ-CQ-73-"; }) &&
      std::none_of(o.frames.begin(), o.frames.end(), [](auto const& d) { return d.data == "HELLO-WORLD-" || d.data == "TEST12345678"; });
}
}  // namespace

int main() {
  if (!retired_adapter_callbacks()) { std::puts("FAIL: retired adapter callbacks entered a restarted session"); return 1; }
  if (!positive_and_restart()) { std::puts("FAIL: positive streaming tracking/SIC or restart"); return 1; }
  if (!invalidation(false)) { std::puts("FAIL: stale decode after capture discontinuity"); return 1; }
  if (!invalidation(true)) { std::puts("FAIL: stale decode after session restart"); return 1; }
  if (!deadline_and_pressure(false)) { std::puts("FAIL: deadline suppressed mandatory decoding"); return 1; }
  if (!deadline_and_pressure(true)) { std::puts("FAIL: queue pressure/coalescing load regression"); return 1; }
  if (!deadline_and_pressure(true, true)) { std::puts("FAIL: ring-horizon stale work was not discarded"); return 1; }
  if (!elapsed_deadline()) { std::puts("FAIL: elapsed optional deadline was not enforced"); return 1; }
  if (!queued_event_invalidation()) { std::puts("FAIL: queued old-epoch decode events leaked across a discontinuity"); return 1; }
  std::puts("PASS: positive capture-to-callback recovery, lifecycle, deadline and queue pressure");
  return 0;
}

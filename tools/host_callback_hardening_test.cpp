#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "js8core/engine.hpp"

using namespace js8core;
using namespace std::chrono_literals;

namespace {
class Output final : public AudioOutput {
 public:
  bool start(AudioStreamParams const& params, AudioOutputFill fill,
             AudioErrorHandler) override {
    format_ = params.format;
    fill_ = std::move(fill);
    active_ = true;
    return true;
  }
  void stop() override { ++stops_; active_ = false; }

  bool render(SampleType type, int channels, bool& has_audio) {
    if (!fill_ || !active_) return false;
    constexpr std::size_t kFrames = 4096;
    const std::size_t bytes_per_sample =
        type == SampleType::Float32 ? sizeof(float) : sizeof(std::int16_t);
    const std::size_t bytes = kFrames * static_cast<std::size_t>(channels) * bytes_per_sample;
    // Deliberate +1 offset: output is valid byte storage, not aligned
    // float/int16_t storage. Keep both guard bytes intact.
    std::vector<std::byte> raw(bytes + 2, std::byte{0x5a});
    auto fmt = format_;
    fmt.sample_type = type;
    fmt.channels = channels;
    AudioOutputBuffer buffer{
        std::span<std::byte>(raw.data() + 1, bytes), fmt, SteadyClock::now()};
    if (fill_(buffer) != bytes || raw.front() != std::byte{0x5a} ||
        raw.back() != std::byte{0x5a}) return false;
    // Float32 must obey the same [-1, +1] output contract as Int16,
    // including the gain-boost path. Read via memcpy: storage is unaligned.
    if (type == SampleType::Float32) {
      for (std::size_t offset = 1; offset <= bytes; offset += sizeof(float)) {
        float value = 0.0f;
        std::memcpy(&value, raw.data() + offset, sizeof(value));
        if (!std::isfinite(value) || value < -1.0f || value > 1.0f) return false;
      }
    }
    for (std::size_t i = 1; i <= bytes; ++i)
      if (raw[i] != std::byte{0}) has_audio = true;
    return true;
  }

  int stops() const { return stops_; }
 private:
  AudioFormat format_{};
  AudioOutputFill fill_;
  bool active_ = false;
  int stops_ = 0;
};

bool cross_engine_render_guard_and_unaligned_output() {
  Output output_b;
  EngineDependencies deps_b;
  deps_b.audio_out = &output_b;
  auto engine_b = make_engine(EngineConfig{}, {}, deps_b);

  Output output_a;
  EngineDependencies deps_a;
  deps_a.audio_out = &output_a;
  std::atomic<bool> reentered{false};
  EngineCallbacks callbacks;
  callbacks.on_log = [&](LogLevel, std::string_view message) {
    if (message.find("TX output format") == std::string_view::npos ||
        reentered.exchange(true)) return;
    // This runs during the output callback of engine A. It must stop B's
    // output, not confuse B with A's renderer and skip its adapter stop().
    engine_b->stop_transmit();
  };
  EngineConfig boosted_config;
  boosted_config.tx_output_gain_boost_enabled = true;
  auto engine_a = make_engine(boosted_config, callbacks, deps_a);
  if (!engine_a->start() || !engine_b->start()) return false;
  if (!engine_b->start_tune(1500, 0, 0) ||
      !engine_a->start_tune(1500, 0, 0)) return false;

  bool int16_audio = false;
  bool float_audio = false;
  for (int i = 0; i < 16; ++i) {
    if (!output_a.render(SampleType::Int16, 2, int16_audio) ||
        !output_a.render(SampleType::Float32, 1, float_audio)) return false;
  }
  bool const ok = reentered && output_b.stops() == 1 &&
                  !engine_b->is_transmitting() && int16_audio && float_audio;
  engine_a->stop();
  engine_b->stop();
  if (!ok) std::puts("FAIL: cross-engine render guard or unaligned PCM output");
  return ok;
}

bool throwing_host_callback_is_contained() {
  std::mutex mutex;
  std::condition_variable cv;
  int received = 0;
  int errors = 0;
  EngineCallbacks callbacks;
  callbacks.on_event = [&](events::Variant const& event) {
    if (!std::holds_alternative<events::Spectrum>(event)) return;
    int count = 0;
    {
      std::lock_guard<std::mutex> lock(mutex);
      count = ++received;
    }
    cv.notify_all();
    if (count == 1) throw std::runtime_error("deliberate event error");
  };
  callbacks.on_error = [&](std::string_view) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      ++errors;
    }
    cv.notify_all();
    // A failing error reporter must not crash the worker either.
    throw std::runtime_error("deliberate error reporter failure");
  };
  auto engine = make_engine(EngineConfig{}, callbacks, {});
  if (!engine->start()) return false;

  std::vector<std::int16_t> input(2048, 1000);
  AudioFormat format{12000, 1, SampleType::Int16};
  auto feed = [&] {
    AudioInputBuffer buffer{std::as_bytes(std::span(input)), format, SteadyClock::now()};
    return engine->submit_capture(buffer);
  };
  if (!feed()) return false;
  {
    std::unique_lock<std::mutex> lock(mutex);
    if (!cv.wait_for(lock, 5s, [&] { return errors >= 1; })) return false;
  }
  std::this_thread::sleep_for(160ms);
  if (!feed()) return false;
  bool delivered_second = false;
  {
    std::unique_lock<std::mutex> lock(mutex);
    delivered_second = cv.wait_for(lock, 5s, [&] { return received >= 2; });
  }
  engine->stop();
  if (!delivered_second) std::puts("FAIL: event callback exception killed dispatcher");
  return delivered_second;
}

bool stop_awaits_inflight_event() {
  std::mutex mutex;
  std::condition_variable cv;
  bool callback_entered = false;
  bool release = false;
  std::atomic<bool> stop_returned{false};
  EngineCallbacks callbacks;
  callbacks.on_event = [&](events::Variant const& event) {
    if (!std::holds_alternative<events::Spectrum>(event)) return;
    std::unique_lock<std::mutex> lock(mutex);
    callback_entered = true;
    cv.notify_all();
    cv.wait(lock, [&] { return release; });
  };
  auto engine = make_engine(EngineConfig{}, callbacks, {});
  if (!engine->start()) return false;
  std::vector<std::int16_t> input(2048, 1000);
  AudioFormat format{12000, 1, SampleType::Int16};
  AudioInputBuffer buffer{std::as_bytes(std::span(input)), format, SteadyClock::now()};
  if (!engine->submit_capture(buffer)) return false;
  {
    std::unique_lock<std::mutex> lock(mutex);
    if (!cv.wait_for(lock, 5s, [&] { return callback_entered; })) return false;
  }
  std::thread stopper([&] {
    engine->stop();
    stop_returned.store(true);
  });
  std::this_thread::sleep_for(100ms);
  bool const waited = !stop_returned.load();
  {
    std::lock_guard<std::mutex> lock(mutex);
    release = true;
  }
  cv.notify_all();
  stopper.join();
  if (!waited || !stop_returned.load()) {
    std::puts("FAIL: stop returned while callback still active");
    return false;
  }
  engine.reset(); // Safe immediately after cross-thread stop returns.
  return true;
}

}  // namespace

int main() {
  if (!cross_engine_render_guard_and_unaligned_output()) return 1;
  if (!throwing_host_callback_is_contained()) return 2;
  if (!stop_awaits_inflight_event()) return 3;
  std::puts("PASS: engine-specific render guard, unaligned PCM, throwing callbacks, shutdown barrier");
  return 0;
}

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include "js8core/engine.hpp"

using namespace js8core;
using namespace std::chrono_literals;

namespace {

class FailingInput final : public AudioInput {
 public:
  bool start(AudioStreamParams const&, AudioInputHandler,
             AudioErrorHandler) override { return false; }
  void stop() override {}
};

class FailingRig final : public RigControl {
 public:
  bool start(RigStateHandler, RigErrorHandler) override { return false; }
  void stop() override {}
  void apply(RigState const&, unsigned) override {}
  void request_status(unsigned) override {}
};

class BlockingInput final : public AudioInput {
 public:
  bool start(AudioStreamParams const&, AudioInputHandler,
             AudioErrorHandler) override {
    std::unique_lock<std::mutex> lock(mutex_);
    entered_ = true;
    cv_.notify_all();
    cv_.wait(lock, [this] { return release_; });
    return true;
  }

  void stop() override { ++stops_; }

  void await_start() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return entered_; });
  }

  void release() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      release_ = true;
    }
    cv_.notify_all();
  }

  int stops() const { return stops_.load(); }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  bool entered_ = false;
  bool release_ = false;
  std::atomic<int> stops_{0};
};

bool legacy_lifecycle_contract() {
  FailingInput input;
  FailingRig rig;
  int adapter_errors = 0;
  EngineCallbacks callbacks;
  callbacks.on_error = [&](std::string_view) { ++adapter_errors; };
  EngineDependencies deps;
  deps.audio_in = &input;
  deps.rig = &rig;
  auto engine = make_engine(EngineConfig{}, callbacks, deps);

  std::vector<std::int16_t> samples(256, 100);
  AudioFormat format{12000, 1, SampleType::Int16};
  AudioInputBuffer buffer{std::as_bytes(std::span(samples)), format,
                          SteadyClock::now()};
  bool const before_start = engine->submit_capture(buffer);
  bool const started = engine->start();
  bool const running_capture = engine->submit_capture(buffer);
  engine->stop();
  bool const after_stop = engine->submit_capture(buffer);
  if (!before_start || !started || !running_capture || !after_stop ||
      adapter_errors != 2) {
    std::puts("FAIL: legacy start() or manual capture lifecycle contract");
    return false;
  }
  return true;
}

bool cross_thread_stop_waits_for_start() {
  BlockingInput input;
  EngineDependencies deps;
  deps.audio_in = &input;
  auto engine = make_engine(EngineConfig{}, {}, deps);
  std::atomic<bool> started{false};
  std::atomic<bool> stop_entered{false};
  std::atomic<bool> stop_returned{false};
  std::thread starter([&] { started = engine->start(); });
  input.await_start();

  std::thread stopper([&] {
    stop_entered = true;
    engine->stop();
    stop_returned = true;
  });
  while (!stop_entered.load()) std::this_thread::yield();
  std::this_thread::sleep_for(100ms);
  bool const blocked = !stop_returned.load();

  input.release();
  starter.join();
  stopper.join();

  if (!blocked || started.load() || !stop_returned.load() || input.stops() != 1) {
    std::puts("FAIL: cross-thread stop returned before startup adapter cleanup");
    return false;
  }
  engine.reset();  // Must be safe immediately after stop() returns.
  return true;
}

bool stale_spectrum_is_invalidated() {
  std::mutex mutex;
  std::condition_variable cv;
  bool release_first = false;
  int event_count = 0;
  bool first_entered = false;

  EngineCallbacks callbacks;
  callbacks.on_event = [&](events::Variant const& event) {
    if (!std::holds_alternative<events::Spectrum>(event)) return;
    std::unique_lock<std::mutex> lock(mutex);
    ++event_count;
    if (event_count == 1) {
      first_entered = true;
      cv.notify_all();
      cv.wait_for(lock, 5s, [&] { return release_first; });
    }
    cv.notify_all();
  };

  auto engine = make_engine(EngineConfig{}, callbacks, {});
  if (!engine->start()) return false;

  std::vector<std::int16_t> samples(2048, 1000);
  AudioFormat format{12000, 1, SampleType::Int16};
  auto feed = [&] {
    AudioInputBuffer buffer{std::as_bytes(std::span(samples)), format,
                            SteadyClock::now()};
    return engine->submit_capture(buffer);
  };

  if (!feed()) return false;
  {
    std::unique_lock<std::mutex> lock(mutex);
    if (!cv.wait_for(lock, 5s, [&] { return first_entered; })) {
      std::puts("FAIL: no initial spectrum event");
      return false;
    }
  }
  // Make a pending spectrum request from the old RX session while its
  // event callback remains blocked.
  std::this_thread::sleep_for(130ms);
  if (!feed()) return false;
  engine->stop();
  if (!engine->start()) return false;
  {
    std::lock_guard<std::mutex> lock(mutex);
    release_first = true;
  }
  cv.notify_all();

  std::this_thread::sleep_for(200ms);
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (event_count != 1) {
      std::puts("FAIL: delivered stale spectrum event from previous session");
      engine->stop();
      return false;
    }
  }

  std::this_thread::sleep_for(110ms);
  if (!feed()) return false;
  bool received_new = false;
  {
    std::unique_lock<std::mutex> lock(mutex);
    received_new = cv.wait_for(lock, 5s, [&] { return event_count >= 2; });
    if (event_count != 2) received_new = false;
  }
  engine->stop();
  if (!received_new) {
    std::puts("FAIL: new spectrum missing or duplicate stale events after restart");
    return false;
  }
  return true;
}

bool raw_frame_rejected() {
  auto engine = make_engine(EngineConfig{}, {}, {});
  if (!engine->start()) return false;

  TxFrameRequest frame;
  frame.submode = 0;
  frame.audio_frequency_hz = 1500;
  bool compatible = true;
  for (auto const& text : {"", "SHORT", "HELLO-WORLD?", "HELLO-WORLD/"}) {
    frame.frame = text;
    try {
      compatible &= !engine->transmit_frame(frame);
    } catch (...) {
      compatible = false;
    }
  }
  // The old API takes the first 12 characters and masks type bits to three.
  frame.frame = "HELLO-WORLD-EXTRA";
  try {
    compatible &= engine->transmit_frame(frame);
  } catch (...) {
    compatible = false;
  }
  frame.frame = "HELLO-WORLD-";
  for (int type : {-1, 8, 31}) {
    frame.bits = type;
    try {
      compatible &= engine->transmit_frame(frame);
    } catch (...) {
      compatible = false;
    }
  }
  engine->stop();
  if (!compatible) {
    std::puts("FAIL: raw frame validation differs from the legacy contract");
    return false;
  }
  return true;
}

}  // namespace

int main() {
  if (!legacy_lifecycle_contract()) return 4;
  if (!cross_thread_stop_waits_for_start()) return 1;
  if (!stale_spectrum_is_invalidated()) return 2;
  if (!raw_frame_rejected()) return 3;
  std::puts("PASS: legacy startup/capture, cross-thread cancellation, session isolation, raw TX validation");
  return 0;
}

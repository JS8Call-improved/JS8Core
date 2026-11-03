#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "js8core/engine.hpp"

using namespace js8core;

namespace {
class FakeOutput final : public AudioOutput {
 public:
  bool start(AudioStreamParams const& params, AudioOutputFill fill,
             AudioErrorHandler) override {
    std::lock_guard<std::mutex> lock(mu_);
    format_ = params.format;
    fill_ = std::move(fill);
    started_ = true;
    return true;
  }
  void stop() override {
    std::lock_guard<std::mutex> lock(mu_);
    // A separate shutdown worker may invoke stop() while render() is
    // active; that is safe because a real adapter joins the callback thread.
    // The forbidden case is stop() invoked FROM its own rendering thread.
    if (rendering_ && render_thread_ == std::this_thread::get_id())
      stopped_on_render_thread_ = true;
    ++stops_;
    started_ = false;
  }
  bool stopped_on_render_thread() const {
    std::lock_guard<std::mutex> lock(mu_);
    return stopped_on_render_thread_;
  }
  bool started() const {
    std::lock_guard<std::mutex> lock(mu_);
    return started_;
  }
  int stops() const {
    std::lock_guard<std::mutex> lock(mu_);
    return stops_;
  }
  std::size_t render() {
    std::vector<std::byte> samples(1024 * sizeof(std::int16_t));
    AudioOutputBuffer buffer{std::span<std::byte>(samples), format_, SteadyClock::now()};
    {
      std::lock_guard<std::mutex> lock(mu_);
      rendering_ = true;
      render_thread_ = std::this_thread::get_id();
    }
    auto const count = fill_ ? fill_(buffer) : 0;
    {
      std::lock_guard<std::mutex> lock(mu_);
      rendering_ = false;
    }
    return count;
  }
 private:
  mutable std::mutex mu_;
  AudioFormat format_{};
  AudioOutputFill fill_;
  bool started_ = false;
  bool rendering_ = false;
  bool stopped_on_render_thread_ = false;
  std::thread::id render_thread_{};
  int stops_ = 0;
};

class FailingInput final : public AudioInput {
 public:
  bool start(AudioStreamParams const&, AudioInputHandler,
             AudioErrorHandler) override { return false; }
  void stop() override {}
};

// A host adapter is allowed to invoke its error callback before start()
// returns. The handler calls engine.stop() synchronously.
class ReentrantStartingInput final : public AudioInput {
 public:
  bool report_error = true;
  int stopped = 0;
  bool start(AudioStreamParams const&, AudioInputHandler,
             AudioErrorHandler on_error) override {
    if (report_error) on_error("test: synchronous adapter error");
    return true;
  }
  void stop() override { ++stopped; }
};
}

int main() {
  FakeOutput output;
  std::unique_ptr<Js8Engine> engine;
  std::vector<std::int16_t> signal(512, 1000);
  AudioFormat format{12000, 1, SampleType::Int16};

  std::atomic<bool> did_reenter_capture{false};
  std::atomic<bool> did_reenter_tx{false};
  std::atomic<bool> did_reenter_event{false};
  std::atomic<bool> event_callback_finished{false};
  std::atomic<bool> callback_ok{true};
  std::mutex event_mutex;
  std::condition_variable event_cv;

  EngineCallbacks callbacks;
  callbacks.on_log = [&](LogLevel, std::string_view log) {
    if (!engine) return;
    if (log.find("Ring buffer aligned") != std::string_view::npos &&
        !did_reenter_capture.exchange(true)) {
      AudioInputBuffer input{std::as_bytes(std::span(signal)), format,
                             SteadyClock::now()};
      if (!engine->submit_capture(input)) callback_ok.store(false);
    }
    if (log.find("TX output format") != std::string_view::npos &&
        !did_reenter_tx.exchange(true)) {
      // stop_transmit() reacquires tx_mutex_; this deadlocked when on_log
      // ran inside render_tx_audio()'s critical section.
      engine->stop_transmit();
    }
  };
  callbacks.on_event = [&](events::Variant const& event) {
    if (!std::holds_alternative<events::Spectrum>(event) || !engine ||
        did_reenter_event.exchange(true)) return;
    AudioInputBuffer input{std::as_bytes(std::span(signal)), format,
                           SteadyClock::now()};
    if (!engine->submit_capture(input)) callback_ok.store(false);
    event_callback_finished.store(true);
    event_cv.notify_all();
  };

  EngineConfig cfg;
  EngineDependencies deps;
  deps.audio_out = &output;
  engine = make_engine(cfg, callbacks, deps);
  if (!engine->start()) return 1;

  AudioInputBuffer input{std::as_bytes(std::span(signal)), format, SteadyClock::now()};
  if (!engine->submit_capture(input) || !did_reenter_capture.load()) {
    std::fputs("FAIL: synchronous capture logging reentry\n", stderr);
    return 2;
  }

  if (!engine->start_tune(1500.0, 0, 0.0) || output.render() == 0 ||
      !did_reenter_tx.load() || engine->is_transmitting()) {
    std::fputs("FAIL: TX logging reentry\n", stderr);
    return 3;
  }

  {
    std::unique_lock<std::mutex> lock(event_mutex);
    if (!event_cv.wait_for(lock, std::chrono::seconds(3),
                           [&] { return event_callback_finished.load(); })) {
      std::fputs("FAIL: spectrum callback was not invoked\n", stderr);
      return 4;
    }
  }
  // A host logging callback must not close an output stream on its own
  // rendering thread: real adapters commonly join that thread in stop().
  if (output.stopped_on_render_thread()) {
    std::fputs("FAIL: output stop ran on the render thread\\n", stderr);
    return 7;
  }
  engine->stop();
  if (output.stops() == 0 || output.started()) {
    std::fputs("FAIL: engine stop did not close deferred audio output\\n", stderr);
    return 8;
  }
  // After stopping, transmitting must fail without starting a new stream.
  TxFrameRequest frame;
  frame.frame = "HELLO-WORLD-";
  frame.audio_frequency_hz = 1500;
  TxMessageRequest message;
  message.text = "CQ";
  message.my_call = "K1ABC";
  message.my_grid = "FN20";
  if (engine->start_tune(1500.0, 0, 0.0) ||
      engine->transmit_frame(frame) || engine->transmit_message(message)) {
    std::fputs("FAIL: TX accepted while engine stopped\\n", stderr);
    return 9;
  }
  if (!engine->start() || !engine->start_tune(1500.0, 0, 0.0)) {
    std::fputs("FAIL: TX rejected after successful engine restart\\n", stderr);
    return 10;
  }
  engine->stop();
  if (output.started()) {
    std::fputs("FAIL: restarted engine left audio output open\\n", stderr);
    return 11;
  }

  // An error handler can itself call stop(). That previously deadlocked
  // because start() invoked it while holding lifecycle_mutex_.
  FailingInput failing_input;
  EngineDependencies failing_deps;
  failing_deps.audio_in = &failing_input;
  std::unique_ptr<Js8Engine> failing_engine;
  bool saw_failure_callback = false;
  EngineCallbacks error_callbacks;
  error_callbacks.on_error = [&](std::string_view) {
    saw_failure_callback = true;
    failing_engine->stop();
  };
  failing_engine = make_engine(cfg, error_callbacks, failing_deps);
  if (failing_engine->start() || !saw_failure_callback) {
    std::fputs("FAIL: lifecycle failure callback reentry\\n", stderr);
    return 6;
  }

  ReentrantStartingInput reentrant_input;
  EngineDependencies reentrant_deps;
  reentrant_deps.audio_in = &reentrant_input;
  std::unique_ptr<Js8Engine> reentrant_engine;
  bool synchronous_error_seen = false;
  EngineCallbacks reentrant_callbacks;
  reentrant_callbacks.on_error = [&](std::string_view) {
    synchronous_error_seen = true;
    reentrant_engine->stop();
  };
  reentrant_engine = make_engine(cfg, reentrant_callbacks, reentrant_deps);
  if (reentrant_engine->start() || !synchronous_error_seen ||
      reentrant_input.stopped != 1) {
    std::fputs("FAIL: synchronous start callback cancellation\\n", stderr);
    return 12;
  }
  reentrant_input.report_error = false;
  if (!reentrant_engine->start()) {
    std::fputs("FAIL: engine did not restart after cancelled startup\\n", stderr);
    return 13;
  }
  reentrant_engine->stop();
  if (reentrant_input.stopped != 2) {
    std::fputs("FAIL: restarted input was not stopped\\n", stderr);
    return 14;
  }

  if (!callback_ok.load()) {
    std::fputs("FAIL: callback reentry returned an error\n", stderr);
    return 5;
  }
  std::puts("PASS: callback reentry, deferred output shutdown, start cancellation, and TX lifecycle");
  return 0;
}

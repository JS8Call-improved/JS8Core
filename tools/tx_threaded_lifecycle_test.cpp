#include <atomic>
#include <chrono>
#include <condition_variable>
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
using namespace std::chrono_literals;

namespace {

class ThreadedOutput final : public AudioOutput {
 public:
  ~ThreadedOutput() override { stop(); }

  bool start(AudioStreamParams const& params, AudioOutputFill fill,
             AudioErrorHandler) override {
    format_ = params.format;
    fill_ = std::move(fill);
    worker_ = std::thread([this] {
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&] { return render_enabled_; });
      }
      std::vector<std::byte> raw(1024 * sizeof(std::int16_t));
      AudioOutputBuffer buffer{std::span<std::byte>(raw), format_, SteadyClock::now()};
      fill_(buffer);
    });
    return true;
  }

  void stop() override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stop_requested_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
  }

  void render_once() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      render_enabled_ = true;
    }
    cv_.notify_all();
  }

  bool await_stop() {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, 5s, [&] { return stop_requested_; });
  }

 private:
  AudioFormat format_{};
  AudioOutputFill fill_;
  std::thread worker_;
  std::mutex mutex_;
  std::condition_variable cv_;
  bool render_enabled_ = false;
  bool stop_requested_ = false;
};

}  // namespace

int main() {
  ThreadedOutput output;
  std::mutex mutex;
  std::condition_variable cv;
  bool entered_callback = false;
  bool allow_reentry = false;
  bool finished_callback = false;
  std::unique_ptr<Js8Engine> engine;

  EngineCallbacks callbacks;
  callbacks.on_log = [&](LogLevel, std::string_view message) {
    if (message.find("TX output format") == std::string_view::npos) return;
    {
      std::unique_lock<std::mutex> lock(mutex);
      entered_callback = true;
      cv.notify_all();
      cv.wait(lock, [&] { return allow_reentry; });
    }
    // The external control thread is simultaneously in AudioOutput::stop(),
    // joining this render thread. This must not wait for its lifecycle mutex.
    engine->stop_transmit();
    {
      std::lock_guard<std::mutex> lock(mutex);
      finished_callback = true;
    }
    cv.notify_all();
  };
  EngineConfig config;
  config.tx_output_rate_hz = 48000;
  EngineDependencies deps;
  deps.audio_out = &output;
  engine = make_engine(config, callbacks, deps);
  if (!engine->start() || !engine->start_tune(1500.0, 0, 0.0)) {
    std::puts("FAIL: start");
    return 1;
  }
  output.render_once();
  {
    std::unique_lock<std::mutex> lock(mutex);
    if (!cv.wait_for(lock, 5s, [&] { return entered_callback; })) {
      std::puts("FAIL: render callback did not begin");
      return 2;
    }
  }

  std::thread stopper([&] { engine->stop_transmit(); });
  bool const stop_entered = output.await_stop();
  {
    std::lock_guard<std::mutex> lock(mutex);
    allow_reentry = true;
  }
  cv.notify_all();
  stopper.join();
  engine->stop();

  if (!stop_entered || !finished_callback || engine->is_transmitting()) {
    std::puts("FAIL: joined audio thread while reentering TX shutdown");
    return 3;
  }
  // A render-thread stop with no second control thread must still close the
  // stream: the engine's deferred shutdown worker performs the blocking join.
  ThreadedOutput deferred_output;
  EngineDependencies deferred_deps;
  deferred_deps.audio_out = &deferred_output;
  std::unique_ptr<Js8Engine> deferred_engine;
  std::atomic<bool> callback_requested_stop{false};
  EngineCallbacks deferred_callbacks;
  deferred_callbacks.on_log = [&](LogLevel, std::string_view message) {
    if (message.find("TX output format") == std::string_view::npos ||
        callback_requested_stop.exchange(true)) return;
    deferred_engine->stop_transmit();
  };
  deferred_engine = make_engine(config, deferred_callbacks, deferred_deps);
  if (!deferred_engine->start() || !deferred_engine->start_tune(1500.0, 0, 0.0)) {
    std::puts("FAIL: deferred shutdown test start");
    return 4;
  }
  deferred_output.render_once();
  bool const deferred_stopped = deferred_output.await_stop();
  deferred_engine->stop();
  if (!callback_requested_stop || !deferred_stopped ||
      deferred_engine->is_transmitting()) {
    std::puts("FAIL: render-thread TX stop did not close its output automatically");
    return 5;
  }

  std::puts("PASS: concurrent TX shutdown and automatic render-thread deferred output stop");
  return 0;
}

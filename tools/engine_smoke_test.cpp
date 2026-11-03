#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "js8core/engine.hpp"

using namespace js8core;

namespace {

class FakeOutput final : public AudioOutput {
 public:
  bool start(AudioStreamParams const& params, AudioOutputFill fill,
             AudioErrorHandler) override {
    format_ = params.format;
    fill_ = std::move(fill);
    started_ = true;
    return true;
  }

  void stop() override { started_ = false; }

  bool started() const { return started_; }

  std::size_t render(std::size_t frames, std::vector<std::byte>& storage) {
    storage.assign(frames * sizeof(std::int16_t), std::byte{0});
    AudioOutputBuffer buffer{std::span<std::byte>(storage), format_, SteadyClock::now()};
    return fill_ ? fill_(buffer) : 0;
  }

 private:
  AudioFormat format_{};
  AudioOutputFill fill_;
  bool started_ = false;
};

double energy(std::vector<std::byte> const& storage) {
  auto const* samples = reinterpret_cast<std::int16_t const*>(storage.data());
  std::size_t const count = storage.size() / sizeof(std::int16_t);
  double sum = 0.0;
  for (std::size_t i = 0; i < count; ++i) {
    double const v = samples[i];
    sum += v * v;
  }
  return sum;
}

}  // namespace

int main() {
  FakeOutput output;

  EngineConfig config;
  config.tx_output_rate_hz = 48000;

  std::mutex mutex;
  std::condition_variable cv;
  bool got_spectrum = false;
  std::vector<std::string> errors;

  EngineCallbacks callbacks;
  callbacks.on_event = [&](events::Variant const& event) {
    if (std::holds_alternative<events::Spectrum>(event)) {
      std::lock_guard<std::mutex> lock(mutex);
      got_spectrum = true;
      cv.notify_all();
    }
  };
  callbacks.on_error = [&](std::string_view message) {
    std::lock_guard<std::mutex> lock(mutex);
    errors.emplace_back(message);
  };

  EngineDependencies deps;
  deps.audio_out = &output;

  auto engine = make_engine(config, callbacks, deps);
  if (!engine) {
    std::printf("FAIL: engine construction\n");
    return 1;
  }
  if (!engine->start()) {
    std::printf("FAIL: engine start\n");
    return 1;
  }
  if (engine->time_drift_ms() != 0) {
    std::printf("FAIL: initial drift\n");
    return 1;
  }

  // Capture: feed a small non-silent buffer and wait for a spectrum frame.
  std::vector<std::int16_t> captured(1024);
  for (std::size_t i = 0; i < captured.size(); ++i) {
    captured[i] = (i % 2 == 0) ? 1200 : -1200;
  }
  AudioFormat format{12000, 1, SampleType::Int16};
  AudioInputBuffer input{std::as_bytes(std::span(captured)), format,
                         SteadyClock::now()};
  if (!engine->submit_capture(input)) {
    std::printf("FAIL: submit_capture\n");
    return 1;
  }
  {
    std::unique_lock<std::mutex> lock(mutex);
    if (!cv.wait_for(lock, std::chrono::seconds(3), [&]() { return got_spectrum; })) {
      std::printf("FAIL: no spectrum event\n");
      return 1;
    }
  }

  // Clock offset is stored and reported.
  engine->set_time_drift_ms(123);
  if (engine->time_drift_ms() != 123) {
    std::printf("FAIL: drift setter\n");
    return 1;
  }

  // Tune produces a continuous tone immediately.
  engine->set_tx_ready(true);
  if (!engine->start_tune(1500.0, 0, 0.0)) {
    std::printf("FAIL: start_tune\n");
    return 1;
  }
  if (!engine->is_transmitting()) {
    std::printf("FAIL: tune did not mark transmitting\n");
    return 1;
  }
  std::vector<std::byte> storage;
  double tune_energy = 0.0;
  for (int i = 0; i < 40; ++i) {
    if (output.render(4800, storage) == 0) {
      std::printf("FAIL: tune render produced no frames\n");
      return 1;
    }
    tune_energy += energy(storage);
  }
  if (tune_energy <= 0.0) {
    std::printf("FAIL: tune produced silence\n");
    return 1;
  }
  engine->stop_transmit();
  if (engine->is_transmitting()) {
    std::printf("FAIL: stop_transmit left engine transmitting\n");
    return 1;
  }

  // A full frame transmits after its slot lead-in.
  TxFrameRequest request;
  request.frame = "HELLO-WORLD-";
  request.bits = 0;
  request.submode = 0;
  request.audio_frequency_hz = 1500.0;
  request.tx_delay_s = 0.0;
  if (!engine->transmit_frame(request)) {
    std::printf("FAIL: transmit_frame\n");
    return 1;
  }

  double frame_energy = 0.0;
  bool finished = false;
  for (int i = 0; i < 2000; ++i) {
    if (output.render(4800, storage) == 0) {
      std::printf("FAIL: frame render produced no frames\n");
      return 1;
    }
    frame_energy += energy(storage);
    if (!engine->is_transmitting()) {
      finished = true;
      break;
    }
  }
  if (!finished) {
    std::printf("FAIL: frame transmission did not finish\n");
    return 1;
  }
  if (frame_energy <= 0.0) {
    std::printf("FAIL: frame produced silence\n");
    return 1;
  }

  engine->stop();

  {
    std::lock_guard<std::mutex> lock(mutex);
    if (!errors.empty()) {
      std::printf("FAIL: engine reported error: %s\n", errors.front().c_str());
      return 1;
    }
  }

  std::printf("PASS: engine smoke test\n");
  return 0;
}

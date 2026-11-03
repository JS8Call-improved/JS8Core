#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "js8core/engine.hpp"

using namespace js8core;

namespace {

// Models the Android JNI adapter's offline rig object without copying it.
class OfflineRig final : public RigControl {
 public:
  bool start(RigStateHandler callback, RigErrorHandler) override {
    callback_ = std::move(callback);
    report_offline();
    return true;
  }
  void stop() override { callback_ = {}; }
  void apply(RigState const&, unsigned) override { ++applies_; }
  void request_status(unsigned) override { report_offline(); }
  void report_offline() {
    if (callback_) {
      RigState state{};
      state.online = false;
      callback_(state);
    }
  }
  int applies() const { return applies_; }
 private:
  RigStateHandler callback_;
  int applies_ = 0;
};

// Simulates a real rig whose PTT status is still unkeyed while the host has
// explicitly granted audio readiness. Legacy setter must take precedence.
class OnlineRig final : public RigControl {
 public:
  bool start(RigStateHandler callback, RigErrorHandler) override {
    callback_ = std::move(callback);
    report_unkeyed();
    return true;
  }
  void stop() override { callback_ = {}; }
  void apply(RigState const&, unsigned) override {}
  void request_status(unsigned) override { report_unkeyed(); }
  void report_unkeyed() {
    if (!callback_) return;
    RigState state{};
    state.online = true;
    state.ptt = false;
    callback_(state);
  }
 private:
  RigStateHandler callback_;
};

class TappedOutput final : public AudioOutput {
 public:
  bool start(AudioStreamParams const& params, AudioOutputFill fill,
             AudioErrorHandler) override {
    format_ = params.format;
    callback_ = std::move(fill);
    return true;
  }
  void stop() override { callback_ = {}; }
  bool render() {
    if (!callback_) return false;
    std::vector<std::byte> samples(1152 * sizeof(std::int16_t));
    AudioOutputBuffer buffer{std::span<std::byte>(samples), format_,
                             SteadyClock::now()};
    return callback_(buffer) == samples.size();
  }
 private:
  AudioFormat format_{};
  AudioOutputFill callback_;
};

bool host_managed_ptt() {
  OfflineRig rig;
  TappedOutput output;
  EngineConfig config;
  config.tx_output_rate_hz = 11520;
  EngineDependencies deps;
  deps.rig = &rig;
  deps.audio_out = &output;
  auto engine = make_engine(config, {}, deps);
  if (!engine->start()) return false;

  TxFrameRequest frame;
  frame.frame = "HELLO-WORLD-";
  frame.submode = 0;
  frame.audio_frequency_hz = 1500;

  // Android's TX monitor queues a message with PTT not yet ready. The
  // queued request and countdown must remain available to key the radio.
  engine->set_tx_ready(false);
  if (!engine->transmit_frame(frame) || !engine->is_transmitting()) return false;
  if (engine->tx_milliseconds_until_audio() < 0) return false;
  if (!output.render()) return false;
  if (engine->is_transmitting_audio()) return false;

  // Host keying opens the audio gate, without a command to its NullRig stub.
  engine->set_tx_ready(true);
  rig.report_offline();
  if (!output.render()) return false;
  engine->stop_transmit();

  // Legacy Android also submits a frame with no preceding setTxReady.
  if (!engine->transmit_frame(frame)) return false;
  if (engine->tx_milliseconds_until_audio() < 0) return false;
  engine->stop_transmit();

  // Tune is accepted while gated, then resumes once PTT is confirmed.
  engine->set_tx_ready(false);
  if (!engine->start_tune(1500.0, 0, 0.0) || !output.render() ||
      engine->is_transmitting_audio()) return false;
  engine->set_tx_ready(true);
  if (!output.render() || !engine->is_transmitting_audio()) return false;
  rig.report_offline();
  if (!engine->is_transmitting_audio()) return false;

  engine->set_tx_ready(false);
  if (engine->is_transmitting_audio()) return false;
  engine->stop_transmit();

  // The legacy public API takes the first twelve characters of a frame and
  // falls back to Normal on unknown submode values.
  frame.frame = "HELLO-WORLD-EXTRA";
  frame.submode = 99;
  if (!engine->transmit_frame(frame)) return false;
  engine->stop_transmit();
  engine->stop();

  return rig.applies() == 0;
}

bool explicit_host_gate_overrides_online_rig() {
  OnlineRig rig;
  TappedOutput output;
  EngineDependencies deps;
  deps.rig = &rig;
  deps.audio_out = &output;
  EngineConfig config;
  config.tx_output_rate_hz = 11520;
  auto engine = make_engine(config, {}, deps);
  if (!engine->start()) return false;

  engine->set_tx_ready(false);
  if (!engine->start_tune(1500.0, 0, 0.0)) return false;
  if (engine->is_transmitting_audio()) return false;

  engine->set_tx_ready(true);
  if (!engine->is_transmitting_audio()) return false;
  rig.report_unkeyed();
  if (!engine->is_transmitting_audio()) return false;

  engine->set_tx_ready(false);
  rig.report_unkeyed();
  const bool muted = !engine->is_transmitting_audio();
  engine->stop();
  return muted;
}

bool accelerated_android_capture() {
  std::atomic<int> alignments{0};
  EngineCallbacks callbacks;
  callbacks.on_log = [&](LogLevel, std::string_view message) {
    if (message.find("Ring buffer aligned to UTC minute") !=
        std::string_view::npos) ++alignments;
  };

  EngineConfig config;
  config.sample_rate_hz = 12000;
  auto engine = make_engine(config, callbacks, {});
  if (!engine->start()) return false;

  const int baseline = alignments.load();
  std::vector<std::int16_t> samples(4096, 700);
  AudioFormat format{12000, 1, SampleType::Int16};

  // The old Android instrumentation test submits 4096-frame chunks without
  // waiting 341 ms between callbacks; every callback is stamped with 'now'.
  for (int chunk = 0; chunk < 9; ++chunk) {
    AudioInputBuffer buffer{std::as_bytes(std::span(samples)), format,
                            SteadyClock::now()};
    if (!engine->submit_capture(buffer)) return false;
  }
  const int realignments = alignments.load() - baseline;
  engine->stop();
  return realignments == 1;
}

}  // namespace

int main() {
  if (!host_managed_ptt()) {
    std::puts("FAIL: Android externally managed TX was refused or bypassed");
    return 1;
  }
  if (!explicit_host_gate_overrides_online_rig()) {
    std::puts("FAIL: explicit host readiness overridden by online PTT status");
    return 3;
  }
  if (!accelerated_android_capture()) {
    std::puts("FAIL: accelerated JNI audio repeatedly realigned the ring");
    return 2;
  }
  std::puts("PASS: legacy readiness gating, immediate countdown, online/offline rigs, accelerated capture");
  return 0;
}

#include "js8core/engine.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

using namespace js8core;

namespace {
class Rig final : public RigControl {
 public:
  bool start(RigStateHandler handler, RigErrorHandler) override {
    std::lock_guard<std::mutex> lock(mu_);
    callback_ = std::move(handler);
    state_.online = true;
    state_.rx_frequency = 7078000;
    state_.tx_frequency = 7078000;
    state_.mode = Mode::DIG_U;
    callback_(state_);
    return true;
  }
  void stop() override {
    std::lock_guard<std::mutex> lock(mu_);
    stopped_ = true;
  }
  void apply(RigState const& desired, unsigned) override {
    RigStateHandler cb;
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (desired.tx_frequency != 7078000 || desired.rx_frequency != 7078000 ||
          desired.mode != Mode::DIG_U) bad_tuning_ = true;
      state_ = desired;
      if (desired.ptt) {
        saw_on_ = true;
        ++keyups_;
      } else {
        saw_off_ = true;
      }
      cb = callback_;
    }
    if (cb) cb(desired);
  }
  void request_status(unsigned) override {
    RigStateHandler cb;
    RigState current;
    {
      std::lock_guard<std::mutex> lock(mu_);
      cb = callback_;
      current = state_;
    }
    if (cb) cb(current);
  }
  bool keyed() {
    std::lock_guard<std::mutex> lock(mu_);
    return state_.ptt;
  }
  int keyups() {
    std::lock_guard<std::mutex> lock(mu_);
    return keyups_;
  }
  void set_online(bool online) {
    RigStateHandler cb;
    RigState current;
    {
      std::lock_guard<std::mutex> lock(mu_);
      state_.online = online;
      state_.ptt = false;
      cb = callback_;
      current = state_;
    }
    if (cb) cb(current);
  }
  bool successful() {
    std::lock_guard<std::mutex> lock(mu_);
    return saw_on_ && saw_off_ && !bad_tuning_ && !state_.ptt && stopped_;
  }
 private:
  std::mutex mu_;
  RigStateHandler callback_;
  RigState state_{};
  bool stopped_ = false;
  bool saw_on_ = false, saw_off_ = false, bad_tuning_ = false;
  int keyups_ = 0;
};

class Output final : public AudioOutput {
 public:
  bool start(AudioStreamParams const& params, AudioOutputFill fill,
             AudioErrorHandler) override {
    format_ = params.format;
    handler_ = std::move(fill);
    return true;
  }
  void stop() override { handler_ = {}; }
  std::size_t render() {
    std::vector<std::byte> raw(4096 * sizeof(std::int16_t));
    AudioOutputBuffer buffer{std::span<std::byte>(raw), format_,
                              SteadyClock::now() + std::chrono::seconds(2)};
    return handler_ ? handler_(buffer) : 0;
  }
 private:
  AudioFormat format_{};
  AudioOutputFill handler_;
};
}

int main() {
  Rig rig;
  Output output;
  EngineConfig config;
  config.tx_output_rate_hz = 48000;
  EngineDependencies deps;
  deps.rig = &rig;
  deps.audio_out = &output;
  auto engine = make_engine(config, {}, deps);
  if (!engine->start()) return 1;
  TxFrameRequest request;
  request.frame = "HELLO-WORLD-";
  request.audio_frequency_hz = 1500;
  if (!engine->transmit_frame(request)) return 2;
  for (int i = 0; i < 1000 && !rig.keyed(); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  if (!rig.keyed() || output.render() != 8192) return 3;
  // Disconnect while a PTT request is live, then reconnect. A subsequent
  // transmission must enqueue a fresh PTT-on command (not trust the old
  // ptt_requested_ cached flag).
  rig.set_online(false);
  engine->stop_transmit();
  rig.set_online(true);
  if (!engine->transmit_frame(request)) return 4;
  for (int i = 0; i < 1000 && rig.keyups() < 2; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  if (rig.keyups() < 2 || !rig.keyed()) {
    std::puts("FAIL: PTT was not re-keyed after disconnect/reconnect");
    return 5;
  }
  engine->stop_transmit();
  engine->stop();
  if (!rig.successful()) return 6;
  std::puts("PASS: PTT key/release and re-key after disconnect preserve rig tuning");
  return 0;
}

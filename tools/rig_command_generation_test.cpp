#include "js8core/engine.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <span>
#include <thread>
#include <utility>
#include <vector>

using namespace js8core;
using namespace std::chrono_literals;

namespace {

class BlockingRig final : public RigControl {
 public:
  bool start(RigStateHandler on_state, RigErrorHandler) override {
    RigState state;
    {
      std::lock_guard<std::mutex> lock(mu_);
      callback_ = std::move(on_state);
      state_.online = true;
      state_.rx_frequency = state_.tx_frequency = 7078000;
      state_.mode = Mode::DIG_U;
      state = state_;
    }
    if (callback_) callback_(state);
    return true;
  }
  void stop() override {}

  void apply(RigState const& desired, unsigned) override {
    {
      std::unique_lock<std::mutex> lock(mu_);
      if (desired.ptt && !first_keyup_entered_) {
        first_keyup_entered_ = true;
        cv_.notify_all();
        cv_.wait(lock, [&] { return release_first_; });
      }
    }
    RigStateHandler callback;
    RigState state;
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (desired.ptt) ++keyups_;
      else ++unkeys_;
      state_.ptt = desired.ptt;
      // Preserve the current physical connection state while applying an
      // older command that may have started before a disconnect/reconnect.
      state = state_;
      callback = callback_;
    }
    if (callback) callback(state);
    cv_.notify_all();
  }
  void request_status(unsigned) override {
    RigStateHandler callback;
    RigState state;
    {
      std::lock_guard<std::mutex> lock(mu_);
      callback = callback_;
      state = state_;
    }
    if (callback) callback(state);
  }
  bool wait_first_keyup() {
    std::unique_lock<std::mutex> lock(mu_);
    return cv_.wait_for(lock, 5s, [&] { return first_keyup_entered_; });
  }
  void set_online(bool online) {
    RigStateHandler callback;
    RigState state;
    {
      std::lock_guard<std::mutex> lock(mu_);
      state_.online = online;
      state_.ptt = false;
      state = state_;
      callback = callback_;
    }
    if (callback) callback(state);
  }
  void release_first() {
    {
      std::lock_guard<std::mutex> lock(mu_);
      release_first_ = true;
    }
    cv_.notify_all();
  }
  bool wait_new_keyup() {
    std::unique_lock<std::mutex> lock(mu_);
    return cv_.wait_for(lock, 5s, [&] {
      return keyups_ >= 2 && unkeys_ >= 1 && state_.ptt;
    });
  }
 private:
  std::mutex mu_;
  std::condition_variable cv_;
  RigStateHandler callback_;
  RigState state_{};
  bool first_keyup_entered_ = false;
  bool release_first_ = false;
  int keyups_ = 0;
  int unkeys_ = 0;
};

class Output final : public AudioOutput {
 public:
  bool start(AudioStreamParams const&, AudioOutputFill,
             AudioErrorHandler) override { return true; }
  void stop() override {}
};

}  // namespace

int main() {
  BlockingRig rig;
  Output output;
  EngineDependencies deps;
  deps.rig = &rig;
  deps.audio_out = &output;
  auto engine = make_engine(EngineConfig{}, {}, deps);
  if (!engine->start()) return 1;

  TxFrameRequest frame;
  frame.frame = "HELLO-WORLD-";
  frame.audio_frequency_hz = 1500;
  if (!engine->transmit_frame(frame) || !rig.wait_first_keyup()) {
    std::puts("FAIL: first key-up was not submitted");
    rig.release_first();
    return 2;
  }

  // Queue two commands behind the blocked key-up, then invalidate them.
  // Their cancellation must count as completion and the old in-flight
  // command must be followed by a corrective unkey on reconnection.
  engine->stop_transmit();           // pending PTT-off
  if (!engine->transmit_frame(frame)) {
    rig.release_first();
    return 3;
  }                                    // pending PTT-on
  rig.set_online(false);             // retires both queued commands
  engine->stop_transmit();
  rig.set_online(true);
  if (!engine->transmit_frame(frame)) {
    rig.release_first();
    return 4;
  }

  rig.release_first();
  if (!rig.wait_new_keyup()) {
    std::puts("FAIL: stale PTT was not corrected before fresh key-up");
    engine->stop();
    return 5;
  }

  engine->stop_transmit();
  const auto begin = std::chrono::steady_clock::now();
  engine->stop();
  const auto duration = std::chrono::steady_clock::now() - begin;
  if (duration > 1500ms) {
    std::puts("FAIL: cancelled PTT command accounting stalled shutdown");
    return 6;
  }
  std::puts("PASS: in-flight stale PTT corrected and cancelled command accounting balanced");
  return 0;
}

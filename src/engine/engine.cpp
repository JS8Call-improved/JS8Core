#include "js8core/engine.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef __ANDROID__
#include <android/log.h>
#endif

#include "js8core/compat/numbers.hpp"
#include "js8core/decoder.hpp"
#include "src/decoder/ldpc.hpp"
#include "js8core/decoder_state.hpp"
#include "js8core/dsp/resampler.hpp"
#include "js8core/protocol/constants.hpp"
#include "js8core/protocol/costas.hpp"
#include "js8core/protocol/submode.hpp"
#include "js8core/protocol/varicode.hpp"
#include "js8core/tx/modulator.hpp"

#include "clock_math.hpp"

namespace js8core {
namespace {

using protocol::SubmodeId;

constexpr int kCaptureRate = kJs8RxSampleRate;
constexpr int kRingFrames = kJs8NtMax * kCaptureRate;
constexpr auto kSpectrumInterval = std::chrono::milliseconds(100);
constexpr std::size_t kSpectrumBins = static_cast<std::size_t>(kJs8NsMax);
constexpr std::size_t kSpectrumMaxInput = 4096;
constexpr std::size_t kSpectrumMinInput = 64;
constexpr double kBoostGain = 3.1623;
constexpr int kTurboRetryFrames = kCaptureRate;
constexpr int kTimestampToleranceMs = 250;

// AudioOutput::stop() may join the rendering thread. If a host on_log callback
// asks to stop TX from that same rendering thread, leave the stream open and
// silent until a different thread closes it (e.g. engine stop/destruction).
class Js8EngineImpl;
thread_local Js8EngineImpl* g_tx_render_engine = nullptr;
struct ScopedTxAudioCallback {
  Js8EngineImpl* previous;
  explicit ScopedTxAudioCallback(Js8EngineImpl* engine)
      : previous(g_tx_render_engine) { g_tx_render_engine = engine; }
  ~ScopedTxAudioCallback() { g_tx_render_engine = previous; }
};

std::optional<protocol::Submode> submode_for_value(int value) {
  switch (value) {
    case 0: return protocol::find(SubmodeId::A);
    case 1: return protocol::find(SubmodeId::B);
    case 2: return protocol::find(SubmodeId::C);
    case 4: return protocol::find(SubmodeId::E);
    case 8: return protocol::find(SubmodeId::I);
    default: return protocol::find(SubmodeId::A);
  }
}

protocol::CostasType costas_for_value(int value) {
  return value == 0 ? protocol::CostasType::Original : protocol::CostasType::Modified;
}

int default_submode_mask() {
  int mask = 0;
  for (auto const& sm : protocol::submodes()) {
    if (sm.enabled) mask |= 1 << static_cast<int>(sm.id);
  }
  return mask;
}

std::tm utc_parts(SystemClock::time_point tp) {
  auto const seconds = SystemClock::to_time_t(tp);
  std::tm out{};
#if defined(_WIN32)
  gmtime_s(&out, &seconds);
#else
  gmtime_r(&seconds, &out);
#endif
  return out;
}

std::int64_t milliseconds_since_epoch(SystemClock::time_point tp) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count();
}

std::size_t next_power_of_two(std::size_t value) {
  if (value == 0) return 1;
  --value;
  value |= value >> 1;
  value |= value >> 2;
  value |= value >> 4;
  value |= value >> 8;
  value |= value >> 16;
#if INTPTR_MAX == INT64_MAX
  value |= value >> 32;
#endif
  return value + 1;
}

void fft_in_place(std::vector<std::complex<double>>& data) {
  std::size_t const n = data.size();
  if (n < 2) return;

  for (std::size_t i = 1, j = 0; i < n; ++i) {
    std::size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(data[i], data[j]);
  }

  for (std::size_t len = 2; len <= n; len <<= 1) {
    double const angle = -2.0 * js8core::compat::numbers::pi / static_cast<double>(len);
    std::complex<double> const step(std::cos(angle), std::sin(angle));
    for (std::size_t base = 0; base < n; base += len) {
      std::complex<double> twiddle(1.0, 0.0);
      for (std::size_t j = 0; j < len / 2; ++j) {
        auto const even = data[base + j];
        auto const odd = data[base + j + len / 2] * twiddle;
        data[base + j] = even + odd;
        data[base + j + len / 2] = even - odd;
        twiddle *= step;
      }
    }
  }
}

// One submode's periodic decode window. Windows are aligned to absolute
// positions in the capture ring so that they track UTC slot boundaries.
struct SlotPlan {
  SubmodeId id;
  int period = 0;       // frames per cycle
  int required = 0;     // frames needed before a decode attempt
  int retry = 0;        // minimum frames between attempts
  int window = -1;      // absolute start of the current window
  int next = -1;        // absolute start of the following window
  int last_attempt = -1;
};

// Returns true when a decode window is due. On success, `start` and `size`
// describe the absolute ring range to decode.
bool slot_ready(SlotPlan& slot, int position, int previous, int& start, int& size) {
  int const cycle = slot.period;
  int const cycle_index = (position / cycle) % (kRingFrames / cycle);
  int const jump = std::abs(position - previous);

  bool const wrapped =
      position < slot.window &&
      position < std::max(0, slot.window - cycle + slot.required);

  if (wrapped || position < previous || jump > cycle ||
      slot.window < 0 || slot.next < 0) {
    slot.window = cycle_index * cycle;
    slot.next = slot.window + cycle;
    slot.last_attempt = -1;
  }

  while (position >= slot.next) {
    slot.window = slot.next;
    slot.next += cycle;
    slot.last_attempt = -1;
  }

  bool const due =
      slot.window + slot.required <= position &&
      (slot.last_attempt < slot.window ||
       position - slot.last_attempt >= slot.retry);
  if (!due) return false;

  start = slot.window;
  size = std::max(slot.required, position - slot.window);
  slot.last_attempt = position;
  return true;
}

class Js8EngineImpl final : public Js8Engine {
 public:
  Js8EngineImpl(EngineConfig config, EngineCallbacks callbacks, EngineDependencies deps)
      : config_(std::move(config)),
        callbacks_(std::move(callbacks)),
        deps_(deps) {
    ring_.samples.resize(kRingFrames);
    ring_.params.nfa = 200;
    ring_.params.nfb = 2500;
    ring_.params.nfqso = 1500;

    align_ring(aligned_now());

    if (config_.submodes == 0) config_.submodes = default_submode_mask();
    active_submodes_.store(config_.submodes);

    build_slot_plans();
    start_decode_thread();
    start_spectrum_thread();
    if (deps_.rig) rig_thread_ = std::thread([this] { rig_loop(); });
    if (deps_.audio_out) {
      tx_shutdown_thread_ = std::thread([this] { tx_shutdown_loop(); });
    }
  }

  ~Js8EngineImpl() override {
    stop();
    shutdown_tx_thread();
    shutdown_decode_thread();
    shutdown_spectrum_thread();
    shutdown_rig_thread();
  }

  bool start() override {
    {
      std::lock_guard<std::mutex> lock(lifecycle_mutex_);
      if (lifecycle_state_ == LifecycleState::Running) return true;
      if (lifecycle_state_ != LifecycleState::Stopped) return false;
      lifecycle_state_ = LifecycleState::Starting;
      lifecycle_owner_ = std::this_thread::get_id();
      cancel_start_ = false;
      session_generation_.fetch_add(1, std::memory_order_acq_rel);
      awaiting_first_buffer_.store(true);
      running_.store(true);
    }

    // Never hold lifecycle_mutex_ while calling a platform adapter: its
    // start() may synchronously invoke on_error, whose host handler may stop
    // this engine. A stop during startup requests cancellation; the start
    // owner tears down partially initialized adapters after each call returns.
    bool input_started = false;
    bool rig_started = false;
    bool ok = true;

    if (deps_.audio_in) {
      AudioStreamParams params;
      params.format.sample_rate = config_.sample_rate_hz ? config_.sample_rate_hz : kCaptureRate;
      params.format.channels = 1;
      params.format.sample_type = SampleType::Int16;
      params.frames_per_buffer = 0;

      input_started = deps_.audio_in->start(
          params,
          [this](AudioInputBuffer const& buffer) {
            if (running_.load()) submit_capture(buffer);
          },
          [this](std::string_view message) { report_error(message); });
      // Legacy start() reports adapter errors but remains usable for manual
      // submit_capture() even when the device audio adapter cannot start.
      if (!input_started) report_error("Failed to start audio input");
    }

    {
      std::lock_guard<std::mutex> lock(lifecycle_mutex_);
      if (cancel_start_) ok = false;
    }

    if (ok && deps_.rig) {
      // A host readiness setting is a public API value. Preserve it across
      // starts just as the original core did; otherwise follow rig status.
      tx_ready_.store(host_tx_ready_override_.load() == 1);
      rig_started = deps_.rig->start(
          [this](RigState const& state) {
            {
              std::lock_guard<std::mutex> lock(rig_mutex_);
              // A disconnect invalidates PTT intent. A reconnect must
              // issue a new key-up rather than trusting an old request.
              bool const reconnect = !rig_state_known_ || !rig_state_.online;
              if (!state.online || reconnect) invalidate_rig_commands_locked();
              rig_state_ = state;
              rig_state_known_ = true;
            }
            // An explicit host setter wins over rig status (legacy API).
            // Without a host override, follow confirmed PTT from real rigs.
            const int host = host_tx_ready_override_.load();
            tx_ready_.store(host >= 0 ? host != 0
                                       : (state.online && state.ptt));
            rig_cv_.notify_all();
          },
          [this](std::string_view message) { report_error(message); });
      // As with audio input, a failed rig adapter does not reject start().
      if (!rig_started) report_error("Failed to start rig control");
      if (rig_started) {
        bool cancelled = false;
        {
          std::lock_guard<std::mutex> lock(lifecycle_mutex_);
          cancelled = cancel_start_;
        }
        if (!cancelled) deps_.rig->request_status(rig_sequence_.fetch_add(1) + 1);
      }
    }

    {
      std::lock_guard<std::mutex> lock(lifecycle_mutex_);
      if (cancel_start_) ok = false;
      if (ok) {
        lifecycle_state_ = LifecycleState::Running;
        lifecycle_owner_ = {};
        return true;
      }
      lifecycle_state_ = LifecycleState::Stopping;
      running_.store(false);
      session_generation_.fetch_add(1, std::memory_order_acq_rel);
    }

    // Stop only adapters successfully started. Do not call host code under
    // lifecycle_mutex_, including when a host callback cancelled startup.
    invalidate_pending_work();
    stop_transmit();
    await_tx_shutdown();
    if (rig_started) deps_.rig->stop();
    if (input_started) deps_.audio_in->stop();
    tx_ready_.store(false);
    {
      std::lock_guard<std::mutex> lock(rig_mutex_);
      rig_state_known_ = false;
      invalidate_rig_commands_locked();
    }
    // Include error callbacks and in-flight event delivery in completion:
    // an external stop() must not destroy the engine while either is active.
    await_event_dispatch();
    {
      std::lock_guard<std::mutex> lock(lifecycle_mutex_);
      lifecycle_state_ = LifecycleState::Stopped;
      lifecycle_owner_ = {};
    }
    lifecycle_cv_.notify_all();
    return false;
  }

  void stop() override {
    // If an event callback calls stop() while another thread is already
    // shutting down, never block that callback: the owner may be waiting
    // for this very event to finish.
    bool const in_event_dispatch = on_event_dispatch_thread();
    bool stop_adapters = false;
    bool wait_for_owner = false;
    {
      std::unique_lock<std::mutex> lock(lifecycle_mutex_);
      if (lifecycle_state_ == LifecycleState::Starting ||
          lifecycle_state_ == LifecycleState::Stopping) {
        if (lifecycle_state_ == LifecycleState::Starting) {
          cancel_start_ = true;
          running_.store(false);
          session_generation_.fetch_add(1, std::memory_order_acq_rel);
        }
        // A synchronous adapter callback on the startup/stop owner cannot
        // wait for its own stack to finish. Other threads must wait so that
        // stop() followed by destruction cannot race adapter startup.
        if (lifecycle_owner_ == std::this_thread::get_id() ||
            in_event_dispatch) return;
        wait_for_owner = true;
      } else {
        stop_adapters = lifecycle_state_ == LifecycleState::Running;
        lifecycle_state_ = LifecycleState::Stopping;
        lifecycle_owner_ = std::this_thread::get_id();
        running_.store(false);
        session_generation_.fetch_add(1, std::memory_order_acq_rel);
      }
      if (wait_for_owner) {
        lifecycle_cv_.wait(lock, [this] {
          return lifecycle_state_ == LifecycleState::Stopped;
        });
        return;
      }
    }

    invalidate_pending_work();
    // Also close TX when the engine was already stopped: the output adapter
    // may still have been active after an interrupted callback.
    stop_transmit();
    await_tx_shutdown();
    if (stop_adapters) {
      if (deps_.audio_in) deps_.audio_in->stop();
      if (deps_.rig) {
        flush_rig_commands();
        deps_.rig->stop();
        tx_ready_.store(false);
        std::lock_guard<std::mutex> lock(rig_mutex_);
        rig_state_known_ = false;
        invalidate_rig_commands_locked();
      }
    }
    // An external stop() must not return while an old-session host event
    // callback is executing. Calls made from within that callback are exempt.
    await_event_dispatch();
    {
      std::lock_guard<std::mutex> lock(lifecycle_mutex_);
      lifecycle_state_ = LifecycleState::Stopped;
      lifecycle_owner_ = {};
    }
    lifecycle_cv_.notify_all();
  }

  bool submit_capture(AudioInputBuffer const& buffer) override {
    // The original manual-input API accepts valid buffers before start() and
    // after stop(). Session-aware workers still suppress stale callbacks.
    if (buffer.format.sample_type != SampleType::Int16) {
      report_error("Unsupported sample type");
      return false;
    }
    if (config_.sample_rate_hz && buffer.format.sample_rate != config_.sample_rate_hz) {
      report_error("Unexpected sample rate");
      return false;
    }
    if (buffer.format.sample_rate <= 0) {
      report_error("Invalid RX sample rate");
      return false;
    }

    int const channels = buffer.format.channels;
    if (channels <= 0) return false;

    std::size_t const bytes_per_frame =
        sizeof(std::int16_t) * static_cast<std::size_t>(channels);
    std::size_t const frames = buffer.data.size() / bytes_per_frame;

    // The decode worker copies the ring under this lock; capture adapters must
    // deliver sequential buffers.
    std::vector<std::string> deferred_logs;
    std::unique_lock<std::mutex> capture_guard(capture_mutex_);

    bool const stamped = buffer.captured_at != SteadyTimePoint{};
    bool const first = awaiting_first_buffer_.exchange(false);
    if (first) {
      // An adapter may reuse the engine across start/stop sessions.
      capture_delivery_timestamps_ = false;
      expected_capture_at_.reset();
    }
    bool discontinuity = false;
    if (stamped && expected_capture_at_ && !capture_delivery_timestamps_) {
      auto const offset_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 buffer.captured_at - *expected_capture_at_)
                                 .count();
      if (offset_ms < -kTimestampToleranceMs) {
        // Legacy Android JNI stamps each buffer at callback delivery, not at
        // the first captured sample. Its reference tests can submit many
        // buffers faster than real time. Treat a timestamp that runs behind
        // the sample timeline as delivery-time metadata, and keep the
        // continuous audio ring rather than clearing it on every buffer.
        capture_delivery_timestamps_ = true;
        deferred_logs.emplace_back(
            "Capture timestamps lag audio sample timeline; using sample continuity");
      } else {
        discontinuity = offset_ms > kTimestampToleranceMs;
      }
    }

    bool const realign = realign_requested_.exchange(false);
    bool const rate_changed = rx_rate_ != buffer.format.sample_rate;

    if (first || realign || discontinuity || rate_changed) {
      auto wall = aligned_now();
      if (stamped) {
        wall += std::chrono::duration_cast<SystemClock::duration>(
            buffer.captured_at - SteadyClock::now());
      }
      align_ring(wall, &deferred_logs);
      rx_resampler_.reset();
      rx_fractional_.reset();
      rx_pending_.clear();
    }

    rx_rate_ = buffer.format.sample_rate;
    bool const integral = (rx_rate_ % kCaptureRate) == 0;
    if (integral && rx_resampler_.input_rate() != rx_rate_) {
      rx_resampler_.configure(rx_rate_, kCaptureRate);
    }
    if (!integral && rx_fractional_.input_rate() != rx_rate_) {
      rx_fractional_.configure(rx_rate_, kCaptureRate);
    }

    if (stamped) {
      expected_capture_at_ =
          buffer.captured_at +
          std::chrono::duration_cast<SteadyClock::duration>(
              std::chrono::duration<double>(static_cast<double>(frames) /
                                            buffer.format.sample_rate));
    } else {
      expected_capture_at_.reset();
    }

    // Audio adapters expose raw bytes, which may not be aligned for int16_t.
    rx_input_.resize(frames * static_cast<std::size_t>(channels));
    std::memcpy(rx_input_.data(), buffer.data.data(),
                rx_input_.size() * sizeof(std::int16_t));
    auto const* samples = rx_input_.data();

    ++capture_log_counter_;
    if (frames > 0 && capture_log_counter_ % 100 == 0 && callbacks_.on_log) {
      double sum = 0.0;
      for (std::size_t i = 0; i < frames; ++i) {
        double const v = samples[i * channels];
        sum += v * v;
      }
      char message[256];
      std::snprintf(message, sizeof(message),
                    "Audio submit: frames=%zu, rms=%.1f, total_samples=%lld, kin=%d",
                    frames, std::sqrt(sum / static_cast<double>(frames)),
                    static_cast<long long>(total_frames_), ring_.params.kin);
      deferred_logs.emplace_back(message);
    }

    for (std::size_t i = 0; i < frames; ++i) {
      rx_pending_.push_back(static_cast<float>(samples[i * channels]));
    }

    std::size_t produced = 0;
    if (integral) {
      std::size_t const factor = static_cast<std::size_t>(rx_rate_ / kCaptureRate);
      produced = rx_pending_.size() / factor;
      rx_float_.resize(produced);
      if (produced > 0) {
        std::size_t consumed = 0;
        rx_resampler_.process(std::span<float>(rx_float_.data(), produced),
                              [&]() { return rx_pending_[consumed++]; });
        rx_pending_.erase(rx_pending_.begin(),
                          rx_pending_.begin() + static_cast<std::ptrdiff_t>(consumed));
      }
    } else {
      rx_fractional_.process(
          std::span<const float>(rx_pending_.data(), rx_pending_.size()), rx_float_);
      produced = rx_float_.size();
      rx_pending_.clear();
    }

    if (produced > 0) {
      std::size_t const size = ring_.samples.size();
      std::size_t cursor = static_cast<std::size_t>(ring_.params.kin);
      for (std::size_t i = 0; i < produced; ++i) {
        ring_.samples[(cursor + i) % size] = static_cast<std::int16_t>(
            std::clamp(std::lround(rx_float_[i]), -32768L, 32767L));
      }
      ring_.params.kin = static_cast<int>((cursor + produced) % size);
      total_frames_ += static_cast<int>(produced);
    }

    if (callbacks_.on_event && frames > 0) {
      auto const now = std::chrono::steady_clock::now();
      if (now - last_spectrum_time_ >= kSpectrumInterval) {
        last_spectrum_time_ = now;
        queue_spectrum(samples, frames, channels, buffer.format.sample_rate);
      }
    }

    schedule_decodes(&deferred_logs);
    capture_guard.unlock();
    for (auto const& message : deferred_logs) {
      if (callbacks_.on_log) log_info(message);
    }
    return true;
  }

  static bool valid_tx_frame(std::string_view frame, int bits) {
    if (frame.size() != 12 || bits < 0 || bits > 7) return false;
    return std::all_of(frame.begin(), frame.end(), [](char c) {
      return decoder::word_for(c) != 0xff;
    });
  }

  bool transmit_message(TxMessageRequest const& request) override {
    std::lock_guard<std::recursive_mutex> guard(tx_lifecycle_mutex_);
    if (tx_output_stopping_ || tx_shutdown_pending_.load() ||
        !tx_permitted()) return false;
    auto const submode = submode_for_value(request.submode);
    if (!submode) return false;

    protocol::varicode::MessageInfo info;
    auto const frames = protocol::varicode::build_message_frames(
        request.my_call, request.my_grid, request.selected_call, request.text,
        request.force_identify, request.force_data, request.submode, &info);
    if (frames.empty()) return false;

    auto const costas = protocol::costas(costas_for_value(request.submode));
    std::deque<TxFrame> built;
    for (auto const& entry : frames) {
      auto const& frame = entry.first;
      if (!valid_tx_frame(frame, entry.second)) return false;
      TxFrame tx;
      tx.bits = entry.second;
      tx.frame = frame;
      legacy_encode(tx.bits, costas, tx.frame.c_str(), tx.tones.data());
      built.push_back(std::move(tx));
    }

    {
      std::lock_guard<std::mutex> lock(tx_mutex_);
      tx_queue_.clear();
      tx_current_.reset();
      tx_queue_ = std::move(built);
      if (tx_queue_.empty()) return false;

      tx_settings_.submode = request.submode;
      tx_settings_.audio_frequency_hz = request.audio_frequency_hz;
      tx_settings_.tx_delay_s = request.tx_delay_s;
      tx_settings_.tuning = false;
      tx_active_.store(true);

      tx_modulator_.stop();
      tx_resampler_.reset();
      // Queue and schedule immediately to expose the legacy TX countdown
      // before the output adapter's first callback.
      tx_playback_epoch_ms_ = 0;
      tx_input_sample_index_ = 0;
      start_next_frame_locked();
    }

    if (!start_tx_output()) {
      stop_transmit();
      return false;
    }
    return true;
  }

  bool transmit_frame(TxFrameRequest const& request) override {
    std::lock_guard<std::recursive_mutex> guard(tx_lifecycle_mutex_);
    if (tx_output_stopping_ || tx_shutdown_pending_.load() ||
        !tx_permitted()) return false;
    auto const submode = submode_for_value(request.submode);
    if (!submode) return false;
    if (request.frame.size() < 12 ||
        !valid_tx_frame(request.frame.substr(0, 12), request.bits & 7)) return false;

    TxFrame tx;
    tx.bits = request.bits;
    tx.frame = request.frame.substr(0, 12);
    auto const costas = protocol::costas(costas_for_value(request.submode));
    legacy_encode(tx.bits, costas, tx.frame.c_str(), tx.tones.data());

    {
      std::lock_guard<std::mutex> lock(tx_mutex_);
      tx_queue_.clear();
      tx_current_.reset();
      tx_queue_.push_back(std::move(tx));
      tx_settings_.submode = request.submode;
      tx_settings_.audio_frequency_hz = request.audio_frequency_hz;
      tx_settings_.tx_delay_s = request.tx_delay_s;
      tx_settings_.tuning = false;
      tx_active_.store(true);

      tx_modulator_.stop();
      tx_resampler_.reset();
      // Queue and schedule immediately to expose the legacy TX countdown
      // before the output adapter's first callback.
      tx_playback_epoch_ms_ = 0;
      tx_input_sample_index_ = 0;
      start_next_frame_locked();
    }

    if (!start_tx_output()) {
      stop_transmit();
      return false;
    }
    return true;
  }

  bool start_tune(double audio_frequency_hz, int submode, double tx_delay_s) override {
    std::lock_guard<std::recursive_mutex> guard(tx_lifecycle_mutex_);
    if (tx_output_stopping_ || tx_shutdown_pending_.load() ||
        !tx_permitted()) return false;
    auto const mode = submode_for_value(submode);
    if (!mode) return false;

    {
      std::lock_guard<std::mutex> lock(tx_mutex_);
      tx_queue_.clear();
      tx_current_.reset();
      tx_settings_.submode = submode;
      tx_settings_.audio_frequency_hz = audio_frequency_hz;
      tx_settings_.tx_delay_s = tx_delay_s;
      tx_settings_.tuning = true;
      tx_active_.store(true);

      tx_resampler_.reset();
      std::array<int, protocol::kJs8NumSymbols> tones{};
      tx_modulator_.start(tones, mode->symbol_samples, mode->start_delay_ms,
                          mode->tx_seconds * 1000, tx_settings_.audio_frequency_hz,
                          tx_settings_.tx_delay_s, true);
    }

    if (!start_tx_output()) {
      stop_transmit();
      return false;
    }
    return true;
  }

  void stop_transmit() override {
    std::unique_lock<std::recursive_mutex> guard(tx_lifecycle_mutex_);
    bool stop_output = false;
    {
      std::lock_guard<std::mutex> lock(tx_mutex_);
      tx_active_.store(false);
      tx_queue_.clear();
      tx_settings_.tuning = false;
      tx_current_.reset();
      if (g_tx_render_engine == this && tx_output_started_.load()) {
        // An output callback must never synchronously join its own thread.
        // The dedicated shutdown worker closes the stream after return.
        {
          std::lock_guard<std::mutex> shutdown_lock(tx_shutdown_mutex_);
          tx_shutdown_pending_.store(true);
          tx_shutdown_requested_ = true;
        }
        tx_shutdown_cv_.notify_one();
      } else {
        stop_output = tx_output_started_.exchange(false);
      }
    }
    // The output adapter may join its callback thread here, so tx_mutex_ must
    // not be held: the render callback acquires it too.
    if (stop_output && deps_.audio_out) {
      // AudioOutput::stop() may wait for render callbacks to finish. Never
      // hold tx_lifecycle_mutex_ across that join: render callbacks can call
      // back into stop_transmit() or other engine operations.
      tx_output_stopping_ = true;
      guard.unlock();
      deps_.audio_out->stop();
      guard.lock();
      tx_output_stopping_ = false;
    }
    {
      std::lock_guard<std::mutex> lock(tx_mutex_);
      tx_modulator_.stop();
      tx_resampler_.reset();
    }
    if (deps_.rig) request_ptt(false);
  }

  bool is_transmitting() const override { return tx_active_.load(); }

  bool is_transmitting_audio() const override {
    return tx_modulator_.is_active() && tx_ready_.load();
  }

  int tx_milliseconds_until_audio() const override {
    return tx_modulator_.milliseconds_until_active();
  }

  void set_tx_ready(bool ready) override {
    // Android-port's public setter is unconditional, including for online
    // rigs. Track the explicit host override separately from automatic PTT
    // status so subsequent rig callbacks cannot silently undo the setting.
    host_tx_ready_override_.store(ready ? 1 : 0);
    tx_ready_.store(ready);
  }

  void set_tx_boost_enabled(bool enabled) override { tx_boost_enabled_.store(enabled); }

  void set_vox_lead_ms(int milliseconds) override {
    vox_lead_ms_.store(std::clamp(milliseconds, 0, 5000));
  }

  void set_submodes(int submodes) override {
    constexpr int known =
        (1 << static_cast<int>(SubmodeId::A)) |
        (1 << static_cast<int>(SubmodeId::B)) |
        (1 << static_cast<int>(SubmodeId::C)) |
        (1 << static_cast<int>(SubmodeId::E)) |
        (1 << static_cast<int>(SubmodeId::I));
    active_submodes_.store(submodes & known);
  }

  void set_sync_stats(bool enabled) override { sync_stats_.store(enabled); }

  void set_time_drift_ms(std::int64_t drift_ms) override {
    const auto previous = time_drift_ms_.exchange(
        drift_ms, std::memory_order_acq_rel);
    if (previous == drift_ms) return;

    // Capture owns ring realignment; changing clock settings from a UI
    // callback must not mutate a decoder's currently active window.
    realign_requested_.store(true, std::memory_order_release);
    tx_modulator_.set_clock_offset_ms(drift_ms);
    if (callbacks_.on_log) {
      char message[128];
      std::snprintf(message, sizeof(message), "Cycle clock correction: %lld ms",
                    static_cast<long long>(drift_ms));
      log_info(message);
    }
  }

  std::int64_t time_drift_ms() const override { return time_drift_ms_.load(); }

 private:
  struct TxFrame {
    std::array<int, protocol::kJs8NumSymbols> tones{};
    int bits = 0;
    std::string frame;
  };

  struct TxSettings {
    int submode = 0;
    double audio_frequency_hz = 0.0;
    double tx_delay_s = 0.0;
    bool tuning = false;
  };

  struct DecodeRequest {
    DecodeParams params;
    std::int64_t drift_ms = 0;
    std::uint64_t epoch = 0;
    std::int64_t frames_at_request = 0;
    std::uint64_t session = 0;
  };

  struct SpectrumTask {
    std::vector<std::int16_t> samples;
    std::size_t frames = 0;
    int channels = 1;
    int sample_rate = 0;
    std::uint64_t session = 0;
  };

  struct PendingEvent {
    events::Variant event;
    std::uint64_t session = 0;
  };

  // Invalidates asynchronous decode/spectrum work when a session ends.
  // Do not wait for the decode worker here: it may be processing a long
  // frame. Each event is checked against the generation before delivery.
  void invalidate_pending_work() {
    {
      std::lock_guard<std::mutex> lock(decode_mutex_);
      decode_queue_.clear();
    }
    {
      std::lock_guard<std::mutex> lock(spectrum_mutex_);
      spectrum_queue_.clear();
    }
    {
      std::lock_guard<std::mutex> lock(event_mutex_);
      event_queue_.clear();
    }
    event_cv_.notify_all();
  }

  // Host handlers are external code. Exceptions must not escape the decoder,
  // spectrum, audio, or rig worker thread (where they call std::terminate).
  bool on_event_dispatch_thread() {
    std::lock_guard<std::mutex> lock(event_mutex_);
    return event_dispatch_active_ &&
           event_dispatch_thread_ == std::this_thread::get_id();
  }

  void await_event_dispatch() {
    std::unique_lock<std::mutex> lock(event_mutex_);
    if (event_dispatch_thread_ == std::this_thread::get_id()) return;
    event_cv_.wait(lock, [this] { return !event_dispatch_active_; });
  }

  void report_error(std::string_view message) noexcept {
    if (!callbacks_.on_error) return;
    try {
      callbacks_.on_error(message);
    } catch (...) {
      // An error handler can itself fail; do not recurse or terminate.
    }
  }

  void log_info(std::string_view message,
                std::vector<std::string>* deferred_logs = nullptr) {
    if (!callbacks_.on_log) return;
    if (deferred_logs) {
      deferred_logs->emplace_back(message);
    } else {
      try {
        callbacks_.on_log(LogLevel::Info, message);
      } catch (...) {
        report_error("Host log callback threw");
      }
    }
  }

  SystemClock::time_point aligned_now() const {
    return detail::slot_clock::corrected_time(
        time_drift_ms_.load(std::memory_order_acquire));
  }

  // Re-anchor the capture ring to the wall clock. Called at construction and
  // whenever capture continuity or the clock offset changes.
  void align_ring(SystemClock::time_point wall,
                  std::vector<std::string>* deferred_logs = nullptr) {
    ring_drift_ms_ = time_drift_ms_.load();
    ++capture_epoch_;
    std::fill(ring_.samples.begin(), ring_.samples.end(), 0);

    const auto minute_phase = detail::slot_clock::phase_ms(
        detail::slot_clock::unix_ms(wall), kJs8NtMax * 1000);
    const int offset = detail::slot_clock::sample_at_phase(
        minute_phase, kCaptureRate);

    ring_.params.kin = offset;
    total_frames_ = offset;
    previous_position_ = offset;

    for (auto& slot : slots_) {
      slot.window = -1;
      slot.next = -1;
      slot.last_attempt = -1;
    }

    if (callbacks_.on_log) {
      char message[256];
      std::snprintf(message, sizeof(message),
                    "Ring buffer aligned to UTC minute: ms_in_minute=%lld, "
                    "offset_samples=%d, sample_rate=%d, drift_ms=%lld",
                    static_cast<long long>(minute_phase), offset, kCaptureRate,
                    static_cast<long long>(time_drift_ms_.load()));
      log_info(message, deferred_logs);
    }
  }

  void build_slot_plans() {
    auto const now = aligned_now();
    auto const tm = utc_parts(now);
    auto const ms = milliseconds_since_epoch(now);
    int const ms_in_second = static_cast<int>(ms % 1000);
    int const total_ms = tm.tm_sec * 1000 + ms_in_second;

    for (auto const& mode : protocol::submodes()) {
      // Build a plan for every known mode; active_submodes_ selects which are
      // actually decoded, so modes can be enabled at runtime.
      int const period = mode.tx_seconds * kCaptureRate;
      int const period_ms = mode.tx_seconds * 1000;
      int const ms_into_period = total_ms % period_ms;
      int ms_until_next = (period_ms - ms_into_period + mode.start_delay_ms) % period_ms;
      if (ms_until_next == 0) ms_until_next = period_ms;

      int required = mode.symbol_samples * protocol::kJs8NumSymbols +
                     static_cast<int>((0.5 + mode.start_delay_ms / 1000.0) * kCaptureRate);
      int retry = period;
      if (mode.id == SubmodeId::C) {
        required = mode.symbol_samples * protocol::kJs8NumSymbols;
        retry = kTurboRetryFrames;
      }

      slots_.push_back(SlotPlan{mode.id, period, required, retry, -1, -1, -1});

      if (callbacks_.on_log) {
        char message[256];
        std::snprintf(message, sizeof(message),
                      "Submode %.*s: period=%ds, delay=%dms, current=%d.%03ds, "
                      "next_in=%dms (%d samples)",
                      static_cast<int>(mode.name.size()), mode.name.data(),
                      mode.tx_seconds, mode.start_delay_ms, tm.tm_sec, ms_in_second,
                      ms_until_next, (ms_until_next * kCaptureRate) / 1000);
        log_info(message);
      }
    }
  }

  void set_window(SubmodeId id, int start, int size) {
    switch (id) {
      case SubmodeId::A: ring_.params.kposA = start; ring_.params.kszA = size; break;
      case SubmodeId::B: ring_.params.kposB = start; ring_.params.kszB = size; break;
      case SubmodeId::C: ring_.params.kposC = start; ring_.params.kszC = size; break;
      case SubmodeId::E: ring_.params.kposE = start; ring_.params.kszE = size; break;
      case SubmodeId::I: ring_.params.kposI = start; ring_.params.kszI = size; break;
    }
  }

  void fill_decode_metadata() {
    auto const tm = utc_parts(aligned_now());
    ring_.params.utc = tm.tm_hour * 10000 + tm.tm_min * 100 + tm.tm_sec;
    ring_.params.newdat = true;
    ring_.params.syncStats = sync_stats_.load();
  }

  void schedule_decodes(std::vector<std::string>* deferred_logs) {
    if (!callbacks_.on_event) return;

    int const position = ring_.params.kin;
    int const previous = previous_position_;
    bool any = false;
    ring_.params.nsubmodes = 0;

    for (auto& slot : slots_) {
      if ((active_submodes_.load() & (1 << static_cast<int>(slot.id))) == 0) continue;

      int start = 0;
      int size = 0;
      if (!slot_ready(slot, position, previous, start, size)) continue;

      int const wrapped = start % kRingFrames;
      set_window(slot.id, wrapped, size);
      ring_.params.nsubmodes |= 1 << static_cast<int>(slot.id);
      any = true;

      if (callbacks_.on_log) {
        auto const tm = utc_parts(SystemClock::now());
        auto const ms = milliseconds_since_epoch(SystemClock::now());
        char message[512];
        std::snprintf(message, sizeof(message),
                      "Decode ready at UTC %02d:%02d:%02d.%03d, submode_id=%d, "
                      "kpos_abs=%d, kpos_wrapped=%d, ksz=%d, k=%d, k0=%d, "
                      "total_samples=%lld",
                      tm.tm_hour, tm.tm_min, tm.tm_sec,
                      static_cast<int>(ms % 1000), static_cast<int>(slot.id),
                      start, wrapped, size, position, previous,
                      static_cast<long long>(total_frames_));
        log_info(message, deferred_logs);
      }
    }

    previous_position_ = position;
    if (!any) return;

    fill_decode_metadata();
    enqueue_decode(DecodeRequest{ring_.params, ring_drift_ms_, capture_epoch_, total_frames_,
                                session_generation_.load(std::memory_order_acquire)});
  }

  bool tx_permitted() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    return lifecycle_state_ == LifecycleState::Running &&
           running_.load();
  }

  bool start_tx_output() {
    // Re-key for each new message even if the output stream remains open
    // after the previous frame completed.
    if (deps_.rig && !request_ptt(true)) {
      report_error("Rig not online or state unavailable; TX refused");
      return false;
    }
    if (tx_output_started_.load()) return true;
    // Legacy API accepts a scheduled TX without an audio output dependency.
    if (!deps_.audio_out) return true;

    AudioStreamParams params;
    params.format.sample_rate = config_.tx_output_rate_hz;
    params.format.channels = 1;
    params.format.sample_type = SampleType::Int16;

    bool const ok = deps_.audio_out->start(
        params,
        [this](AudioOutputBuffer& buffer) { return render_tx_audio(buffer); },
        [this](std::string_view message) { report_error(message); });

    if (!ok) report_error("Failed to start audio output");
    tx_output_started_.store(ok);
    // The adapter may invoke an error handler synchronously from start().
    // If that handler cancelled the TX session, close the newly started
    // stream now that start() has returned to the controlling thread.
    if (ok && (!running_.load() || !tx_active_.load())) {
      tx_output_started_.store(false);
      deps_.audio_out->stop();
      return false;
    }
    return ok;
  }

  std::size_t render_tx_audio(AudioOutputBuffer& buffer) {
    ScopedTxAudioCallback callback_scope(this);
    std::size_t const bytes_per_sample =
        buffer.format.sample_type == SampleType::Float32 ? sizeof(float)
                                                         : sizeof(std::int16_t);
    if (bytes_per_sample == 0 || buffer.format.channels <= 0) return 0;

    std::size_t const frames =
        buffer.data.size() /
        (bytes_per_sample * static_cast<std::size_t>(buffer.format.channels));
    if (frames == 0) return 0;

    int const output_rate = buffer.format.sample_rate;
    if (output_rate <= 0) {
      std::fill(buffer.data.begin(), buffer.data.end(), std::byte{0});
      return buffer.data.size();
    }

    std::vector<std::string> deferred_logs;
    std::unique_lock<std::mutex> lock(tx_mutex_);

    if (!tx_output_logged_ && callbacks_.on_log) {
      char message[256];
      std::snprintf(message, sizeof(message),
                    "TX output format: rate=%d Hz, channels=%d, type=%s",
                    output_rate, buffer.format.channels,
                    buffer.format.sample_type == SampleType::Float32 ? "float"
                                                                     : "int16");
      deferred_logs.emplace_back(message);
      tx_output_logged_ = true;
    }

    if (tx_resampler_.input_rate() != kCaptureRate ||
        tx_resampler_.output_rate() != output_rate) {
      tx_resampler_.configure(kCaptureRate, output_rate);
    }

    if (tx_float_.size() < frames) tx_float_.resize(frames);
    auto const wall_now = SystemClock::now();
    auto const steady_now = SteadyClock::now();
    auto wall_playback = wall_now;
    if (buffer.playback_at != SteadyTimePoint{}) {
      wall_playback += std::chrono::duration_cast<SystemClock::duration>(
          buffer.playback_at - steady_now);
    }
    tx_playback_epoch_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(
        wall_playback.time_since_epoch()).count();
    tx_input_sample_index_ = 0;
    tx_resampler_.process(std::span<float>(tx_float_.data(), frames),
                          [this]() {
                            float const sample = next_tx_sample_locked();
                            ++tx_input_sample_index_;
                            return sample;
                          });

    float gain = std::clamp(config_.tx_output_gain, 0.0f, 1.0f);
    if (tx_boost_enabled_.load()) gain *= static_cast<float>(kBoostGain);
    if (gain != 1.0f) {
      for (std::size_t i = 0; i < frames; ++i) tx_float_[i] *= gain;
    }

    if (callbacks_.on_log && (++tx_log_counter_ % 250 == 0)) {
      double sum = 0.0;
      float peak = 0.0f;
      std::size_t limited = 0;
      for (std::size_t i = 0; i < frames; ++i) {
        double const v = tx_float_[i];
        sum += v * v;
        peak = std::max(peak, std::abs(tx_float_[i]));
        if (std::abs(tx_float_[i]) > 1.0f) ++limited;
      }
      char message[256];
      std::snprintf(message, sizeof(message),
                    "TX audio: rate=%d, channels=%d, frames=%zu, gain=%.3f, "
                    "rms=%.4f, peak=%.4f, soft_limited=%zu, active=%d, "
                    "tuning=%d, queue=%zu",
                    output_rate, buffer.format.channels, frames, gain,
                    frames > 0 ? std::sqrt(sum / static_cast<double>(frames)) : 0.0,
                    peak, limited, tx_active_.load() ? 1 : 0,
                    tx_settings_.tuning ? 1 : 0, tx_queue_.size());
      deferred_logs.emplace_back(message);
    }

    // AudioOutputBuffer contains bytes, not a promise of float/int16_t
    // alignment. Write through memcpy to avoid misaligned typed stores.
    auto* output = buffer.data.data();
    if (buffer.format.sample_type == SampleType::Float32) {
      bool const boost = tx_boost_enabled_.load();
      for (std::size_t i = 0; i < frames; ++i) {
        float const sample = boost ? std::tanh(tx_float_[i])
                                   : std::clamp(tx_float_[i], -1.0f, 1.0f);
        for (int ch = 0; ch < buffer.format.channels; ++ch) {
          std::memcpy(output, &sample, sizeof(sample));
          output += sizeof(sample);
        }
      }
    } else {
      bool const boost = tx_boost_enabled_.load();
      for (std::size_t i = 0; i < frames; ++i) {
        float v = tx_float_[i];
        v = boost ? std::tanh(v) : std::clamp(v, -1.0f, 1.0f);
        auto const sample = static_cast<std::int16_t>(std::lround(v * 32767.0f));
        for (int ch = 0; ch < buffer.format.channels; ++ch) {
          std::memcpy(output, &sample, sizeof(sample));
          output += sizeof(sample);
        }
      }
    }

    auto const bytes_written = frames * bytes_per_sample *
                               static_cast<std::size_t>(buffer.format.channels);
    lock.unlock();
    for (auto const& message : deferred_logs) {
      if (callbacks_.on_log) log_info(message);
    }
    return bytes_written;
  }

  float next_tx_sample_locked() {
    if (!tx_active_.load()) return 0.0f;

    if (tx_modulator_.is_idle()) {
      tx_current_.reset();
      if (!tx_queue_.empty()) {
        start_next_frame_locked();
      } else if (!tx_settings_.tuning) {
        tx_active_.store(false);
        if (deps_.rig) request_ptt(false);
        return 0.0f;
      }
    }

    // Readiness gates the waveform rather than rejecting a queued request.
    // Hold the current active symbol when external PTT is not yet keyed.
    if (tx_modulator_.is_active() && !tx_ready_.load()) return 0.0f;

    return tx_modulator_.next_sample();
  }

  void start_next_frame_locked() {
    if (tx_queue_.empty()) return;
    auto const mode = submode_for_value(tx_settings_.submode);
    if (!mode) {
      tx_queue_.clear();
      tx_active_.store(false);
      return;
    }

    TxFrame frame = std::move(tx_queue_.front());
    tx_queue_.pop_front();
    tx_current_ = frame;

    std::optional<std::int64_t> playback_epoch;
    if (tx_playback_epoch_ms_ > 0) {
      playback_epoch = tx_playback_epoch_ms_ +
                       tx_input_sample_index_ * 1000 / kCaptureRate;
    }
    tx_modulator_.start(frame.tones, mode->symbol_samples, mode->start_delay_ms,
                        mode->tx_seconds * 1000, tx_settings_.audio_frequency_hz,
                        tx_settings_.tx_delay_s, tx_settings_.tuning,
                        vox_lead_ms_.load(), playback_epoch);
  }

  bool compute_spectrum(std::int16_t const* data, std::size_t frames,
                        int channels, int sample_rate, events::Spectrum& out) {
    if (!data || frames == 0 || sample_rate <= 0) return false;

    std::size_t const limit = std::min(frames, kSpectrumMaxInput);
    std::size_t n = next_power_of_two(limit);
    if (n > limit) n >>= 1;
    if (n < kSpectrumMinInput) return false;

    double const two_pi = 2.0 * js8core::compat::numbers::pi;
    if (n != spectrum_n_) {
      spectrum_n_ = n;
      spectrum_window_.assign(n, 0.0);
      spectrum_samples_.assign(n, 0.0);
      spectrum_fft_.assign(n, std::complex<double>{});
      for (std::size_t i = 0; i < n; ++i) {
        spectrum_window_[i] =
            0.5 * (1.0 - std::cos(two_pi * static_cast<double>(i) /
                                  static_cast<double>(n - 1)));
      }
    }

    double power_sum = 0.0;
    double peak = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
      double const v = static_cast<double>(data[i * channels]);
      power_sum += v * v;
      peak = std::max(peak, std::abs(v));
      spectrum_samples_[i] = v * spectrum_window_[i];
    }

    for (std::size_t i = 0; i < n; ++i) {
      spectrum_fft_[i] = std::complex<double>(spectrum_samples_[i], 0.0);
    }
    fft_in_place(spectrum_fft_);

    out.bins.resize(kSpectrumBins);
    double const scale = 1.0 / static_cast<double>(n * n);
    std::size_t const source_bins = n / 2;
    double const source_bin_hz = static_cast<double>(sample_rate) / static_cast<double>(n);
    double const target_bin_hz =
        (static_cast<double>(sample_rate) / 2.0) / static_cast<double>(kSpectrumBins);

    for (std::size_t i = 0; i < kSpectrumBins; ++i) {
      double const pos = (static_cast<double>(i) * target_bin_hz) / source_bin_hz;
      auto const index = static_cast<std::size_t>(pos);
      double const frac = pos - static_cast<double>(index);
      float const v0 = index < source_bins
                           ? static_cast<float>(std::norm(spectrum_fft_[index]) * scale)
                           : 0.0f;
      float const v1 = (index + 1 < source_bins)
                           ? static_cast<float>(std::norm(spectrum_fft_[index + 1]) * scale)
                           : v0;
      out.bins[i] = v0 + static_cast<float>(frac) * (v1 - v0);
    }

    out.bin_hz = static_cast<float>(target_bin_hz);
    out.power_db =
        power_sum > 0.0 ? static_cast<float>(10.0 * std::log10(power_sum / n)) : 0.0f;
    out.peak_db = peak > 0.0 ? static_cast<float>(20.0 * std::log10(peak)) : 0.0f;
    return true;
  }

  int estimate_drift(DecodeState const& task, events::Decoded const& decoded) const {
    const auto mode = submode_for_value(decoded.mode);
    if (!mode) return static_cast<int>(task.drift_ms_at_capture);

    // These are the decoder's five timestamp origins in SubmodeId order.
    const std::array<int, 5> origins{
        task.params.kposA, task.params.kposB, task.params.kposC,
        task.params.kposE, task.params.kposI};
    const int origin = origins.at(static_cast<std::size_t>(mode->id));
    return detail::slot_clock::suggested_drift_ms(
        task.drift_ms_at_capture, origin, kCaptureRate,
        mode->start_delay_ms, mode->tx_seconds * 1000, decoded.xdt);
  }

  void emit_event(events::Variant const& event, std::uint64_t session) {
    if (!callbacks_.on_event) return;
    auto const current = [this, session] {
      return running_.load(std::memory_order_acquire) &&
             session == session_generation_.load(std::memory_order_acquire);
    };
    if (!current()) return;
    {
      std::unique_lock<std::mutex> lock(event_mutex_);
      if (!current()) return;
      // UI progress can be coalesced or dropped. Important results instead
      // apply backpressure to decode/spectrum workers, never to capture.
      auto const discardable = [](PendingEvent const& item) {
        return std::holds_alternative<events::Spectrum>(item.event) ||
               std::holds_alternative<events::SyncState>(item.event);
      };
      constexpr std::size_t kMaxPendingEvents = 128;
      if (std::holds_alternative<events::Spectrum>(event)) {
        auto old = std::find_if(event_queue_.begin(), event_queue_.end(),
                                [](PendingEvent const& item) {
                                  return std::holds_alternative<events::Spectrum>(item.event);
                                });
        if (old != event_queue_.end()) {
          *old = PendingEvent{event, session};
          return;
        }
      }
      while (event_queue_.size() >= kMaxPendingEvents) {
        auto old = std::find_if(event_queue_.begin(), event_queue_.end(), discardable);
        if (old != event_queue_.end()) {
          event_queue_.erase(old);
        } else if (std::holds_alternative<events::Spectrum>(event) ||
                   std::holds_alternative<events::SyncState>(event)) {
          return;
        } else {
          // Avoid unbounded memory growth without discarding decoded frames.
          // stop() clears the queue and wakes producers waiting here.
          event_cv_.wait(lock, [&] {
            return event_queue_.size() < kMaxPendingEvents || !current();
          });
          if (!current()) return;
        }
      }
      event_queue_.push_back(PendingEvent{event, session});
      if (event_dispatch_active_) return;
      event_dispatch_active_ = true;
      event_dispatch_thread_ = std::this_thread::get_id();
    }
    // Serial delivery, no event mutex while invoking host code.
    for (;;) {
      PendingEvent next;
      {
        std::lock_guard<std::mutex> lock(event_mutex_);
        if (event_queue_.empty()) {
          event_dispatch_active_ = false;
          event_dispatch_thread_ = {};
          event_cv_.notify_all();
          return;
        }
        next = std::move(event_queue_.front());
        event_queue_.pop_front();
      }
      event_cv_.notify_all();
      if (!running_.load(std::memory_order_acquire) ||
          next.session != session_generation_.load(std::memory_order_acquire)) continue;
      try {
        callbacks_.on_event(next.event);
      } catch (...) {
        // Drop only the offending event; keep the dispatcher alive and
        // notify the host safely. Throwing across a std::thread is fatal.
        report_error("Host event callback threw; event discarded");
      }
    }
  }

  // PTT commands are issued off the audio callback and carry the latest
  // confirmed radio state, preserving its mode, split and tuned frequencies.
  // Called under rig_mutex_. Retired queued commands count as completed
  // so the shutdown flush never waits for commands that no longer exist.
  void invalidate_rig_commands_locked() {
    ++rig_generation_;
    rig_completed_ += static_cast<std::uint64_t>(rig_commands_.size());
    rig_commands_.clear();
    ptt_requested_ = false;
    tx_ready_.store(false);
  }

  bool request_ptt(bool on) {
    if (!deps_.rig) return true;
    // Offline rigs (Android's placeholder) cannot receive PTT commands.
    // Online rigs still receive PTT requests even if the host has explicitly
    // allowed audio through the legacy readiness setter.
    {
      std::lock_guard<std::mutex> lock(rig_mutex_);
      if (!rig_state_known_ || !rig_state_.online) {
        // Offline/placeholder rigs cannot take PTT commands, but TX requests
        // must be queued for Android's external PTT monitor. They stay muted
        // until explicit host readiness, not until an implicit rig callback.
        return true;
      }
      if (ptt_requested_ == on) return true;
      ptt_requested_ = on;
      rig_commands_.push_back(RigCommand{on, rig_generation_});
      ++rig_enqueued_;
      if (on && host_tx_ready_override_.load() < 0) tx_ready_.store(false);
    }
    rig_cv_.notify_all();
    return true;
  }

  void rig_loop() {
    for (;;) {
      RigCommand command;
      RigState desired;
      {
        std::unique_lock<std::mutex> lock(rig_mutex_);
        rig_cv_.wait(lock, [this] { return rig_stopping_ || !rig_commands_.empty(); });
        if (rig_commands_.empty() && rig_stopping_) return;
        command = rig_commands_.front();
        rig_commands_.pop_front();
        desired = rig_state_;
      }
      // Even already-dequeued work is invalidated by a rig disconnect.
      bool current = false;
      {
        std::lock_guard<std::mutex> lock(rig_mutex_);
        current = command.generation == rig_generation_ &&
                  rig_state_known_ && rig_state_.online;
        if (current) desired = rig_state_;
      }
      if (current) {
        desired.ptt = command.on;
        try {
          deps_.rig->apply(desired, rig_sequence_.fetch_add(1) + 1);
        } catch (...) {
          report_error("RigControl::apply threw while changing PTT");
          tx_ready_.store(false);
        }
      }
      {
        std::lock_guard<std::mutex> lock(rig_mutex_);
        ++rig_completed_;
        // A stale key-up already submitted to apply() cannot be retracted.
        // If it completes after a reconnect, send unkey ahead of any new
        // session's PTT-on. RigControl must honor ordered apply requests.
        if (current && command.on && command.generation != rig_generation_ &&
            rig_state_known_ && rig_state_.online) {
          rig_commands_.push_front(RigCommand{false, rig_generation_});
          ++rig_enqueued_;
          tx_ready_.store(false);
        }
      }
      rig_cv_.notify_all();
    }
  }

  void flush_rig_commands() {
    std::unique_lock<std::mutex> lock(rig_mutex_);
    auto const target = rig_enqueued_;
    rig_cv_.wait_for(lock, std::chrono::seconds(2), [this, target] {
      return rig_completed_ >= target;
    });
    // Acknowledgment is asynchronous; allow the radio a short interval
    // to confirm key-up before disconnecting the adapter.
    rig_cv_.wait_for(lock, std::chrono::milliseconds(750), [this] {
      return !rig_state_known_ || !rig_state_.online || !rig_state_.ptt;
    });
  }

  // Real AudioOutput::stop() may wait for its render callback thread to
  // exit. Keep its invocation on a separate control thread when stop_transmit
  // originates on that callback, rather than leaving the stream open forever.
  void tx_shutdown_loop() {
    for (;;) {
      {
        std::unique_lock<std::mutex> lock(tx_shutdown_mutex_);
        tx_shutdown_cv_.wait(lock, [this] {
          return tx_shutdown_requested_ || tx_shutdown_stopping_;
        });
        if (!tx_shutdown_requested_ && tx_shutdown_stopping_) return;
        tx_shutdown_requested_ = false;
      }
      stop_transmit();
      {
        std::lock_guard<std::mutex> lock(tx_shutdown_mutex_);
        tx_shutdown_pending_.store(false);
      }
      tx_shutdown_cv_.notify_all();
    }
  }

  void await_tx_shutdown() {
    // Never wait for a worker that must join the current render callback.
    if (g_tx_render_engine == this ||
        std::this_thread::get_id() == tx_shutdown_thread_.get_id()) return;
    std::unique_lock<std::mutex> lock(tx_shutdown_mutex_);
    tx_shutdown_cv_.wait(lock, [this] {
      return !tx_shutdown_pending_.load();
    });
  }

  void shutdown_tx_thread() {
    if (!tx_shutdown_thread_.joinable()) return;
    {
      std::lock_guard<std::mutex> lock(tx_shutdown_mutex_);
      tx_shutdown_stopping_ = true;
    }
    tx_shutdown_cv_.notify_all();
    tx_shutdown_thread_.join();
  }

  void shutdown_rig_thread() {
    if (!deps_.rig) return;
    {
      std::lock_guard<std::mutex> lock(rig_mutex_);
      rig_stopping_ = true;
    }
    rig_cv_.notify_all();
    if (rig_thread_.joinable()) rig_thread_.join();
  }

  void start_decode_thread() {
    decode_thread_ = std::thread([this]() { decode_loop(); });
  }

  void shutdown_decode_thread() {
    {
      std::lock_guard<std::mutex> lock(decode_mutex_);
      decode_stop_ = true;
      decode_queue_.clear();
    }
    decode_cv_.notify_one();
    if (decode_thread_.joinable()) decode_thread_.join();
  }

  void start_spectrum_thread() {
    spectrum_thread_ = std::thread([this]() { spectrum_loop(); });
  }

  void shutdown_spectrum_thread() {
    {
      std::lock_guard<std::mutex> lock(spectrum_mutex_);
      spectrum_stop_ = true;
      spectrum_queue_.clear();
    }
    spectrum_cv_.notify_one();
    if (spectrum_thread_.joinable()) spectrum_thread_.join();
  }

  // Carry forward any submode windows that were ready only in the snapshot
  // being replaced.
  static void merge_windows(DecodeParams& into, DecodeParams const& older) {
    struct Window {
      int bit;
      int DecodeParams::*position;
      int DecodeParams::*size;
    };
    static constexpr Window windows[] = {
        {1 << static_cast<int>(SubmodeId::A), &DecodeParams::kposA, &DecodeParams::kszA},
        {1 << static_cast<int>(SubmodeId::B), &DecodeParams::kposB, &DecodeParams::kszB},
        {1 << static_cast<int>(SubmodeId::C), &DecodeParams::kposC, &DecodeParams::kszC},
        {1 << static_cast<int>(SubmodeId::E), &DecodeParams::kposE, &DecodeParams::kszE},
        {1 << static_cast<int>(SubmodeId::I), &DecodeParams::kposI, &DecodeParams::kszI},
    };
    for (auto const& window : windows) {
      if ((older.nsubmodes & window.bit) == 0 ||
          (into.nsubmodes & window.bit) != 0) {
        continue;
      }
      into.*window.position = older.*window.position;
      into.*window.size = older.*window.size;
      into.nsubmodes |= window.bit;
    }
  }

  void enqueue_decode(DecodeRequest snapshot) {
    {
      std::lock_guard<std::mutex> lock(decode_mutex_);
      if (!running_.load(std::memory_order_acquire) ||
          snapshot.session != session_generation_.load(std::memory_order_acquire)) return;
      if (decode_queue_.empty()) {
        decode_queue_.push_back(std::move(snapshot));
      } else {
        merge_windows(snapshot.params, decode_queue_.back().params);
        decode_queue_.back() = std::move(snapshot);
      }
    }
    decode_cv_.notify_one();
  }

  void queue_spectrum(std::int16_t const* data, std::size_t frames, int channels,
                      int sample_rate) {
    if (!data || frames == 0 || sample_rate <= 0) return;
    std::size_t const count = std::min(frames, kSpectrumMaxInput);
    SpectrumTask task;
    task.frames = count;
    task.channels = 1;
    task.sample_rate = sample_rate;
    task.session = session_generation_.load(std::memory_order_acquire);
    task.samples.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
      task.samples[i] = data[i * channels];
    }
    {
      std::lock_guard<std::mutex> lock(spectrum_mutex_);
      if (!running_.load(std::memory_order_acquire) ||
          task.session != session_generation_.load(std::memory_order_acquire)) return;
      // Spectrum frames are disposable UI data: keep only the newest.
      spectrum_queue_.clear();
      spectrum_queue_.push_back(std::move(task));
    }
    spectrum_cv_.notify_one();
  }

  void decode_loop() {
    for (;;) {
      DecodeRequest request;
      {
        std::unique_lock<std::mutex> lock(decode_mutex_);
        decode_cv_.wait(lock, [&]() {
          return decode_stop_ || !decode_queue_.empty();
        });
        if (decode_stop_) return;
        request = std::move(decode_queue_.front());
        decode_queue_.pop_front();
      }
      if (!running_.load(std::memory_order_acquire) ||
          request.session != session_generation_.load(std::memory_order_acquire)) continue;

      DecodeState task;
      task.samples.resize(kRingFrames);
      task.params = request.params;
      task.drift_ms_at_capture = request.drift_ms;
      bool stale = false;
      constexpr std::size_t kCopyChunk = 8192;
      // Copy in bounded chunks to avoid blocking the real-time capture
      // callback for an entire 720K-sample memcpy.
      for (std::size_t offset = 0; offset < task.samples.size(); offset += kCopyChunk) {
        std::lock_guard<std::mutex> lock(capture_mutex_);
        if (!running_.load(std::memory_order_acquire) ||
            request.session != session_generation_.load(std::memory_order_acquire) ||
            request.epoch != capture_epoch_ ||
            total_frames_ - request.frames_at_request >= kRingFrames) {
          stale = true;
          break;
        }
        std::size_t const n = std::min(kCopyChunk, task.samples.size() - offset);
        std::copy_n(ring_.samples.begin() + static_cast<std::ptrdiff_t>(offset),
                    n, task.samples.begin() + static_cast<std::ptrdiff_t>(offset));
      }
      if (stale || !running_.load(std::memory_order_acquire) ||
          request.session != session_generation_.load(std::memory_order_acquire)) continue;

      if (callbacks_.on_log) {
        char message[512];
        std::snprintf(message, sizeof(message),
                      "Calling legacy_decode: nsubmodes=0x%x, freq_range=%d-%d Hz, "
                      "nfqso=%d Hz, sample_rate=%d, buffer_size=%zu, callback=%s",
                      task.params.nsubmodes, task.params.nfa, task.params.nfb,
                      task.params.nfqso, config_.sample_rate_hz, task.samples.size(),
                      callbacks_.on_event ? "SET" : "NULL");
        log_info(message);
      }

      std::size_t const count = legacy_decode(task, [this, &task, session = request.session](events::Variant const& event) {
        if (!running_.load(std::memory_order_acquire) ||
            session != session_generation_.load(std::memory_order_acquire)) return;
        if (auto const* decoded = std::get_if<events::Decoded>(&event)) {
          auto out = *decoded;
          out.drift_ms = estimate_drift(task, out);
          out.capture_drift_ms = task.drift_ms_at_capture;
          emit_event(events::Variant{std::move(out)}, session);
          return;
        }
        emit_event(event, session);
      });

      if (callbacks_.on_log && running_.load(std::memory_order_acquire) &&
          request.session == session_generation_.load(std::memory_order_acquire)) {
        char message[256];
        std::snprintf(message, sizeof(message), "legacy_decode returned: %zu decodes",
                      count);
        log_info(message);
      }
    }
  }

  void spectrum_loop() {
    for (;;) {
      SpectrumTask task;
      {
        std::unique_lock<std::mutex> lock(spectrum_mutex_);
        spectrum_cv_.wait(lock, [&]() {
          return spectrum_stop_ || !spectrum_queue_.empty();
        });
        if (spectrum_stop_) return;
        task = std::move(spectrum_queue_.front());
        spectrum_queue_.pop_front();
      }
      if (!running_.load(std::memory_order_acquire) ||
          task.session != session_generation_.load(std::memory_order_acquire)) continue;

      auto& spectrum = std::get<events::Spectrum>(spectrum_event_);
      if (compute_spectrum(task.samples.data(), task.frames, task.channels,
                           task.sample_rate, spectrum)) {
        emit_event(spectrum_event_, task.session);
      }
    }
  }

  EngineConfig config_;
  EngineCallbacks callbacks_;
  EngineDependencies deps_;

  // Capture state. `ring_` is mutated on the capture thread and copied by the
  // decode worker under capture_mutex_.
  DecodeState ring_;
  std::vector<SlotPlan> slots_;
  int previous_position_ = 0;
  std::int64_t total_frames_ = 0;
  int capture_log_counter_ = 0;

  std::atomic<std::int64_t> time_drift_ms_{0};
  std::atomic<int> active_submodes_{0};
  std::atomic<bool> realign_requested_{false};
  std::int64_t ring_drift_ms_ = 0;

  std::atomic<bool> running_{false};
  std::atomic<bool> awaiting_first_buffer_{true};
  std::mutex capture_mutex_;
  std::optional<SteadyTimePoint> expected_capture_at_;
  bool capture_delivery_timestamps_ = false;  // Protected by capture_mutex_.
  std::uint64_t capture_epoch_ = 0;

  // Receive resampling. All members below are capture-thread only.
  dsp::Resampler rx_resampler_;
  dsp::FractionalRxResampler rx_fractional_;
  int rx_rate_ = 0;
  std::vector<float> rx_pending_;
  std::vector<float> rx_float_;
  std::vector<std::int16_t> rx_input_;

  // Transmit state.
  enum class LifecycleState { Stopped, Starting, Running, Stopping };
  std::mutex lifecycle_mutex_;
  std::condition_variable lifecycle_cv_;
  std::thread::id lifecycle_owner_{};
  LifecycleState lifecycle_state_ = LifecycleState::Stopped;
  bool cancel_start_ = false;
  std::atomic<std::uint64_t> session_generation_{0};
  std::recursive_mutex tx_lifecycle_mutex_;
  bool tx_output_stopping_ = false; // Protected by tx_lifecycle_mutex_.
  std::mutex tx_shutdown_mutex_;
  std::condition_variable tx_shutdown_cv_;
  std::thread tx_shutdown_thread_;
  std::atomic<bool> tx_shutdown_pending_{false};
  bool tx_shutdown_requested_ = false;
  bool tx_shutdown_stopping_ = false;
  std::mutex tx_mutex_;
  std::deque<TxFrame> tx_queue_;
  std::optional<TxFrame> tx_current_;
  TxSettings tx_settings_{};
  tx::Modulator tx_modulator_;
  dsp::Resampler tx_resampler_;
  std::vector<float> tx_float_;
  int tx_log_counter_ = 0;
  bool tx_output_logged_ = false;
  std::int64_t tx_playback_epoch_ms_ = 0;
  std::int64_t tx_input_sample_index_ = 0;
  std::atomic<bool> tx_active_{false};
  std::atomic<bool> tx_output_started_{false};
  std::atomic<bool> tx_ready_{true};
  // -1: rely on rig PTT feedback; 0/1: host explicitly closed/opened audio.
  std::atomic<int> host_tx_ready_override_{-1};
  std::atomic<bool> tx_boost_enabled_{config_.tx_output_gain_boost_enabled};
  std::atomic<int> vox_lead_ms_{0};

  // Spectrum state. The worker owns the FFT buffers; only the event variant is
  // shared, and emit_event serializes callbacks.
  std::chrono::steady_clock::time_point last_spectrum_time_{};
  std::size_t spectrum_n_ = 0;
  std::vector<double> spectrum_window_;
  std::vector<double> spectrum_samples_;
  std::vector<std::complex<double>> spectrum_fft_;
  events::Variant spectrum_event_{events::Spectrum{}};
  std::mutex event_mutex_;
  std::deque<PendingEvent> event_queue_;
  std::condition_variable event_cv_;
  bool event_dispatch_active_ = false;
  std::thread::id event_dispatch_thread_{};

  std::thread rig_thread_;
  std::mutex rig_mutex_;
  std::condition_variable rig_cv_;
  RigState rig_state_{};
  bool rig_state_known_{false};
  bool ptt_requested_{false};
  bool rig_stopping_{false};
  struct RigCommand {
    bool on = false;
    std::uint64_t generation = 0;
  };
  std::deque<RigCommand> rig_commands_;
  std::uint64_t rig_generation_{0};
  std::uint64_t rig_enqueued_{0};
  std::uint64_t rig_completed_{0};
  std::atomic<unsigned> rig_sequence_{0};

  std::thread decode_thread_;
  std::mutex decode_mutex_;
  std::condition_variable decode_cv_;
  std::deque<DecodeRequest> decode_queue_;
  bool decode_stop_ = false;

  std::thread spectrum_thread_;
  std::mutex spectrum_mutex_;
  std::condition_variable spectrum_cv_;
  std::deque<SpectrumTask> spectrum_queue_;
  bool spectrum_stop_ = false;

  std::atomic<bool> sync_stats_{false};
};

}  // namespace

std::unique_ptr<Js8Engine> make_engine(EngineConfig const& config,
                                       EngineCallbacks callbacks,
                                       EngineDependencies deps) {
  return std::make_unique<Js8EngineImpl>(config, std::move(callbacks), deps);
}

}  // namespace js8core

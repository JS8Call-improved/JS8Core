#include "js8core/tx/modulator.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>

#include "js8core/compat/numbers.hpp"

#include "../engine/clock_math.hpp"

namespace js8core::tx {
namespace {

constexpr double kTwoPi = 2.0 * js8core::compat::numbers::pi;
constexpr int kMaxVoxLeadMs = 5000;
constexpr int kRampDenominator = 200;  // five-millisecond ramp at 12 kHz
constexpr double kToneTailSymbols = 0.017;
constexpr double kTailGainStep = 0.98;

// Returns the delay before the next complete JS8 slot. A request that arrives
// partway through a slot always waits for the following boundary; shortening
// the lead-in would transmit a partial frame.
std::int64_t slot_wait_ms(std::int64_t period_offset_ms,
                          std::int64_t tx_delay_ms,
                          std::int64_t start_delay_ms,
                          std::int64_t period_ms) {
  if (period_offset_ms + tx_delay_ms >= period_ms) {
    return period_ms - period_offset_ms + start_delay_ms;
  }
  if (period_offset_ms < start_delay_ms) {
    return start_delay_ms - period_offset_ms;
  }
  return period_ms - period_offset_ms + start_delay_ms;
}

}  // namespace

void Modulator::set_clock_offset_ms(std::int64_t correction_ms) noexcept {
  clock_offset_ms_.store(correction_ms, std::memory_order_relaxed);
}

void Modulator::start(std::array<int, protocol::kJs8NumSymbols> const& tones,
                      int symbol_samples, int start_delay_ms, int period_ms,
                      double audio_frequency_hz, double tx_delay_s,
                      bool tuning, int vox_lead_ms) {
  start(tones, symbol_samples, start_delay_ms, period_ms, audio_frequency_hz,
        tx_delay_s, tuning, vox_lead_ms, std::nullopt);
}

void Modulator::start(std::array<int, protocol::kJs8NumSymbols> const& tones,
                      int symbol_samples,
                      int start_delay_ms,
                      int period_ms,
                      double audio_frequency_hz,
                      double tx_delay_s,
                      bool tuning,
                      int vox_lead_ms,
                      std::optional<std::int64_t> playback_epoch_ms) {
  if (symbol_samples <= 0 || period_ms <= 0) {
    stop();
    return;
  }

  tones_ = tones;
  tuning_ = tuning;
  symbol_samples_ = symbol_samples;
  base_rate_ = protocol::kJs8RxSampleRate;
  tone_spacing_ =
      static_cast<double>(base_rate_) / static_cast<double>(symbol_samples_);
  audio_frequency_ = audio_frequency_hz;
  audio_frequency0_ = 0.0;
  phi_ = 0.0;
  dphi_ = 0.0;
  amp_ = 1.0;
  ic_ = 0;
  isym0_ = std::numeric_limits<std::uint64_t>::max();
  vox_sample_ = 0;
  silent_frames_.store(0);

  const int lead_ms =
      tuning_ ? 0 : std::clamp(vox_lead_ms, 0, kMaxVoxLeadMs);
  vox_lead_frames_ =
      static_cast<std::int64_t>(lead_ms) * base_rate_ / 1000;

  if (!tuning_) {
    const auto reference_ms = playback_epoch_ms.value_or(
        detail::slot_clock::unix_ms(std::chrono::system_clock::now()));
    const auto corrected_ms =
        reference_ms + clock_offset_ms_.load(std::memory_order_relaxed);
    const auto offset = detail::slot_clock::phase_ms(corrected_ms, period_ms);
    const std::int64_t delay =
        std::max<std::int64_t>(0, static_cast<std::int64_t>(tx_delay_s * 1000.0));

    std::int64_t wait =
        slot_wait_ms(offset, delay, start_delay_ms, period_ms);
    if (lead_ms > 0 && wait < lead_ms) wait += period_ms;

    silent_frames_.store(wait * base_rate_ / 1000);
  }

  state_.store(silent_frames_.load() > 0 ? State::Synchronizing
                                         : State::Active);
}

void Modulator::stop() {
  state_.store(State::Idle);
  silent_frames_.store(0);
  ic_ = 0;
  phi_ = 0.0;
}

float Modulator::next_sample() {
  const State state = state_.load();
  if (state == State::Idle) return 0.0f;

  if (state == State::Synchronizing) {
    const std::int64_t remaining = silent_frames_.load();
    if (remaining > 0) {
      if (silent_frames_.fetch_sub(1) == 1) {
        state_.store(State::Active);
      }

      if (vox_lead_frames_ > 0 && remaining <= vox_lead_frames_) {
        const std::int64_t ramp =
            std::max<std::int64_t>(1, base_rate_ / kRampDenominator);
        const double gain =
            std::min({1.0,
                      static_cast<double>(vox_sample_ + 1) / ramp,
                      static_cast<double>(remaining) / ramp});
        const float sample = static_cast<float>(
            gain * std::sin(kTwoPi * audio_frequency_ * vox_sample_ / base_rate_));
        ++vox_sample_;
        return sample;
      }
      return 0.0f;
    }
    state_.store(State::Active);
  }

  const std::int64_t tail_start =
      tuning_ ? std::numeric_limits<std::int64_t>::max()
              : static_cast<std::int64_t>(
                    (protocol::kJs8NumSymbols - kToneTailSymbols) *
                    symbol_samples_);
  const std::int64_t frame_end =
      tuning_ ? std::numeric_limits<std::int64_t>::max()
              : static_cast<std::int64_t>(protocol::kJs8NumSymbols *
                                          symbol_samples_);

  if (static_cast<std::int64_t>(ic_) >= frame_end) {
    state_.store(State::Idle);
    phi_ = 0.0;
    return 0.0f;
  }

  const std::uint64_t symbol =
      tuning_ ? 0 : ic_ / static_cast<std::uint64_t>(symbol_samples_);

  if (symbol != isym0_ || audio_frequency_ != audio_frequency0_) {
    const double frequency =
        audio_frequency_ + static_cast<double>(tones_[symbol]) * tone_spacing_;
    dphi_ = kTwoPi * frequency / static_cast<double>(base_rate_);
    isym0_ = symbol;
    audio_frequency0_ = audio_frequency_;
  }

  phi_ += dphi_;
  if (phi_ > kTwoPi) phi_ -= kTwoPi;

  if (static_cast<std::int64_t>(ic_) > tail_start) amp_ *= kTailGainStep;

  const float sample = static_cast<float>(amp_ * std::sin(phi_));
  ++ic_;

  if (amp_ <= 0.0) {
    state_.store(State::Idle);
    phi_ = 0.0;
  }

  return sample;
}

int Modulator::milliseconds_until_active() const {
  const State state = state_.load();
  if (state == State::Idle) return -1;
  if (state == State::Active) return 0;

  const std::int64_t frames = silent_frames_.load();
  if (frames <= 0) return 0;

  return static_cast<int>(
      (frames * 1000 + protocol::kJs8RxSampleRate - 1) /
      protocol::kJs8RxSampleRate);
}

}  // namespace js8core::tx

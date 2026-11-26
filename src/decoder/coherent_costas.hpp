#pragma once

#include "js8core/decode_control.hpp"
#include "js8core/protocol/costas.hpp"

#include <array>
#include <complex>
#include <optional>
#include <span>
#include <vector>

namespace js8core::decoder {

struct CoherentCostasScore {
  // Complete blocks with nonzero, finite observed energy have a score in [0, 1].
  std::array<std::optional<double>, 3> blocks{};
  // Second-best block coherence. One bright block cannot carry a hypothesis.
  double support = 0.0;
};

struct CoherentCostasPeak {
  int frame_start;
  double frequency_offset_hz;
  double support;
};

// Reusable phase-continuous baseband reference for a single submode. Only
// phase within each known pilot block is constrained.
class CoherentCostasReference {
 public:
  static std::optional<CoherentCostasReference> make(int samples_per_symbol,
                                                    protocol::CostasArray const& costas);

  // `channel` is a contiguous view of observed samples (no padding). Its
  // lowest tone is at 0 Hz. A positive trial offset matches a positively
  // shifted signal. No score is returned unless at least two blocks are usable.
  std::optional<CoherentCostasScore> score(std::span<std::complex<float> const> channel,
                                          int frame_start, double sample_rate_hz,
                                          double trial_offset_hz) const;

  // Fixed local grid around the seed and channel center. A denied or canceled
  // search never returns a partial peak.
  std::optional<CoherentCostasPeak> search(std::span<std::complex<float> const> channel,
                                          int seed_start, double sample_rate_hz,
                                          double min_offset_hz, double max_offset_hz,
                                          DecodeControl const& control) const;

 private:
  CoherentCostasReference() = default;

  int samples_per_symbol_ = 0;
  std::array<std::vector<std::complex<double>>, 3> conjugate_templates_;
};

}  // namespace js8core::decoder

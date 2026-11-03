#include "js8core/dsp/flatten.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <memory>
#include <numeric>
#include <vector>

#include <Eigen/Dense>

#include "js8core/compat/numbers.hpp"

// The flattener removes a slowly varying spectral baseline. It samples the
// input at a fixed phase offset, fits a low-order polynomial through those
// samples, smooths the polynomial with a three-point moving average, and
// subtracts the result. Input is assumed to be in dB.

namespace js8core::dsp {
namespace {

constexpr int kFitOrder = 5;
constexpr int kTermCount = kFitOrder + 1;
constexpr int kSamplePercent = 10;
constexpr int kNodeCount = kTermCount / 2;

static_assert(kFitOrder % 2 == 1, "fit order must be odd");
static_assert(kSamplePercent >= 0 && kSamplePercent <= 100,
              "sample percentage out of range");

using NodeList = std::array<double, kNodeCount>;

NodeList chebyshev_nodes() {
  NodeList nodes{};
  for (int i = 0; i < kNodeCount; ++i) {
    const double angle =
        js8core::compat::numbers::pi * (2.0 * i + 1.0) / (2.0 * kNodeCount);
    nodes[static_cast<std::size_t>(i)] = std::cos(angle);
  }
  return nodes;
}

const NodeList kNodes = chebyshev_nodes();

}  // namespace

class Flatten::Impl {
 public:
  explicit Impl(std::size_t length = 0) { resize(length); }

  void operator()(float* data, std::size_t length) {
    if (length != capacity_) resize(length);
    if (length == 0) return;

    // Rotate the input by the sampling offset so the fit sees a uniformly
    // distributed subset of the spectrum.
    const std::size_t offset = length * kSamplePercent / 100;
    for (std::size_t i = 0; i < length; ++i) {
      const std::size_t source = (offset + i >= length) ? offset + i - length
                                                        : offset + i;
      samples_[i] = data[source];
    }

    // Build the Vandermonde design matrix at Chebyshev node positions.
    for (std::size_t row = 0; row < length; ++row) {
      const std::size_t node_index =
          static_cast<std::size_t>(row) * kNodes.size() / length;
      const double position =
          0.5 * (kNodes[node_index] + 1.0) * static_cast<double>(length - 1);
      double power = 1.0;
      for (int term = 0; term < kTermCount; ++term) {
        design_(static_cast<Eigen::Index>(row), term) =
            static_cast<float>(power);
        power *= position;
      }
    }

    Eigen::Matrix<float, Eigen::Dynamic, 1> target(
        static_cast<Eigen::Index>(length));
    for (std::size_t i = 0; i < length; ++i) {
      target(static_cast<Eigen::Index>(i)) = samples_[i];
    }

    const Eigen::VectorXf coefficients =
        design_.colPivHouseholderQr().solve(target);

    // Evaluate the fitted polynomial over the span.
    for (std::size_t i = 0; i < length; ++i) {
      double value = 0.0;
      double power = 1.0;
      for (int term = 0; term < kTermCount; ++term) {
        value += coefficients(term) * power;
        power *= static_cast<double>(i);
      }
      baseline_[i] = static_cast<float>(value);
    }

    // Three-point moving average smooths the baseline.
    for (std::size_t i = 1; i + 1 < length; ++i) {
      baseline_[i] =
          (baseline_[i - 1] + baseline_[i] + baseline_[i + 1]) / 3.0f;
    }

    for (std::size_t i = 0; i < length; ++i) {
      data[i] -= baseline_[i];
    }
  }

 private:
  void resize(std::size_t length) {
    capacity_ = length;
    samples_.assign(length, 0.0f);
    baseline_.assign(length, 0.0f);
    design_.resize(static_cast<Eigen::Index>(length), kTermCount);
  }

  std::size_t capacity_ = 0;
  std::vector<float> samples_;
  std::vector<float> baseline_;
  Eigen::Matrix<float, Eigen::Dynamic, Eigen::Dynamic> design_;
};

Flatten::Flatten(bool enabled)
    : impl_(enabled ? std::make_unique<Impl>() : nullptr) {}

Flatten::~Flatten() = default;

void Flatten::operator()(bool enabled) {
  if (enabled && !impl_) {
    impl_ = std::make_unique<Impl>();
  } else if (!enabled) {
    impl_.reset();
  }
}

void Flatten::operator()(float* data, std::size_t size) {
  if (impl_) (*impl_)(data, size);
}

}  // namespace js8core::dsp

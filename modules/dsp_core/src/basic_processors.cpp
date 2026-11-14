#include "dsp_core/basic_processors.h"

#include <algorithm>
#include <cmath>

namespace ms::dsp_core {
namespace {
constexpr float kMinGainDb = -60.0f;
constexpr float kMaxGainDb = 60.0f;

float db_to_linear(float db) { return std::pow(10.0f, db / 20.0f); }
}  // namespace

void GainProcessor::prepare(const ProcessContext& context) {
  // 20 ms 到达目标值的指数系数
  const double samples = std::max(1.0, 0.02 * context.sampleRate);
  ramp_coeff_ = static_cast<float>(1.0 - std::exp(-1.0 / samples));
  current_gain_ = db_to_linear(target_gain_db_.load());
}

void GainProcessor::process(float* data, int frames) {
  const float target = db_to_linear(std::clamp(target_gain_db_.load(), kMinGainDb, kMaxGainDb));
  for (int i = 0; i < frames; ++i) {
    current_gain_ += (target - current_gain_) * ramp_coeff_;
    data[i] *= current_gain_;
  }
}

void GainProcessor::reset() { current_gain_ = db_to_linear(target_gain_db_.load()); }

void GainProcessor::set_gain_db(float db) noexcept {
  target_gain_db_.store(std::clamp(db, kMinGainDb, kMaxGainDb));
}

void DcBlocker::prepare(const ProcessContext& context) {
  // 截止频率约 20 Hz 的一阶高通
  const double rc = 1.0 / (2.0 * 3.141592653589793 * 20.0);
  const double dt = 1.0 / context.sampleRate;
  coefficient_ = static_cast<float>(rc / (rc + dt));
  reset();
}

void DcBlocker::process(float* data, int frames) {
  for (int i = 0; i < frames; ++i) {
    const float input = data[i];
    const float output = input - last_input_ + coefficient_ * last_output_;
    last_input_ = input;
    last_output_ = output;
    data[i] = output;
  }
}

void DcBlocker::reset() {
  last_input_ = 0.0f;
  last_output_ = 0.0f;
}

void Limiter::prepare(const ProcessContext& context) {
  const double samples = std::max(1.0, 0.05 * context.sampleRate);
  release_coeff_ = static_cast<float>(1.0 - std::exp(-1.0 / samples));
  envelope_ = 1.0f;
}

void Limiter::process(float* data, int frames) {
  const float ceiling = db_to_linear(ceiling_db_.load());
  for (int i = 0; i < frames; ++i) {
    const float magnitude = std::fabs(data[i]);
    const float required = magnitude > ceiling ? ceiling / magnitude : 1.0f;

    if (required < envelope_) {
      envelope_ = required;  // 瞬时压制，保证不越过天花板
    } else {
      envelope_ += (required - envelope_) * release_coeff_;
    }
    data[i] *= envelope_;
  }
}

void Limiter::reset() { envelope_ = 1.0f; }

void Limiter::set_ceiling_db(float db) noexcept {
  ceiling_db_.store(std::clamp(db, -24.0f, 0.0f));
}

}  // namespace ms::dsp_core

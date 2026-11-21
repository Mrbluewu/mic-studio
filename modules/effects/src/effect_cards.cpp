#include "effects/effect_cards.h"

#include <algorithm>
#include <cmath>

namespace ms::effects {
namespace {
constexpr float kPi = 3.14159265358979323846f;
float db_to_linear(float db) { return std::pow(10.0f, db / 20.0f); }
}  // namespace

void Biquad::configure(Type type, double sample_rate, float frequency_hz, float gain_db, float q) {
  const float a = std::pow(10.0f, gain_db / 40.0f);
  const float w0 = 2.0f * kPi * std::clamp(frequency_hz, 20.0f, static_cast<float>(sample_rate * 0.45)) /
                   static_cast<float>(sample_rate);
  const float cos_w0 = std::cos(w0);
  const float sin_w0 = std::sin(w0);
  const float alpha = sin_w0 / (2.0f * std::max(q, 0.1f));

  double b0 = 1.0, b1 = 0.0, b2 = 0.0, a0 = 1.0, a1 = 0.0, a2 = 0.0;

  switch (type) {
    case Type::HighShelf: {
      const double two_sqrt_a_alpha = 2.0 * std::sqrt(a) * alpha;
      b0 = a * ((a + 1) + (a - 1) * cos_w0 + two_sqrt_a_alpha);
      b1 = -2 * a * ((a - 1) + (a + 1) * cos_w0);
      b2 = a * ((a + 1) + (a - 1) * cos_w0 - two_sqrt_a_alpha);
      a0 = (a + 1) - (a - 1) * cos_w0 + two_sqrt_a_alpha;
      a1 = 2 * ((a - 1) - (a + 1) * cos_w0);
      a2 = (a + 1) - (a - 1) * cos_w0 - two_sqrt_a_alpha;
      break;
    }
    case Type::LowShelf: {
      const double two_sqrt_a_alpha = 2.0 * std::sqrt(a) * alpha;
      b0 = a * ((a + 1) - (a - 1) * cos_w0 + two_sqrt_a_alpha);
      b1 = 2 * a * ((a - 1) - (a + 1) * cos_w0);
      b2 = a * ((a + 1) - (a - 1) * cos_w0 - two_sqrt_a_alpha);
      a0 = (a + 1) + (a - 1) * cos_w0 + two_sqrt_a_alpha;
      a1 = -2 * ((a - 1) + (a + 1) * cos_w0);
      a2 = (a + 1) + (a - 1) * cos_w0 - two_sqrt_a_alpha;
      break;
    }
    case Type::Peaking: {
      b0 = 1 + alpha * a;
      b1 = -2 * cos_w0;
      b2 = 1 - alpha * a;
      a0 = 1 + alpha / a;
      a1 = -2 * cos_w0;
      a2 = 1 - alpha / a;
      break;
    }
  }

  const double inv = 1.0 / a0;
  b0_ = static_cast<float>(b0 * inv);
  b1_ = static_cast<float>(b1 * inv);
  b2_ = static_cast<float>(b2 * inv);
  a1_ = static_cast<float>(a1 * inv);
  a2_ = static_cast<float>(a2 * inv);
  reset();
}

float Biquad::process(float x) noexcept {
  const float y = b0_ * x + z1_;
  z1_ = b1_ * x - a1_ * y + z2_;
  z2_ = b2_ * x - a2_ * y;
  return y;
}

void ClarityEffect::prepare(const dsp_core::ProcessContext& context) {
  shelf_.configure(Biquad::Type::HighShelf, context.sampleRate, 3000.0f, 4.0f, 0.7f);
}

void ClarityEffect::process(float* data, int frames) {
  if (!enabled_.load(std::memory_order_relaxed)) {
    return;
  }
  for (int i = 0; i < frames; ++i) {
    data[i] = shelf_.process(data[i]);
  }
}

void ClarityEffect::reset() { shelf_.reset(); }

void BodyEffect::prepare(const dsp_core::ProcessContext& context) {
  low_shelf_.configure(Biquad::Type::LowShelf, context.sampleRate, 160.0f, 3.0f, 0.7f);
  mud_cut_.configure(Biquad::Type::Peaking, context.sampleRate, 320.0f, -2.5f, 1.0f);
}

void BodyEffect::process(float* data, int frames) {
  if (!enabled_.load(std::memory_order_relaxed)) {
    return;
  }
  for (int i = 0; i < frames; ++i) {
    data[i] = mud_cut_.process(low_shelf_.process(data[i]));
  }
}

void BodyEffect::reset() {
  low_shelf_.reset();
  mud_cut_.reset();
}

void LevelerEffect::prepare(const dsp_core::ProcessContext& context) {
  // 100 ms 跟随、1 s 释放
  attack_coeff_ = static_cast<float>(1.0 - std::exp(-1.0 / (0.1 * context.sampleRate)));
  release_coeff_ = static_cast<float>(1.0 - std::exp(-1.0 / (1.0 * context.sampleRate)));
  gain_ = 1.0f;
}

void LevelerEffect::process(float* data, int frames) {
  if (!enabled_.load(std::memory_order_relaxed)) {
    return;
  }
  constexpr float kTarget = 0.1f;  // −20 dBFS
  for (int i = 0; i < frames; ++i) {
    const float magnitude = std::fabs(data[i]);
    const float desired = magnitude > 1e-5f ? std::clamp(kTarget / magnitude, 0.25f, 4.0f) : gain_;
    const float coeff = desired < gain_ ? attack_coeff_ : release_coeff_;
    gain_ += (desired - gain_) * coeff;
    data[i] *= gain_;
  }
}

void LevelerEffect::reset() { gain_ = 1.0f; }

void DeEsserEffect::prepare(const dsp_core::ProcessContext& context) {
  // 用 7 kHz 提升把齿音频段抽出来单独压限；主体 = 原信号 - 高频段。
  high_pass_.configure(Biquad::Type::Peaking, context.sampleRate, 7000.0f, 6.0f, 1.2f);
  attack_coeff_ = static_cast<float>(1.0 - std::exp(-1.0 / (0.002 * context.sampleRate)));
  release_coeff_ = static_cast<float>(1.0 - std::exp(-1.0 / (0.08 * context.sampleRate)));
  envelope_ = 0.0f;
}

void DeEsserEffect::process(float* data, int frames) {
  if (!enabled_.load(std::memory_order_relaxed)) {
    return;
  }
  constexpr float kThreshold = 0.08f;  // 齿音起始电平
  for (int i = 0; i < frames; ++i) {
    const float high = high_pass_.process(data[i]);
    const float magnitude = std::fabs(high);
    const float coeff = magnitude > envelope_ ? attack_coeff_ : release_coeff_;
    envelope_ += (magnitude - envelope_) * coeff;

    // 超出阈值时对高频段施加衰减，主体信号不受影响
    const float attenuation =
        envelope_ > kThreshold ? std::clamp(kThreshold / envelope_, 0.2f, 1.0f) : 1.0f;
    const float shaped_high = high * attenuation;
    const float body = data[i] - high;
    data[i] = body + shaped_high;
  }
}

void DeEsserEffect::reset() {
  high_pass_.reset();
  envelope_ = 0.0f;
}

}  // namespace ms::effects

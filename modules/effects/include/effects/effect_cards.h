#pragma once

#include "dsp_core/processor.h"

#include <atomic>

namespace ms::effects {

// RBJ 双二阶滤波器：搁架与峰值共用同一个实现。
class Biquad {
 public:
  enum class Type { HighShelf, LowShelf, Peaking };

  void configure(Type type, double sample_rate, float frequency_hz, float gain_db, float q);
  void reset() noexcept { z1_ = 0.0f; z2_ = 0.0f; }
  float process(float x) noexcept;

 private:
  float b0_ = 1.0f, b1_ = 0.0f, b2_ = 0.0f, a1_ = 0.0f, a2_ = 0.0f;
  float z1_ = 0.0f, z2_ = 0.0f;
};

// 效果卡：更清晰（3 kHz 以上抬升）
class ClarityEffect : public dsp_core::IProcessor {
 public:
  void prepare(const dsp_core::ProcessContext& context) override;
  void process(float* data, int frames) override;
  void reset() override;
  void set_enabled(bool enabled) noexcept { enabled_.store(enabled); }

 private:
  Biquad shelf_;
  std::atomic<bool> enabled_{false};
};

// 效果卡：更厚实（低频补一点，混浊处收一点）
class BodyEffect : public dsp_core::IProcessor {
 public:
  void prepare(const dsp_core::ProcessContext& context) override;
  void process(float* data, int frames) override;
  void reset() override;
  void set_enabled(bool enabled) noexcept { enabled_.store(enabled); }

 private:
  Biquad low_shelf_;
  Biquad mud_cut_;
  std::atomic<bool> enabled_{false};
};

// 效果卡：稳住音量（慢速 AGC，目标 −20 dBFS，±12 dB）
class LevelerEffect : public dsp_core::IProcessor {
 public:
  void prepare(const dsp_core::ProcessContext& context) override;
  void process(float* data, int frames) override;
  void reset() override;
  void set_enabled(bool enabled) noexcept { enabled_.store(enabled); }

 private:
  float gain_ = 1.0f;
  float attack_coeff_ = 0.0005f;
  float release_coeff_ = 0.00005f;
  std::atomic<bool> enabled_{false};
};

// 效果卡：去齿音（5 kHz 以上频段的动态压限）
class DeEsserEffect : public dsp_core::IProcessor {
 public:
  void prepare(const dsp_core::ProcessContext& context) override;
  void process(float* data, int frames) override;
  void reset() override;
  void set_enabled(bool enabled) noexcept { enabled_.store(enabled); }

 private:
  Biquad low_pass_;
  Biquad high_pass_;
  float envelope_ = 0.0f;
  float attack_coeff_ = 0.01f;
  float release_coeff_ = 0.0005f;
  std::atomic<bool> enabled_{false};
};

}  // namespace ms::effects

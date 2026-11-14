#pragma once

#include <cstdint>

namespace ms::dsp_core {

// 单声道线性重采样器（push 模式）。
// M1 用它打通链路；M2 接入 soxr 后仅保留为无第三方依赖的兜底实现。
// 注意：线性插值对降采样不做抗混叠，仅用于"格式不匹配时也能出声"。
class MonoResampler {
 public:
  void prepare(std::uint32_t input_rate, std::uint32_t output_rate) noexcept {
    step_ = (input_rate == 0 || output_rate == 0) ? 1.0
                                                  : static_cast<double>(input_rate) /
                                                        static_cast<double>(output_rate);
    reset();
  }

  void reset() noexcept {
    phase_ = 0.0;
    prev_ = 0.0f;
    has_prev_ = false;
  }

  // 漂移补偿：在原有比率上叠加 ppm 级修正。
  void set_ratio(double ratio) noexcept { step_ = ratio; }
  double ratio() const noexcept { return step_; }

  // 输入一个样本，输出 0..max_out 个样本，返回实际输出数。
  std::uint32_t push(float sample, float* out, std::uint32_t max_out) noexcept {
    if (!has_prev_) {
      prev_ = sample;
      has_prev_ = true;
      return 0;
    }
    std::uint32_t written = 0;
    while (phase_ < 1.0 && written < max_out) {
      out[written++] = prev_ + (sample - prev_) * static_cast<float>(phase_);
      phase_ += step_;
    }
    phase_ -= 1.0;
    prev_ = sample;
    return written;
  }

 private:
  double step_ = 1.0;
  double phase_ = 0.0;
  float prev_ = 0.0f;
  bool has_prev_ = false;
};

}  // namespace ms::dsp_core

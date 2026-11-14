#pragma once

#include "dsp_core/processor.h"

#include <atomic>

namespace ms::dsp_core {

// 增益：参数变化走 20 ms 线性斜坡，避免拉杆时出现 zipper noise。
class GainProcessor : public IProcessor {
 public:
  void prepare(const ProcessContext& context) override;
  void process(float* data, int frames) override;
  void reset() override;

  // 非实时线程调用，内部只写原子量
  void set_gain_db(float db) noexcept;

 private:
  std::atomic<float> target_gain_db_{0.0f};
  float current_gain_ = 1.0f;
  float ramp_coeff_ = 0.001f;
};

// 一阶高通：去掉直流与超低频隆隆声（默认 20 Hz）。
class DcBlocker : public IProcessor {
 public:
  void prepare(const ProcessContext& context) override;
  void process(float* data, int frames) override;
  void reset() override;

 private:
  float coefficient_ = 0.997f;
  float last_input_ = 0.0f;
  float last_output_ = 0.0f;
};

// 峰值限幅：瞬时压制、50 ms 释放，天花板固定 −1 dBFS。
class Limiter : public IProcessor {
 public:
  void prepare(const ProcessContext& context) override;
  void process(float* data, int frames) override;
  void reset() override;

  void set_ceiling_db(float db) noexcept;

 private:
  std::atomic<float> ceiling_db_{-1.0f};
  float envelope_ = 1.0f;
  float release_coeff_ = 0.001f;
};

}  // namespace ms::dsp_core

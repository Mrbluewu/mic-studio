#pragma once

#include "dsp_core/processor.h"

namespace ms::denoise {

// 降噪后端统一接口：链上的一级处理器，外加一个 0..1 的强度控制。
class Denoiser : public dsp_core::IProcessor {
 public:
  ~Denoiser() override = default;

  // 0 = 直通，1 = 全量降噪。干湿比决定强度。
  virtual void set_amount(float amount) noexcept = 0;
};

}  // namespace ms::denoise

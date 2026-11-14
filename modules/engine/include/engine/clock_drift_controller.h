#pragma once

#include <algorithm>
#include <cstddef>

namespace ms::engine {

// 以环形缓冲水位为误差的 PI 控制器。
// 麦克风时钟与虚拟声卡时钟不同源，长时间运行必然出现水位单向漂移；
// 这里输出 ppm 级的重采样比率修正，把水位钉在目标值附近。
class ClockDriftController {
 public:
  void prepare(double nominal_step, double target_fill_frames) noexcept {
    nominal_ = nominal_step > 0.0 ? nominal_step : 1.0;
    target_ = target_fill_frames > 1.0 ? target_fill_frames : 1.0;
    integral_ = 0.0;
    last_ppm_ = 0.0;
  }

  void reset() noexcept {
    integral_ = 0.0;
    last_ppm_ = 0.0;
  }

  double update(std::size_t fill_frames) noexcept {
    const double error = (static_cast<double>(fill_frames) - target_) / target_;
    integral_ = std::clamp(integral_ + error * 0.01, -1.0, 1.0);

    double trim = 0.05 * error + 0.02 * integral_;
    trim = std::clamp(trim, -2e-3, 2e-3);  // ±2000 ppm ≈ 3.5 音分，听不出音高变化
    last_ppm_ = trim * 1e6;

    return std::clamp(nominal_ * (1.0 + trim), nominal_ * 0.5, nominal_ * 2.0);
  }

  double last_ppm() const noexcept { return last_ppm_; }

 private:
  double nominal_ = 1.0;
  double target_ = 960.0;
  double integral_ = 0.0;
  double last_ppm_ = 0.0;
};

}  // namespace ms::engine

#pragma once

#include <algorithm>
#include <cstddef>

namespace ms::engine {

// 以环形缓冲水位为误差的 PI 控制器。
// 麦克风时钟与虚拟声卡时钟不同源，长时间运行必然出现水位单向漂移；
// 这里输出 ppm 级的重采样比率修正，把水位钉在目标值附近。
//
// 两点设计约束来自实测：
//  1) 水位只能按设备周期整步变化（48 kHz 下 10 ms = 480 帧），单个采样的
//     相对误差可达 ±50%，直接进 P 项会瞬间顶到钳制边界并反复抖动。
//     因此误差先做一阶低通，控制器带宽远低于采样频率。
//  2) 启动瞬态（预灌开闸瞬间）误差极大，若不加处理一次就能把积分推到饱和，
//     之后需要数百次回调才能解开，那段期间会持续以满量程比率抽干/灌满缓冲。
//     因此采用条件积分：输出已顶在钳制边界且误差仍朝同方向推时停止累积。
class ClockDriftController {
 public:
  void prepare(double nominal_step, double target_fill_frames) noexcept {
    nominal_ = nominal_step > 0.0 ? nominal_step : 1.0;
    target_ = target_fill_frames > 1.0 ? target_fill_frames : 1.0;
    reset();
  }

  void reset() noexcept {
    integral_ = 0.0;
    last_ppm_ = 0.0;
    filtered_error_ = 0.0;
    primed_ = false;
  }

  double update(std::size_t fill_frames) noexcept {
    const double raw = (static_cast<double>(fill_frames) - target_) / target_;
    // 首个采样直接作为初值，让控制器立刻有响应；之后做一阶低通平滑量化噪声。
    if (!primed_) {
      filtered_error_ = raw;
      primed_ = true;
    } else {
      filtered_error_ += kErrorAlpha * (raw - filtered_error_);
    }
    const double error = filtered_error_;

    double trim = kKp * error + kKi * integral_;
    const bool pushing_high = trim >= kTrimLimit && error > 0.0;
    const bool pushing_low = trim <= -kTrimLimit && error < 0.0;
    if (!pushing_high && !pushing_low) {
      integral_ = std::clamp(integral_ + error * kIntegralGain, -kIntegralLimit, kIntegralLimit);
      trim = kKp * error + kKi * integral_;
    }

    trim = std::clamp(trim, -kTrimLimit, kTrimLimit);  // ±2000 ppm ≈ 3.5 音分，听不出音高变化
    last_ppm_ = trim * 1e6;

    return std::clamp(nominal_ * (1.0 + trim), nominal_ * 0.5, nominal_ * 2.0);
  }

  double last_ppm() const noexcept { return last_ppm_; }

 private:
  // 低通系数 0.02 → 时间常数约 50 次回调（10 ms 周期下约 0.5 s）。
  static constexpr double kErrorAlpha = 0.02;
  static constexpr double kKp = 0.002;
  static constexpr double kKi = 0.005;
  static constexpr double kIntegralGain = 0.02;
  static constexpr double kTrimLimit = 2e-3;
  // 积分项单独最多贡献 ±500 ppm。稳态漂移通常只有百 ppm 量级，够用；
  // 同时保证即使积到边界，误差回正后也不会长期把比率钉在钳制值上。
  static constexpr double kIntegralLimit = 0.5 * 1e-3 / kKi;

  double nominal_ = 1.0;
  double target_ = 960.0;
  double integral_ = 0.0;
  double last_ppm_ = 0.0;
  double filtered_error_ = 0.0;
  bool primed_ = false;
};

}  // namespace ms::engine

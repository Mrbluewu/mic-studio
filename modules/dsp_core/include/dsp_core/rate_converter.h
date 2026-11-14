#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace ms::dsp_core {

// 带内部输入 FIFO 的线性重采样器。
//
// 与 push 式子（每输入一帧产出不定帧数）不同，这里由输出侧驱动：
// 调用方先 feed 任意多输入，再 pull 精确的帧数。未被消费的输入留在队列里，
// 因此长期不会"每块净丢几帧"——这正是之前水位持续下滑的根因。
class RateConverter {
 public:
  void prepare(double step) noexcept {
    step_ = step > 0.0 ? step : 1.0;
    reset();
  }

  void reset() noexcept {
    queue_.clear();
    position_ = 0.0;
  }

  // 输入采样率 / 输出采样率
  double step() const noexcept { return step_; }
  void set_step(double step) noexcept {
    if (step > 0.0) {
      step_ = step;
    }
  }

  std::size_t fifo_size() const noexcept { return queue_.size(); }

  void feed(const float* data, std::size_t frames) {
    if (frames == 0) {
      return;
    }
    queue_.insert(queue_.end(), data, data + frames);
    // 安全阀：正常不会触发，异常时丢弃最旧数据而不是无限增长
    constexpr std::size_t kMaxFifo = 65536;
    if (queue_.size() > kMaxFifo) {
      const std::size_t drop = queue_.size() - kMaxFifo;
      queue_.erase(queue_.begin(), queue_.begin() + static_cast<std::ptrdiff_t>(drop));
      position_ = std::max(0.0, position_ - static_cast<double>(drop));
    }
  }

  // 产出最多 frames 帧，返回实际产出数（不足时由调用方补静音）。
  std::size_t pull(float* out, std::size_t frames) {
    std::size_t produced = 0;
    while (produced < frames) {
      const auto index = static_cast<std::size_t>(position_);
      if (index + 1 >= queue_.size()) {
        break;  // 需要下一块输入才能插值
      }
      const double fraction = position_ - static_cast<double>(index);
      const float a = queue_[index];
      const float b = queue_[index + 1];
      out[produced++] = a + (b - a) * static_cast<float>(fraction);
      position_ += step_;
    }

    // 丢弃已消费的前缀，只保留小数位置
    const auto consumed = static_cast<std::size_t>(position_);
    if (consumed > 0) {
      queue_.erase(queue_.begin(), queue_.begin() + static_cast<std::ptrdiff_t>(consumed));
      position_ -= static_cast<double>(consumed);
    }
    return produced;
  }

 private:
  std::vector<float> queue_;
  double position_ = 0.0;
  double step_ = 1.0;
};

}  // namespace ms::dsp_core

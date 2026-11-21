#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ms::dsp_core {

// 带内部输入 FIFO 的线性重采样器。
//
// 与 push 式子（每输入一帧产出不定帧数）不同，这里由输出侧驱动：
// 调用方先 feed 任意多输入，再 pull 精确的帧数。未被消费的输入留在队列里，
// 因此长期不会"每块净丢几帧"——这正是之前水位持续下滑的根因。
//
// 实现为固定容量环形 FIFO：容量在构造时一次性分配（2 的幂 + 位掩码取模），
// feed/pull 全程不分配内存、不搬移数据，可以安全地在实时音频线程调用。
// 超出容量时丢弃最旧输入（安全阀，正常不会触发）。
class RateConverter {
 public:
  RateConverter() : buffer_(kCapacityFrames, 0.0f) {}

  void prepare(double step) noexcept {
    step_ = step > 0.0 ? step : 1.0;
    reset();
  }

  void reset() noexcept {
    write_pos_ = 0;
    read_pos_ = 0;
    position_ = 0.0;
  }

  // 输入采样率 / 输出采样率
  double step() const noexcept { return step_; }
  void set_step(double step) noexcept {
    if (step > 0.0) {
      step_ = step;
    }
  }

  // 未消费的输入帧数（playback 水位，用于判欠载与计算需要补喂多少）
  std::size_t fifo_size() const noexcept {
    return static_cast<std::size_t>(write_pos_ - read_pos_);
  }

  void feed(const float* data, std::size_t frames) noexcept {
    if (frames == 0) {
      return;
    }
    // 单次喂入超过整个容量：只保留末尾 kCapacityFrames 帧
    if (frames > kCapacityFrames) {
      drop_oldest(fifo_size());
      data += frames - kCapacityFrames;
      frames = kCapacityFrames;
    }
    // 空间不足时先丢最旧的，保证写入不越界
    const std::size_t used = fifo_size();
    if (used + frames > kCapacityFrames) {
      drop_oldest(used + frames - kCapacityFrames);
    }

    const std::size_t tail = static_cast<std::size_t>(write_pos_ & mask_);
    const std::size_t first = std::min(frames, kCapacityFrames - tail);
    std::copy(data, data + first, buffer_.begin() + static_cast<std::ptrdiff_t>(tail));
    std::copy(data + first, data + frames, buffer_.begin());
    write_pos_ += frames;
  }

  // 产出最多 frames 帧，返回实际产出数（不足时由调用方补静音）。
  std::size_t pull(float* out, std::size_t frames) noexcept {
    std::size_t produced = 0;
    const std::size_t available = fifo_size();
    while (produced < frames) {
      const auto index = static_cast<std::size_t>(position_);
      if (index + 1 >= available) {
        break;  // 需要下一块输入才能插值
      }
      const double fraction = position_ - static_cast<double>(index);
      const std::size_t base = static_cast<std::size_t>((read_pos_ + index) & mask_);
      const std::size_t next = (base + 1) & mask_;
      const float a = buffer_[base];
      const float b = buffer_[next];
      out[produced++] = a + (b - a) * static_cast<float>(fraction);
      position_ += step_;
    }

    // 丢弃已消费的前缀，只保留小数位置
    const auto consumed = static_cast<std::size_t>(position_);
    if (consumed > 0) {
      read_pos_ += consumed;
      position_ -= static_cast<double>(consumed);
    }
    return produced;
  }

 private:
  static constexpr std::size_t kCapacityFrames = 65536;  // 2 的幂
  static constexpr std::size_t mask_ = kCapacityFrames - 1;

  void drop_oldest(std::size_t frames) noexcept {
    const std::size_t count = std::min(frames, fifo_size());
    read_pos_ += count;
    position_ = std::max(0.0, position_ - static_cast<double>(count));
  }

  std::vector<float> buffer_;
  std::uint64_t write_pos_ = 0;  // 累计写入帧数
  std::uint64_t read_pos_ = 0;   // 累计丢弃帧数
  double position_ = 0.0;        // 相对 read_pos_ 的读取位置（含小数）
  double step_ = 1.0;
};

}  // namespace ms::dsp_core

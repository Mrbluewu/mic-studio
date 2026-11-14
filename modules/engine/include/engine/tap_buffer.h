#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ms::engine {

// 只写环形缓存：实时线程覆盖写入，非实时线程按"最近 N 帧"读取。
// A/B 对比与体检都消费它，实时路径不会因为有人读取而阻塞。
class TapBuffer {
 public:
  explicit TapBuffer(std::size_t capacity_frames)
      : buffer_(capacity_frames > 0 ? capacity_frames : 1, 0.0f) {}

  void write(const float* data, std::size_t frames) noexcept {
    if (data == nullptr || frames == 0) {
      return;
    }
    const std::size_t capacity = buffer_.size();
    std::uint64_t position = position_;
    for (std::size_t i = 0; i < frames; ++i) {
      buffer_[(position + i) % capacity] = data[i];
    }
    position_ = position + frames;
    written_.store(position_, std::memory_order_release);
  }

  // 读取最近 frames 帧（按时间顺序）。数据不足时前面补零。
  void read_latest(float* dst, std::size_t frames) const {
    const std::size_t capacity = buffer_.size();
    const std::uint64_t end = written_.load(std::memory_order_acquire);
    const std::size_t available =
        static_cast<std::size_t>(std::min<std::uint64_t>(end, capacity));
    const std::uint64_t start = end - available;
    const std::size_t offset = frames > available ? frames - available : 0;

    std::fill(dst, dst + offset, 0.0f);
    for (std::size_t i = 0; i < available && offset + i < frames; ++i) {
      dst[offset + i] = buffer_[(start + i) % capacity];
    }
  }

  std::size_t capacity() const noexcept { return buffer_.size(); }

 private:
  std::vector<float> buffer_;
  std::uint64_t position_ = 0;
  std::atomic<std::uint64_t> written_{0};
};

}  // namespace ms::engine

#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace ms::engine {

// 只写环形缓存：实时线程覆盖写入，非实时线程按"最近 N 帧"读取。
// A/B 对比与体检都消费它，实时路径不会因为有人读取而阻塞。
class TapBuffer {
 public:
  explicit TapBuffer(std::size_t capacity_frames)
      : capacity_(capacity_frames > 0 ? capacity_frames : 1),
        buffer_(std::make_unique<std::atomic<std::uint32_t>[]>(capacity_)) {
    for (std::size_t i = 0; i < capacity_; ++i) {
      buffer_[i].store(0, std::memory_order_relaxed);
    }
  }

  void write(const float* data, std::size_t frames) noexcept {
    if (data == nullptr || frames == 0) {
      return;
    }
    const std::uint64_t position = position_;
    for (std::size_t i = 0; i < frames; ++i) {
      buffer_[(position + i) % capacity_].store(to_bits(data[i]), std::memory_order_relaxed);
    }
    position_ = position + frames;
    written_.store(position_, std::memory_order_release);
  }

  // Returns false if a concurrent write changed the requested snapshot.
  bool read_latest(float* dst, std::size_t frames) const noexcept {
    if (dst == nullptr && frames != 0) {
      return false;
    }
    if (frames == 0) {
      return true;
    }
    const std::uint64_t end = written_.load(std::memory_order_acquire);
    const std::size_t available =
        static_cast<std::size_t>(std::min<std::uint64_t>(end, capacity_));
    // 只截取"最近 frames 帧"：可用样本多于 frames 时必须从尾部对齐，否则会读到旧窗口。
    const std::size_t take = std::min(frames, available);
    const std::uint64_t start = end - take;
    const std::size_t offset = frames - take;

    std::fill(dst, dst + offset, 0.0f);
    for (std::size_t i = 0; i < take; ++i) {
      dst[offset + i] = from_bits(buffer_[(start + i) % capacity_].load(
          std::memory_order_relaxed));
    }
    if (written_.load(std::memory_order_acquire) != end) {
      return false;
    }
    return true;
  }

  std::size_t capacity() const noexcept { return capacity_; }

 private:
  static std::uint32_t to_bits(float value) noexcept {
    return std::bit_cast<std::uint32_t>(value);
  }

  static float from_bits(std::uint32_t value) noexcept {
    return std::bit_cast<float>(value);
  }

  const std::size_t capacity_;
  std::unique_ptr<std::atomic<std::uint32_t>[]> buffer_;
  std::uint64_t position_ = 0;
  std::atomic<std::uint64_t> written_{0};
};

}  // namespace ms::engine

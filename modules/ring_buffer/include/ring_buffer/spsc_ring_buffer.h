#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <vector>

namespace ms::ring_buffer {

#if defined(_MSC_VER)
// C4324「由于对齐说明符，结构被填充」：这里的填充正是本意——读写游标各占一条
// 缓存行，避免生产者与消费者互相把对方的缓存行拉失效。定点抑制而非全局关闭。
#pragma warning(push)
#pragma warning(disable : 4324)
#endif

// 单生产者 / 单消费者无锁环形缓冲，元素为 float。
// 写入端是采集线程，读取端是渲染线程；两端都不允许阻塞。
class SpscRingBuffer {
 public:
  explicit SpscRingBuffer(std::size_t capacity_frames) {
    std::size_t capacity = 64;
    while (capacity < capacity_frames) {
      capacity <<= 1;  // 容量取 2 的幂，用位掩码取模
    }
    buffer_.assign(capacity, 0.0f);
    mask_ = capacity - 1;
  }

  SpscRingBuffer(const SpscRingBuffer&) = delete;
  SpscRingBuffer& operator=(const SpscRingBuffer&) = delete;

  void reset() noexcept {
    write_pos_.store(0, std::memory_order_relaxed);
    read_pos_.store(0, std::memory_order_relaxed);
  }

  std::size_t capacity() const noexcept { return buffer_.size(); }

  std::size_t available_read() const noexcept {
    const std::uint64_t write = write_pos_.load(std::memory_order_acquire);
    const std::uint64_t read = read_pos_.load(std::memory_order_relaxed);
    return static_cast<std::size_t>(write - read);
  }

  std::size_t available_write() const noexcept {
    return capacity() - available_read();
  }

  // 返回实际写入帧数；缓冲区满时小于 frames，调用方需自行决定丢弃策略。
  std::size_t write(const float* src, std::size_t frames) noexcept {
    const std::uint64_t write = write_pos_.load(std::memory_order_relaxed);
    const std::uint64_t read = read_pos_.load(std::memory_order_acquire);
    const std::size_t used = static_cast<std::size_t>(write - read);
    const std::size_t count = std::min(frames, capacity() - used);

    for (std::size_t i = 0; i < count; ++i) {
      buffer_[static_cast<std::size_t>((write + i) & mask_)] = src[i];
    }
    write_pos_.store(write + count, std::memory_order_release);
    return count;
  }

  // 返回实际读取帧数；不足时剩余部分由调用方补静音。
  std::size_t read(float* dst, std::size_t frames) noexcept {
    const std::uint64_t read = read_pos_.load(std::memory_order_relaxed);
    const std::uint64_t write = write_pos_.load(std::memory_order_acquire);
    const std::size_t available = static_cast<std::size_t>(write - read);
    const std::size_t count = std::min(frames, available);

    for (std::size_t i = 0; i < count; ++i) {
      dst[i] = buffer_[static_cast<std::size_t>((read + i) & mask_)];
    }
    read_pos_.store(read + count, std::memory_order_release);
    return count;
  }

  std::uint64_t read_index() const noexcept {
    return read_pos_.load(std::memory_order_relaxed);
  }
  std::uint64_t write_index() const noexcept {
    return write_pos_.load(std::memory_order_relaxed);
  }

 private:
  std::vector<float> buffer_;
  std::size_t mask_ = 0;

  alignas(64) std::atomic<std::uint64_t> write_pos_{0};
  alignas(64) std::atomic<std::uint64_t> read_pos_{0};
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

}  // namespace ms::ring_buffer

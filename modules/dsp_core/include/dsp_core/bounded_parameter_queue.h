#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace ms::dsp_core {

// 定容量、无锁、无运行期分配的多生产者 / 单消费者队列。
// 用于插件参数事件：实时线程只出队，插件回调任意线程入队。
// 队列满时 try_push 立即返回 false（丢弃新事件），绝不等待或扩容；
// try_push / try_pop 的循环次数均为固定上界，不含数据相关的长时间自旋。
template <typename T, std::size_t Capacity>
#if defined(_MSC_VER)
// C4324：alignas(64) 是有意为入/出队游标做缓存行隔离，产生的填充属预期。
#pragma warning(push)
#pragma warning(disable : 4324)
#endif

class BoundedParameterQueue {
  static_assert(Capacity > 1);
  static_assert(std::has_single_bit(Capacity));
  static_assert(std::is_trivially_copyable_v<T>);
  static_assert(sizeof(T) <= sizeof(std::uint64_t));

 public:
  BoundedParameterQueue() noexcept {
    for (std::size_t i = 0; i < Capacity; ++i) {
      cells_[i].sequence.store(i, std::memory_order_relaxed);
    }
  }

  bool try_push(const T& value) noexcept {
    std::uint64_t position = enqueue_.load(std::memory_order_relaxed);
    for (int attempt = 0; attempt < 8; ++attempt) {
      Cell& cell = cells_[position & (Capacity - 1)];
      const std::uint64_t sequence = cell.sequence.load(std::memory_order_acquire);
      if (sequence == position) {
        if (enqueue_.compare_exchange_weak(position, position + 1,
                                           std::memory_order_relaxed)) {
          cell.value = value;
          cell.sequence.store(position + 1, std::memory_order_release);
          return true;
        }
        continue;
      }
      // 序号落后于目标位置说明槽位还没被消费者释放，即队列已满。
      if (static_cast<std::int64_t>(sequence - position) < 0) {
        return false;
      }
      position = enqueue_.load(std::memory_order_relaxed);
    }
    return false;
  }

  bool try_pop(T& value) noexcept {
    const std::uint64_t position = dequeue_.load(std::memory_order_relaxed);
    Cell& cell = cells_[position & (Capacity - 1)];
    const std::uint64_t sequence = cell.sequence.load(std::memory_order_acquire);
    if (sequence != position + 1) {
      return false;
    }
    value = cell.value;
    cell.sequence.store(position + Capacity, std::memory_order_release);
    dequeue_.store(position + 1, std::memory_order_relaxed);
    return true;
  }

  static constexpr std::size_t capacity() noexcept { return Capacity; }

 private:
  struct Cell {
    std::atomic<std::uint64_t> sequence{0};
    T value{};
  };

  // 序号初始化在构造函数完成：第 i 个槽的序号为 i，表示"等待写入位置 i"。
  std::array<Cell, Capacity> cells_{};
  alignas(64) std::atomic<std::uint64_t> enqueue_{0};
  alignas(64) std::atomic<std::uint64_t> dequeue_{0};
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

}  // namespace ms::dsp_core

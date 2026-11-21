#include "dsp_core/bounded_parameter_queue.h"
#include "engine/tap_buffer.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace {

// 不用 assert：Release 下 NDEBUG 会让断言整体消失，测试会变成空跑。
#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #condition, __FILE__, \
                   __LINE__);                                                  \
      std::abort();                                                            \
    }                                                                          \
  } while (false)

void test_tap_buffer() {
  ms::engine::TapBuffer tap(4);
  std::array<float, 6> output{};
  CHECK(tap.read_latest(output.data(), output.size()));
  for (float value : output) {
    CHECK(value == 0.0f);
  }

  // 数据不足时前补零。
  const std::array<float, 2> first{1.0f, 2.0f};
  tap.write(first.data(), first.size());
  CHECK(tap.read_latest(output.data(), 4));
  CHECK(output[0] == 0.0f && output[1] == 0.0f && output[2] == 1.0f && output[3] == 2.0f);

  // 写入量超过容量后只保留最近 capacity 帧，并正确跨环。
  const std::array<float, 5> wrapped{3.0f, 4.0f, 5.0f, 6.0f, 7.0f};
  tap.write(wrapped.data(), wrapped.size());
  std::array<float, 4> latest{};
  CHECK(tap.read_latest(latest.data(), latest.size()));
  CHECK(latest[0] == 4.0f && latest[1] == 5.0f && latest[2] == 6.0f && latest[3] == 7.0f);

  std::array<float, 2> tail{};
  CHECK(tap.read_latest(tail.data(), tail.size()));
  CHECK(tail[0] == 6.0f && tail[1] == 7.0f);
}

void test_tap_concurrent() {
  constexpr std::uint32_t kBlocks = 200000;
  ms::engine::TapBuffer tap(256);
  std::atomic<bool> done{false};
  std::thread writer([&] {
    std::array<float, 32> block{};
    for (std::uint32_t sequence = 1; sequence <= kBlocks; ++sequence) {
      block.fill(static_cast<float>(sequence));
      tap.write(block.data(), block.size());
    }
    done.store(true, std::memory_order_release);
  });

  // 并发期只看原子性：每次读到的样本必须是完整写入的整数值，
  // 不能出现被撕裂的浮点位模式。
  std::array<float, 64> output{};
  while (!done.load(std::memory_order_acquire)) {
    if (tap.read_latest(output.data(), output.size())) {
      for (float value : output) {
        CHECK(value >= 0.0f && value <= static_cast<float>(kBlocks));
        CHECK(std::floor(value) == value);
      }
    }
  }
  writer.join();

  // 写线程结束后快照必须完全稳定：最近一块就是最后写入的值。
  std::array<float, 32> final_block{};
  CHECK(tap.read_latest(final_block.data(), final_block.size()));
  for (float value : final_block) {
    CHECK(value == static_cast<float>(kBlocks));
  }
}

struct Change {
  std::uint32_t id = 0;
  std::uint32_t value = 0;
};

void test_parameter_queue() {
  ms::dsp_core::BoundedParameterQueue<Change, 2> queue;
  CHECK(queue.try_push({1, 10}));
  CHECK(queue.try_push({2, 20}));
  CHECK(!queue.try_push({3, 30}));  // 满：丢弃新事件而不是等待

  Change change;
  CHECK(queue.try_pop(change) && change.id == 1 && change.value == 10);
  CHECK(queue.try_push({3, 30}));
  CHECK(queue.try_pop(change) && change.id == 2);
  CHECK(queue.try_pop(change) && change.id == 3);
  CHECK(!queue.try_pop(change));
}

void test_mpsc_parameter_queue() {
  constexpr std::uint32_t kProducers = 4;
  constexpr std::uint32_t kItems = 20000;
  ms::dsp_core::BoundedParameterQueue<Change, 1024> queue;
  std::atomic<std::uint32_t> completed{0};
  std::atomic<std::uint64_t> received{0};
  std::vector<std::thread> producers;
  for (std::uint32_t producer = 0; producer < kProducers; ++producer) {
    producers.emplace_back([&, producer] {
      for (std::uint32_t i = 0; i < kItems; ++i) {
        const Change change{producer, i};
        while (!queue.try_push(change)) {
          std::this_thread::yield();
        }
      }
      completed.fetch_add(1, std::memory_order_release);
    });
  }

  std::thread consumer([&] {
    Change item;
    while (completed.load(std::memory_order_acquire) != kProducers) {
      if (queue.try_pop(item)) {
        received.fetch_add(1, std::memory_order_relaxed);
      }
    }
    while (queue.try_pop(item)) {
      received.fetch_add(1, std::memory_order_relaxed);
    }
  });

  for (auto& producer : producers) {
    producer.join();
  }
  consumer.join();
  CHECK(received.load(std::memory_order_relaxed) ==
        static_cast<std::uint64_t>(kProducers) * kItems);
}

}  // namespace

int main() {
  test_tap_buffer();
  test_tap_concurrent();
  test_parameter_queue();
  test_mpsc_parameter_queue();
  std::printf("all concurrency tests passed\n");
  return 0;
}

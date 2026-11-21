// dsp_core / ring_buffer / clock_drift 的实时组件单元测试。
// 这些类型运行在音频回调线程上，行为契约必须可回归：
//  - SpscRingBuffer：容量取 2 的幂、满/空边界、回绕后数据完整
//  - RateConverter：step 语义、长期净流量、FIFO 上限、reset
//  - ClockDriftController：ppm 符号、钳制范围、输出与 ppm 的一致性
// 测试不依赖任何音频设备，可在无交互会话中运行。

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <vector>

#include "dsp_core/rate_converter.h"
#include "engine/clock_drift_controller.h"
#include "ring_buffer/spsc_ring_buffer.h"
#include "support/check.h"

namespace {

bool near(double a, double b, double tolerance = 1e-9) {
  return std::fabs(a - b) <= tolerance;
}

void test_ring_buffer_basics() {
  begin("SpscRingBuffer 基本读写");

  ms::ring_buffer::SpscRingBuffer ring(100);
  CHECK(ring.capacity() >= 100);
  CHECK((ring.capacity() & (ring.capacity() - 1)) == 0);
  CHECK(ring.available_read() == 0);
  CHECK(ring.available_write() == ring.capacity());

  float input[10];
  for (int i = 0; i < 10; ++i) {
    input[i] = static_cast<float>(i) + 0.5f;
  }
  CHECK(ring.write(input, 10) == 10);
  CHECK(ring.available_read() == 10);
  CHECK(ring.available_write() == ring.capacity() - 10);

  float output[16] = {};
  CHECK(ring.read(output, 4) == 4);
  for (int i = 0; i < 4; ++i) {
    CHECK(output[i] == input[i]);
  }
  CHECK(ring.available_read() == 6);

  CHECK(ring.read(output, 4) == 4);
  CHECK(output[0] == input[4]);
  CHECK(output[3] == input[7]);
  CHECK(ring.available_read() == 2);

  ring.reset();
  CHECK(ring.available_read() == 0);
  CHECK(ring.write_index() == 0);
  CHECK(ring.read_index() == 0);
}

void test_ring_buffer_bounds() {
  begin("SpscRingBuffer 满/空边界");

  ms::ring_buffer::SpscRingBuffer ring(64);
  const std::size_t capacity = ring.capacity();
  std::vector<float> big(capacity * 2, 1.0f);

  // 写入超过剩余空间时按容量截断，返回值即实际写入帧数
  CHECK(ring.write(big.data(), capacity + 17) == capacity);
  CHECK(ring.available_read() == capacity);
  CHECK(ring.available_write() == 0);
  CHECK(ring.write(big.data(), 8) == 0);

  // 读取超过水位时只返回已有帧数，且不越界写
  std::vector<float> output(capacity + 32, -1.0f);
  CHECK(ring.read(output.data(), capacity + 32) == capacity);
  CHECK(output[capacity - 1] == 1.0f);
  CHECK(output[capacity] == -1.0f);
  CHECK(ring.read(output.data(), 4) == 0);
  CHECK(ring.available_read() == 0);
}

void test_ring_buffer_wraparound() {
  begin("SpscRingBuffer 回绕数据完整性");

  ms::ring_buffer::SpscRingBuffer ring(64);
  std::vector<float> chunk(37);
  std::vector<float> output(37);
  float next_value = 0.0f;
  std::size_t verified = 0;
  bool intact = true;

  // 每轮写入 37 帧再读空，多轮后必然跨越容量边界回绕
  for (int round = 0; round < 50 && intact; ++round) {
    for (auto& sample : chunk) {
      sample = next_value;
      next_value += 1.0f;
    }
    if (ring.write(chunk.data(), chunk.size()) != chunk.size()) {
      intact = false;
      break;
    }
    if (ring.read(output.data(), output.size()) != output.size()) {
      intact = false;
      break;
    }
    for (std::size_t i = 0; i < chunk.size(); ++i) {
      if (output[i] != chunk[i]) {
        intact = false;
        break;
      }
    }
    verified += chunk.size();
  }

  CHECK(intact);
  CHECK(verified == 50 * 37);
}

void test_converter_unit_step() {
  begin("RateConverter step=1 直通与插值");

  ms::dsp_core::RateConverter converter;
  converter.prepare(1.0);
  CHECK(near(converter.step(), 1.0, 1e-12));
  CHECK(converter.fifo_size() == 0);

  const float input[] = {0.0f, 1.0f, 2.0f, 3.0f, 4.0f};
  converter.feed(input, 5);
  CHECK(converter.fifo_size() == 5);

  float output[8] = {};
  CHECK(converter.pull(output, 4) == 4);
  CHECK(output[0] == 0.0f);
  CHECK(output[1] == 1.0f);
  CHECK(output[2] == 2.0f);
  CHECK(output[3] == 3.0f);
  // 线性插值需要下一帧作为前视，因此末帧留在 FIFO
  CHECK(converter.fifo_size() == 1);

  converter.feed(input, 0);
  CHECK(converter.fifo_size() == 1);
}

void test_converter_step_variants() {
  begin("RateConverter step=0.5 / 2.0");

  // step=0.5：每个输出只前进半个输入帧，落在相邻两帧中点上
  ms::dsp_core::RateConverter slow;
  slow.prepare(0.5);
  const float input[] = {0.0f, 2.0f, 4.0f, 6.0f, 8.0f};
  slow.feed(input, 5);
  float slow_out[8] = {};
  CHECK(slow.pull(slow_out, 4) == 4);
  CHECK(near(slow_out[0], 0.0, 1e-6));
  CHECK(near(slow_out[1], 1.0, 1e-6));
  CHECK(near(slow_out[2], 2.0, 1e-6));
  CHECK(near(slow_out[3], 3.0, 1e-6));

  // step=2.0：每次输出跨过两帧
  ms::dsp_core::RateConverter fast;
  fast.prepare(2.0);
  const float ramp[] = {0.0f, 2.0f, 4.0f, 6.0f, 8.0f, 10.0f, 12.0f, 14.0f, 16.0f};
  fast.feed(ramp, 9);
  float fast_out[8] = {};
  CHECK(fast.pull(fast_out, 3) == 3);
  CHECK(near(fast_out[0], 0.0, 1e-6));
  CHECK(near(fast_out[1], 4.0, 1e-6));
  CHECK(near(fast_out[2], 8.0, 1e-6));
}

void test_converter_net_flow() {
  begin("RateConverter 长期净流量");

  ms::dsp_core::RateConverter converter;
  converter.prepare(1.0);

  // 模拟渲染回调：每块喂 480 帧、拉 480 帧，连续 200 块
  std::vector<float> block(480);
  std::vector<float> output(480);
  std::size_t fed = 0;
  std::size_t produced = 0;

  for (int i = 0; i < 200; ++i) {
    for (std::size_t j = 0; j < block.size(); ++j) {
      block[j] = static_cast<float>((fed + j) % 1000);
    }
    converter.feed(block.data(), block.size());
    fed += block.size();
    produced += converter.pull(output.data(), output.size());
  }

  // 每块最多欠 1 帧（插值前视），长期不允许逐块累积丢帧
  CHECK(fed >= produced);
  CHECK(fed - produced <= 2);
  CHECK(converter.fifo_size() <= 8);
}

void test_converter_fifo_cap_and_reset() {
  begin("RateConverter FIFO 上限与 reset");

  ms::dsp_core::RateConverter converter;
  converter.prepare(1.0);

  // 持续灌入远超上限的数据，FIFO 不得无限增长
  std::vector<float> huge(200000, 0.25f);
  for (std::size_t offset = 0; offset < huge.size(); offset += 4096) {
    const std::size_t count = std::min<std::size_t>(4096, huge.size() - offset);
    converter.feed(huge.data() + offset, count);
    CHECK(converter.fifo_size() <= 65536);
  }
  CHECK(converter.fifo_size() > 0);

  float output[64] = {};
  CHECK(converter.pull(output, 64) == 64);
  CHECK(near(output[63], 0.25, 1e-6));

  converter.reset();
  CHECK(converter.fifo_size() == 0);
  CHECK(converter.pull(output, 64) == 0);
}

void test_converter_step_guard() {
  begin("RateConverter 非法 step 处理");

  ms::dsp_core::RateConverter converter;
  converter.prepare(0.0);
  CHECK(near(converter.step(), 1.0, 1e-12));

  converter.prepare(-2.0);
  CHECK(near(converter.step(), 1.0, 1e-12));

  converter.prepare(1.25);
  converter.set_step(0.0);
  CHECK(near(converter.step(), 1.25, 1e-12));

  converter.set_step(-1.0);
  CHECK(near(converter.step(), 1.25, 1e-12));

  converter.set_step(0.999);
  CHECK(near(converter.step(), 0.999, 1e-12));
}

void test_drift_controller_sign() {
  begin("ClockDriftController 水位误差符号");

  ms::engine::ClockDriftController drift;
  drift.prepare(1.0, 1000.0);
  CHECK(near(drift.last_ppm(), 0.0, 1e-12));

  // 水位正好等于目标：不修正
  CHECK(near(drift.update(1000), 1.0, 1e-12));
  CHECK(near(drift.last_ppm(), 0.0, 1e-9));

  // 水位偏高：加快消费（比率 > 1，ppm > 0）
  drift.reset();
  const double high = drift.update(1100);
  CHECK(drift.last_ppm() > 0.0);
  CHECK(high > 1.0);
  CHECK(near(high, 1.0 + drift.last_ppm() * 1e-6, 1e-12));

  // 水位偏低：放慢消费（比率 < 1，ppm < 0）
  drift.reset();
  const double low = drift.update(900);
  CHECK(drift.last_ppm() < 0.0);
  CHECK(low < 1.0);
  CHECK(near(low, 1.0 + drift.last_ppm() * 1e-6, 1e-12));
}

void test_drift_controller_clamp() {
  begin("ClockDriftController 钳制与参数保护");

  ms::engine::ClockDriftController drift;
  drift.prepare(1.0, 96000.0);

  // 极端水位：修正量必须钳在 ±2000 ppm
  for (int i = 0; i < 500; ++i) {
    drift.update(0);
  }
  CHECK(near(drift.last_ppm(), -2000.0, 1e-6));

  drift.reset();
  for (int i = 0; i < 500; ++i) {
    drift.update(4000000);
  }
  CHECK(near(drift.last_ppm(), 2000.0, 1e-6));
  // 比率也要留在 [0.5 * nominal, 2.0 * nominal] 内
  const double step = drift.update(4000000);
  CHECK(step >= 0.5);
  CHECK(step <= 2.0);
  CHECK(near(step, 1.0 + drift.last_ppm() * 1e-6, 1e-12));

  // 非法 nominal / target 退化为安全默认值
  ms::engine::ClockDriftController guarded;
  guarded.prepare(0.0, 0.0);
  CHECK(near(guarded.update(1), 1.0, 1e-12));
  CHECK(near(guarded.last_ppm(), 0.0, 1e-9));
}

void test_drift_controller_anti_windup() {
  begin("ClockDriftController 积分抗饱和");

  // 复现启动瞬态：开闸瞬间水位远超目标（预灌造成），随后水位回到目标。
  ms::engine::ClockDriftController drift;
  drift.prepare(1.0, 500.0);

  drift.update(6000);  // error = 11，旧实现单次即让积分饱和
  CHECK(drift.last_ppm() == 2000.0);

  // 之后水位稳定在目标值：修正量必须退出满量程并收敛到小量级，
  // 不允许在 +2000 ppm 上钉住（旧实现要数百次回调才解开）。
  for (int i = 0; i < 400; ++i) {
    drift.update(500);
  }
  CHECK(std::fabs(drift.last_ppm()) < 600.0);

  // 反方向同样成立
  drift.reset();
  drift.update(0);
  CHECK(drift.last_ppm() == -2000.0);
  for (int i = 0; i < 400; ++i) {
    drift.update(500);
  }
  CHECK(std::fabs(drift.last_ppm()) < 600.0);

  // 长时间大误差累积后积分不得无限增长
  drift.reset();
  for (int i = 0; i < 5000; ++i) {
    drift.update(5000);
  }
  CHECK(drift.last_ppm() == 2000.0);
  for (int i = 0; i < 400; ++i) {
    drift.update(500);
  }
  CHECK(std::fabs(drift.last_ppm()) < 600.0);
}

void test_drift_controller_closed_loop() {
  begin("ClockDriftController 闭环稳定");

  // 模拟引擎真实回环：每块喂 480 帧、按修正比率消费 480*step 帧。
  // 水位初始偏离目标，控制器必须把它拉回并保持住，不出现持续下滑或爆表。
  constexpr double kTarget = 960.0;
  constexpr double kBlock = 480.0;

  struct Scenario {
    const char* name;
    double initial_fill;
    double drift_ppm;
  };
  const Scenario scenarios[] = {
      {"初始偏低", kTarget * 0.4, 0.0},
      {"初始偏高", kTarget * 2.5, 0.0},
      {"时钟偏快", kTarget, 300.0},
      {"时钟偏慢", kTarget, -300.0},
  };

  for (const auto& scenario : scenarios) {
    ms::engine::ClockDriftController drift;
    drift.prepare(1.0, kTarget);
    double fill = scenario.initial_fill;
    double min_fill = fill;
    double max_fill = fill;
    double max_abs_ppm = 0.0;

    for (int i = 0; i < 3000; ++i) {  // 约 30 秒
      const double step = drift.update(static_cast<std::size_t>(fill));
      // 生产者按真实时钟产帧，消费者按控制器比率取帧
      const double produced = kBlock * (1.0 + scenario.drift_ppm * 1e-6);
      fill += produced - kBlock * step;
      fill = std::clamp(fill, 0.0, kTarget * 8.0);
      min_fill = std::min(min_fill, fill);
      max_fill = std::max(max_fill, fill);
      max_abs_ppm = std::max(max_abs_ppm, std::fabs(drift.last_ppm()));
    }

    std::printf("   %s：水位 %.0f..%.0f 帧，峰值修正 %.0f ppm\n", scenario.name, min_fill,
                max_fill, max_abs_ppm);
    CHECK(max_abs_ppm <= 2000.0);
    // 全程不得出现抽干
    CHECK(min_fill > 0.0);
    // 收敛后水位应回到目标附近（容差覆盖 480 帧的量化步进）
    CHECK(fill > kTarget * 0.25);
    CHECK(fill < kTarget * 3.0);
  }
}

}  // namespace

int main() {
  std::printf("Mic Studio DSP core tests\n");

  test_ring_buffer_basics();
  test_ring_buffer_bounds();
  test_ring_buffer_wraparound();
  test_converter_unit_step();
  test_converter_step_variants();
  test_converter_net_flow();
  test_converter_fifo_cap_and_reset();
  test_converter_step_guard();
  test_drift_controller_sign();
  test_drift_controller_clamp();
  test_drift_controller_anti_windup();
  test_drift_controller_closed_loop();

  return report("micstudio_dsp_core_tests");
}

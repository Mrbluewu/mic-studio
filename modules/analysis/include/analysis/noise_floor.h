#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace ms::analysis {

// 噪声底估计：用块 RMS 直方图取分位数。
// P10 视为噪声底，P90 视为语音电平；两者之差就是"降噪带来的信噪比提升"。
// 只在实时线程写入，对外只暴露原子结果，因此无需加锁。
class NoiseFloorEstimator {
 public:
  void reset() noexcept {
    for (auto& bin : bins_) {
      bin = 0;
    }
    blocks_ = 0;
    floor_db_.store(-120.0f);
    speech_db_.store(-120.0f);
  }

  // 在实时线程调用：只做平方和，无分配。
  void process(const float* data, int frames) noexcept {
    if (frames <= 0) {
      return;
    }
    double sum = 0.0;
    for (int i = 0; i < frames; ++i) {
      sum += static_cast<double>(data[i]) * data[i];
    }
    const double mean_square = sum / frames;
    const float db = mean_square > 1e-12 ? static_cast<float>(10.0 * std::log10(mean_square)) : -120.0f;

    const int index = std::clamp(static_cast<int>(-db), 0, kBins - 1);
    ++bins_[index];
    ++blocks_;

    if (blocks_ % kDecayInterval == 0) {
      for (auto& bin : bins_) {
        bin >>= 1;  // 约每 5 秒衰减一次，适应环境变化
      }
    }
    if (blocks_ % kPublishInterval == 0) {
      publish();
    }
  }

  float floor_db() const noexcept { return floor_db_.load(std::memory_order_relaxed); }
  float speech_db() const noexcept { return speech_db_.load(std::memory_order_relaxed); }

 private:
  static constexpr int kBins = 121;             // 0 dB .. -120 dB，1 dB 一格
  static constexpr int kPublishInterval = 32;   // 约 0.3 秒刷新一次
  static constexpr int kDecayInterval = 512;    // 约 5 秒衰减一次

  void publish() noexcept {
    std::uint64_t total = 0;
    for (std::uint32_t bin : bins_) {
      total += bin;
    }
    if (total < 16) {
      return;
    }

    // 从安静端累计 10% 作为噪声底
    std::uint64_t cumulative = 0;
    for (int i = kBins - 1; i >= 0; --i) {
      cumulative += bins_[i];
      if (cumulative * 10 >= total) {
        floor_db_.store(static_cast<float>(-i), std::memory_order_relaxed);
        break;
      }
    }

    // 从响端累计 10% 作为语音电平
    cumulative = 0;
    for (int i = 0; i < kBins; ++i) {
      cumulative += bins_[i];
      if (cumulative * 10 >= total) {
        speech_db_.store(static_cast<float>(-i), std::memory_order_relaxed);
        break;
      }
    }
  }

  std::array<std::uint32_t, kBins> bins_{};
  std::uint64_t blocks_ = 0;
  std::atomic<float> floor_db_{-120.0f};
  std::atomic<float> speech_db_{-120.0f};
};

}  // namespace ms::analysis

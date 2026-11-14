#pragma once

#include "denoise/denoiser.h"
#include "platform/error.h"
#include "vst2_host/vst2_slot.h"

#include <atomic>
#include <string>

namespace ms::denoise {

// DeepFilterNet 内置降噪：加载官方 VST2 DLL 作为默认降噪后端。
//
// 强度直接写进插件自带的 “Denoise Amount” 参数（衰减上限 0–100 dB），
// 全程只跑一路湿声。之前用宿主端干/湿混合控制强度，干声按插件自报延迟
// （512 帧）对齐，而实测该插件真实延迟约 1900 帧，两路错开约 30 ms 叠在
// 一起，在 10%–80% 中间强度就会听到同一段语音出现两遍。
class DeepFilterDenoiser : public Denoiser {
 public:
  ~DeepFilterDenoiser() override;

  // 加载插件；失败时不保留半加载状态，错误交由调用方上报。
  platform::Error load(const std::wstring& path);

  bool loaded() const noexcept { return slot_.loaded(); }
  int latency_frames() const noexcept { return slot_.latency_frames(); }

  void prepare(const dsp_core::ProcessContext& context) override;
  void process(float* data, int frames) override;
  void reset() override;
  void set_amount(float amount) noexcept override;

 private:
  // 把当前强度写进插件原生参数（归一化 0..1 对应 0..100 dB）。
  void push_amount();

  vst2_host::Vst2Slot slot_;
  // 插件强度参数序号；-1 表示该插件没有这个参数。
  int amount_index_ = -1;
  // 已下发给插件的强度，避免每个音频块重复写入。
  float pushed_amount_ = -1.0f;
  std::atomic<float> amount_{0.0f};
};

}  // namespace ms::denoise

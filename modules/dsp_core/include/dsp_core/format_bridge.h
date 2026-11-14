#pragma once

#include "audio_io/endpoint.h"
#include "dsp_core/mono_resampler.h"

#include <cstddef>
#include <cstdint>

namespace ms::dsp_core {

// 采集侧：设备格式（任意采样率 / 声道 / 位深）→ 48 kHz mono float
class CaptureBridge {
 public:
  void prepare(const audio_io::AudioFormat& device, std::uint32_t engine_rate = 48000);

  // 返回写入 out 的帧数。设备格式不受支持时返回 0，supported() 为 false。
  std::size_t process(const std::uint8_t* data, std::uint32_t frames, float* out,
                      std::size_t capacity);

  bool supported() const noexcept { return supported_; }

 private:
  audio_io::AudioFormat device_;
  MonoResampler resampler_;
  bool supported_ = false;
};

// 渲染侧：48 kHz mono float → 设备格式
class RenderBridge {
 public:
  void prepare(const audio_io::AudioFormat& device, std::uint32_t engine_rate = 48000);

  // 把 in_frames 个引擎帧重采样为设备采样率，写入 scratch，返回生成帧数。
  // 调用方负责把不足的部分补静音，再调用 write()。
  std::uint32_t render(const float* mono, std::uint32_t in_frames, float* scratch,
                       std::uint32_t capacity);

  // 设备采样率 mono → 设备字节格式（复制到各声道，必要时转 int16）
  void write(const float* mono, std::uint32_t frames, std::uint8_t* out) const;

  bool supported() const noexcept { return supported_; }
  std::uint32_t device_rate() const noexcept { return device_.sampleRate; }

 private:
  audio_io::AudioFormat device_;
  MonoResampler resampler_;
  bool supported_ = false;
};

}  // namespace ms::dsp_core

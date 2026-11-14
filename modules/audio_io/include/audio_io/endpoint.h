#pragma once

#include <cstdint>
#include <string>

namespace ms::audio_io {

// 设备原生格式的描述。引擎内部统一使用 48 kHz / float32 / mono，
// 转换由 FormatBridge 负责，因此这里只做"如实描述"。
struct AudioFormat {
  std::uint32_t sampleRate = 0;
  std::uint16_t channels = 0;
  std::uint16_t bitsPerSample = 0;
  std::uint16_t blockAlign = 0;
  bool isFloat = false;

  std::uint32_t bytes_per_frame() const noexcept { return blockAlign; }
  double bytes_per_second() const noexcept {
    return static_cast<double>(sampleRate) * static_cast<double>(blockAlign);
  }
};

struct EndpointInfo {
  std::uint32_t index = 0;
  std::wstring id;
  std::wstring friendlyName;
  AudioFormat format;

  // 共享模式下引擎的实际周期（帧）。M1 的低延迟预算直接取自这里。
  std::uint32_t defaultPeriodFrames = 0;
  std::uint32_t minPeriodFrames = 0;

  bool isDefaultConsole = false;
  bool isDefaultCommunications = false;

  // 端点音量状态：用于区分"设备静音"与"采集链路异常"。
  bool muted = false;
  float volumeScalar = 1.0f;
};

}  // namespace ms::audio_io

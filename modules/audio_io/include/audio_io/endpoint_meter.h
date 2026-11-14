#pragma once

#include "platform/error.h"

#include <memory>
#include <string>

namespace ms::audio_io {

// 直接读取端点的峰值表（IAudioMeterInformation），不经过任何采集流。
// 用途：区分"设备本身没信号"与"采集链路异常"。
class EndpointMeter {
 public:
  EndpointMeter();
  ~EndpointMeter();

  EndpointMeter(const EndpointMeter&) = delete;
  EndpointMeter& operator=(const EndpointMeter&) = delete;

  platform::Error open(const std::wstring& endpoint_id);
  bool valid() const noexcept;

  // 0.0 - 1.0 的线性峰值
  float peak() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ms::audio_io

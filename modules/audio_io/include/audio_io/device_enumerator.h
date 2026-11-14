#pragma once

#include "audio_io/endpoint.h"
#include "platform/error.h"

#include <vector>

namespace ms::audio_io {

enum class Flow { Capture, Render };

struct DeviceList {
  std::vector<EndpointInfo> items;
  platform::Error error;
};

// 枚举当前所有 ACTIVE 端点。调用线程必须已初始化 COM（MTA）。
DeviceList enumerate_devices(Flow flow);

}  // namespace ms::audio_io

#pragma once

#include "platform/error.h"

#include <functional>
#include <memory>
#include <string>

namespace ms::audio_io {

// 端点变化监听：设备拔出、状态变化、默认设备切换。
// 回调来自系统线程，禁止在其中做重建等重活，只应设置标志。
class DeviceWatcher {
 public:
  enum class Event { Removed, StateChanged, DefaultChanged };

  using Callback = std::function<void(Event event, const std::wstring& endpoint_id)>;

  DeviceWatcher();
  ~DeviceWatcher();

  DeviceWatcher(const DeviceWatcher&) = delete;
  DeviceWatcher& operator=(const DeviceWatcher&) = delete;

  platform::Error start(Callback callback);
  void stop();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ms::audio_io

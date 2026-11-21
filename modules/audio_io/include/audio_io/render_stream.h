#pragma once

#include "audio_io/endpoint.h"
#include "platform/error.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace ms::audio_io {

// 回调在渲染线程执行：把设备格式的样本写入 data。无数据时必须写静音，
// 否则会复现上一块内容（听感为卡带式重复）。
using RenderCallback = std::function<void(std::uint8_t* data, std::uint32_t frames,
                                          const AudioFormat& format)>;

class RenderStream {
 public:
  struct Stats {
    std::uint64_t frames = 0;
    std::uint32_t buffers = 0;
    std::uint32_t silentBuffers = 0;
    std::uint32_t discontinuities = 0;
    std::uint32_t timeouts = 0;
  };

  RenderStream();
  ~RenderStream();

  RenderStream(const RenderStream&) = delete;
  RenderStream& operator=(const RenderStream&) = delete;

  platform::Error open(const std::wstring& endpoint_id);
  platform::Error start(RenderCallback callback);
  void stop();

  // 停止并释放端点（设备归还系统）。之后可再次 open。
  void close();

  const AudioFormat& format() const noexcept;
  std::uint32_t buffer_frames() const noexcept;
  bool running() const noexcept;
  Stats stats() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ms::audio_io

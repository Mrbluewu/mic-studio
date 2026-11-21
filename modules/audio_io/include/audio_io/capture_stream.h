#pragma once

#include "audio_io/endpoint.h"
#include "platform/error.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace ms::audio_io {

// 回调运行在采集线程（MMCSS Pro Audio）。禁止在回调内分配内存、加锁或写日志。
using CaptureCallback =
    std::function<void(const std::uint8_t* data, std::uint32_t frames, const AudioFormat& format)>;

class CaptureStream {
 public:
  struct Stats {
    std::uint64_t frames = 0;
    std::uint32_t buffers = 0;
    std::uint32_t silentBuffers = 0;
    std::uint32_t discontinuities = 0;
    std::uint32_t timeouts = 0;
  };

  CaptureStream();
  ~CaptureStream();

  CaptureStream(const CaptureStream&) = delete;
  CaptureStream& operator=(const CaptureStream&) = delete;

  // 在主线程调用：打开端点、协商格式、创建事件。失败时返回明确错误码。
  // loopback = true 时传入的必须是渲染端点，用于抓取"正在播放"的音频（自检与入向降噪）。
  platform::Error open(const std::wstring& endpoint_id, bool loopback = false);

  // 启动采集线程。回调对象在内部保存，调用方不要捕获短生命周期对象。
  platform::Error start(CaptureCallback callback);

  // 停止并等待线程退出，随后停止底层流。可重复调用。
  void stop();

  // 停止并释放端点（设备归还系统）。之后可再次 open。
  void close();

  const AudioFormat& format() const noexcept;
  // 开流后引擎实际使用的周期（帧）；未打开时为 0。
  std::uint32_t period_frames() const noexcept;
  bool running() const noexcept;
  Stats stats() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ms::audio_io

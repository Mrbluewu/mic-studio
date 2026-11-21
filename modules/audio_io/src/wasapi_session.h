#pragma once

#include "audio_io/endpoint.h"
#include "platform/error.h"

#include <windows.h>

#include <audioclient.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

namespace ms::audio_io::detail {

// 抓取与渲染共用的共享模式会话：枚举端点 → 激活 IAudioClient → 协商格式 →
// 建事件 → Initialize → SetEventHandle → 起音频线程。
// 两者的差异只有数据方向和少量打开参数，故用 Options 表达，循环体留在各自 .cpp。
struct WasapiOptions {
  bool loopback = false;             // 抓渲染端点（采集端专用）
  bool request_raw = false;          // 绕过端点音效 APO（采集端专用）
  bool use_default_period = false;   // true 时按设备默认周期申请缓冲
  // true 时优先用 IAudioClient3 按引擎最小周期开流（仅非 loopback）。
  // 接口不可用或驱动拒绝时自动回退到经典 Initialize，不影响兼容性。
  bool low_latency = false;
  const wchar_t* device_missing = L"找不到该端点";
};

struct WasapiSession {
  Microsoft::WRL::ComPtr<IAudioClient> client;
  HANDLE event = nullptr;
  AudioFormat format;
  // 开流后引擎实际使用的周期（帧）。统计延迟时用它代替写死的 10 ms。
  std::uint32_t period_frames = 0;

  std::thread thread;
  std::atomic<bool> running{false};

  std::atomic<std::uint64_t> frames{0};
  std::atomic<std::uint32_t> buffers{0};
  std::atomic<std::uint32_t> silent{0};
  std::atomic<std::uint32_t> discontinuities{0};
  std::atomic<std::uint32_t> timeouts{0};

  WasapiSession() = default;
  ~WasapiSession();

  WasapiSession(const WasapiSession&) = delete;
  WasapiSession& operator=(const WasapiSession&) = delete;

  // 主线程调用。已打开时先丢弃旧流。成功后 client 可用，可继续 GetService。
  platform::Error open(const std::wstring& endpoint_id, const WasapiOptions& options);

  // 启动音频线程并调用 client->Start()。body 在该线程内跑，返回后线程退出。
  platform::Error start(std::function<void()> body);

  // 停止并等待线程退出，再停止底层流。可重复调用。
  void stop();

  // 停止后释放 IAudioClient（以及 Overlapped 等端点资源），设备立即归还系统。
  void close();

  // 音频线程的等待：返回 true 表示可以继续处理；false 表示线程应退出。
  // 超时会计入 timeouts，但只要会话仍在运行就返回 true（与原逐流实现一致）。
  bool wait_ready(DWORD timeout_ms = 200);
};

}  // namespace ms::audio_io::detail

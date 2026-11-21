#include "wasapi_session.h"

#include <audioclient.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <cmath>
#include <utility>

#include "format_util.h"
#include "platform/rt_thread.h"

using Microsoft::WRL::ComPtr;

namespace ms::audio_io::detail {

WasapiSession::~WasapiSession() {
  stop();
  if (event != nullptr) {
    ::CloseHandle(event);
    event = nullptr;
  }
}

platform::Error WasapiSession::open(const std::wstring& endpoint_id, const WasapiOptions& options) {
  if (client) {
    stop();
    client.Reset();
  }

  ComPtr<IMMDeviceEnumerator> enumerator;
  HRESULT hr = ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&enumerator));
  if (FAILED(hr)) {
    return platform::Error::make(platform::ErrorCode::DeviceEnumFailed, hr);
  }

  ComPtr<IMMDevice> device;
  hr = enumerator->GetDevice(endpoint_id.c_str(), &device);
  if (FAILED(hr) || !device) {
    return platform::Error::make(platform::ErrorCode::DeviceNotFound, hr, options.device_missing);
  }

  ComPtr<IAudioClient> opened;
  hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &opened);
  if (FAILED(hr) || !opened) {
    return platform::Error::make(platform::ErrorCode::DeviceActivateFailed, hr);
  }

  WAVEFORMATEX* mix = nullptr;
  hr = opened->GetMixFormat(&mix);
  if (FAILED(hr) || mix == nullptr) {
    return platform::Error::make(platform::ErrorCode::FormatUnsupported, hr);
  }
  format = parse_format(mix);

  if (event == nullptr) {
    event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (event == nullptr) {
      ::CoTaskMemFree(mix);
      return platform::Error::make(platform::ErrorCode::StreamInitFailed,
                                   HRESULT_FROM_WIN32(::GetLastError()), L"创建事件失败");
    }
  }

  DWORD stream_flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
  if (options.loopback) {
    stream_flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
  }

  // 采集端尽量走 RAW：绕过端点上的音效 APO。
  // 我们自己就是处理器，系统增强只会叠加不可控的延迟与染色。
  // 注意：loopback 流不支持 RAW，会直接返回 E_INVALIDARG。
  if (options.request_raw && !options.loopback) {
    ComPtr<IAudioClient2> client2;
    if (SUCCEEDED(opened.As(&client2))) {
      AudioClientProperties properties{};
      properties.cbSize = sizeof(properties);
      properties.bIsOffload = FALSE;
      properties.eCategory = AudioCategory_Communications;
      properties.Options = AUDCLNT_STREAMOPTIONS_RAW;
      client2->SetClientProperties(&properties);
    }
  }

  // 采集端传 0 让引擎自行选择缓冲长度：部分设备在指定周期时会返回全零数据。
  // 渲染端必须给出周期，否则 GetCurrentPadding 的可用帧数没有稳定基准。
  REFERENCE_TIME period = 0;
  if (options.use_default_period) {
    REFERENCE_TIME default_period = 0;
    REFERENCE_TIME min_period = 0;
    opened->GetDevicePeriod(&default_period, &min_period);
    period = default_period > 0 ? default_period : 100'000;  // 兜底 10 ms
  }

  // 低延迟共享模式：IAudioClient3 允许把引擎周期压到设备最小值
  // （CABLE 类端点常见 128 帧 ≈ 2.7 ms，默认周期是 480 帧）。
  // 只对渲染端尝试：采集端抓的是设备投递节奏，且历史上有设备在指定周期时
  // 返回全零数据。取不到接口或驱动拒绝时直接回退经典 Initialize。
  bool initialized = false;
  if (options.low_latency && !options.loopback) {
    ComPtr<IAudioClient3> client3;
    if (SUCCEEDED(opened.As(&client3))) {
      UINT32 default_frames = 0;
      UINT32 fundamental = 0;
      UINT32 min_frames = 0;
      UINT32 max_frames = 0;
      if (SUCCEEDED(client3->GetSharedModeEnginePeriod(mix, &default_frames, &fundamental,
                                                       &min_frames, &max_frames)) &&
          min_frames > 0 && min_frames < default_frames) {
        UINT32 request = min_frames;
        if (fundamental > 1) {
          request = (request + fundamental - 1) / fundamental * fundamental;
        }
        request = std::min(request, max_frames);
        if (SUCCEEDED(client3->InitializeSharedAudioStream(stream_flags, request, mix, nullptr))) {
          initialized = true;
          period_frames = request;
        }
      }
    }
  }

  if (!initialized) {
    hr = opened->Initialize(AUDCLNT_SHAREMODE_SHARED, stream_flags, period, 0, mix, nullptr);
    if (FAILED(hr)) {
      ::CoTaskMemFree(mix);
      return platform::Error::make(platform::ErrorCode::StreamInitFailed, hr);
    }
    // 采集端传 0 时周期由引擎决定，回读实际值用于延迟统计。
    REFERENCE_TIME actual_default = 0;
    REFERENCE_TIME actual_min = 0;
    if (SUCCEEDED(opened->GetDevicePeriod(&actual_default, &actual_min)) && actual_default > 0) {
      period_frames = static_cast<std::uint32_t>(std::lround(
          static_cast<double>(actual_default) * format.sampleRate / 10'000'000.0));
    }
  }
  ::CoTaskMemFree(mix);

  hr = opened->SetEventHandle(event);
  if (FAILED(hr)) {
    return platform::Error::make(platform::ErrorCode::StreamInitFailed, hr);
  }

  client = opened;
  return {};
}

platform::Error WasapiSession::start(std::function<void()> body) {
  if (!client) {
    return platform::Error::make(platform::ErrorCode::StreamStartFailed, E_UNEXPECTED, L"流尚未打开");
  }
  if (running.load()) {
    return {};
  }

  running.store(true);
  HRESULT hr = client->Start();
  if (FAILED(hr)) {
    running.store(false);
    return platform::Error::make(platform::ErrorCode::StreamStartFailed, hr);
  }

  thread = std::thread([this, body = std::move(body)]() {
    // 音频线程加入 MTA，随后提升为 MMCSS "Pro Audio"。
    ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
      platform::RtScope rt_scope;
      body();
    }
    ::CoUninitialize();
  });

  return {};
}

void WasapiSession::stop() {
  running.store(false);
  if (thread.joinable()) {
    thread.join();
  }
  if (client) {
    client->Stop();
  }
}

void WasapiSession::close() {
  stop();
  client.Reset();
}

bool WasapiSession::wait_ready(DWORD timeout_ms) {
  const DWORD wait = ::WaitForSingleObject(event, timeout_ms);
  if (wait == WAIT_OBJECT_0) {
    return true;
  }
  if (wait == WAIT_TIMEOUT) {
    timeouts.fetch_add(1, std::memory_order_relaxed);
    return running.load(std::memory_order_relaxed);
  }
  return false;
}

}  // namespace ms::audio_io::detail

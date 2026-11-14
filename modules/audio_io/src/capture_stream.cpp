#include "audio_io/capture_stream.h"

#include <windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <atomic>
#include <thread>
#include <utility>

#include "format_util.h"
#include "platform/rt_thread.h"

using Microsoft::WRL::ComPtr;

namespace ms::audio_io {

struct CaptureStream::Impl {
  ComPtr<IAudioClient> client;
  ComPtr<IAudioCaptureClient> capture;
  HANDLE event = nullptr;
  AudioFormat format;
  CaptureCallback callback;

  std::thread thread;
  std::atomic<bool> running{false};

  std::atomic<std::uint64_t> frames{0};
  std::atomic<std::uint32_t> buffers{0};
  std::atomic<std::uint32_t> silent{0};
  std::atomic<std::uint32_t> discontinuities{0};
  std::atomic<std::uint32_t> timeouts{0};
};

CaptureStream::CaptureStream() : impl_(std::make_unique<Impl>()) {}

CaptureStream::~CaptureStream() {
  stop();
  if (impl_->event != nullptr) {
    ::CloseHandle(impl_->event);
    impl_->event = nullptr;
  }
}

platform::Error CaptureStream::open(const std::wstring& endpoint_id, bool loopback) {
  loopback_ = loopback;
  if (impl_->client) {
    stop();
    impl_->client.Reset();
    impl_->capture.Reset();
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
    return platform::Error::make(platform::ErrorCode::DeviceNotFound, hr, L"找不到该端点");
  }

  ComPtr<IAudioClient> client;
  hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client);
  if (FAILED(hr) || !client) {
    return platform::Error::make(platform::ErrorCode::DeviceActivateFailed, hr);
  }

  WAVEFORMATEX* mix = nullptr;
  hr = client->GetMixFormat(&mix);
  if (FAILED(hr) || mix == nullptr) {
    return platform::Error::make(platform::ErrorCode::FormatUnsupported, hr);
  }
  impl_->format = detail::parse_format(mix);

  if (impl_->event == nullptr) {
    impl_->event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (impl_->event == nullptr) {
      ::CoTaskMemFree(mix);
      return platform::Error::make(platform::ErrorCode::StreamInitFailed, HRESULT_FROM_WIN32(::GetLastError()),
                                   L"创建事件失败");
    }
  }

  // 共享模式下用默认周期作为缓冲长度；M1 会换用 IAudioClient3 申请最小周期。
  REFERENCE_TIME default_period = 0;
  REFERENCE_TIME min_period = 0;
  client->GetDevicePeriod(&default_period, &min_period);
  if (default_period <= 0) {
    default_period = 100'000;  // 兜底 10 ms
  }

  DWORD stream_flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
  if (loopback_) {
    stream_flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
  }

  // 采集端尽量走 RAW：绕过端点上的音效 APO。
  // 我们自己就是处理器，系统增强只会叠加不可控的延迟与染色。
  // 注意：loopback 流不支持 RAW，会直接返回 E_INVALIDARG。
  ComPtr<IAudioClient2> client2;
  if (!loopback_ && SUCCEEDED(client.As(&client2))) {
    AudioClientProperties properties{};
    properties.cbSize = sizeof(properties);
    properties.bIsOffload = FALSE;
    properties.eCategory = AudioCategory_Communications;
    properties.Options = AUDCLNT_STREAMOPTIONS_RAW;
    client2->SetClientProperties(&properties);
  }

  // 让引擎自行选择缓冲长度：部分设备在指定周期时会返回全零数据。
  hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, stream_flags, 0, 0, mix, nullptr);
  ::CoTaskMemFree(mix);
  if (FAILED(hr)) {
    return platform::Error::make(platform::ErrorCode::StreamInitFailed, hr);
  }

  hr = client->SetEventHandle(impl_->event);
  if (FAILED(hr)) {
    return platform::Error::make(platform::ErrorCode::StreamInitFailed, hr);
  }

  ComPtr<IAudioCaptureClient> capture;
  hr = client->GetService(IID_PPV_ARGS(&capture));
  if (FAILED(hr) || !capture) {
    return platform::Error::make(platform::ErrorCode::StreamInitFailed, hr);
  }

  impl_->client = client;
  impl_->capture = capture;
  return {};
}

platform::Error CaptureStream::start(CaptureCallback callback) {
  if (!impl_->client || !impl_->capture) {
    return platform::Error::make(platform::ErrorCode::StreamStartFailed, E_UNEXPECTED, L"流尚未打开");
  }
  if (impl_->running.load()) {
    return {};
  }

  impl_->callback = std::move(callback);
  impl_->running.store(true);

  HRESULT hr = impl_->client->Start();
  if (FAILED(hr)) {
    impl_->running.store(false);
    return platform::Error::make(platform::ErrorCode::StreamStartFailed, hr);
  }

  Impl* impl = impl_.get();
  impl->thread = std::thread([impl]() {
    // 采集线程加入 MTA，随后提升为 MMCSS "Pro Audio"。
    ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
      platform::RtScope rt_scope;

      while (impl->running.load(std::memory_order_relaxed)) {
        const DWORD wait = ::WaitForSingleObject(impl->event, 200);
        if (wait != WAIT_OBJECT_0) {
          if (wait == WAIT_TIMEOUT) {
            impl->timeouts.fetch_add(1, std::memory_order_relaxed);
            continue;
          }
          break;
        }

        UINT32 packet = 0;
        while (impl->running.load(std::memory_order_relaxed) &&
               SUCCEEDED(impl->capture->GetNextPacketSize(&packet)) && packet > 0) {
          BYTE* data = nullptr;
          UINT32 frames = 0;
          DWORD flags = 0;

          const HRESULT hr = impl->capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
          if (hr == AUDCLNT_S_BUFFER_EMPTY) {
            break;
          }
          if (FAILED(hr)) {
            impl->running.store(false);
            break;
          }

          if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr) {
            impl->silent.fetch_add(1, std::memory_order_relaxed);
          } else if (impl->callback) {
            impl->callback(data, frames, impl->format);
          }

          impl->frames.fetch_add(frames, std::memory_order_relaxed);
          impl->buffers.fetch_add(1, std::memory_order_relaxed);
          if ((flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0) {
            impl->discontinuities.fetch_add(1, std::memory_order_relaxed);
          }

          impl->capture->ReleaseBuffer(frames);
        }
      }
    }
    ::CoUninitialize();
  });

  return {};
}

void CaptureStream::stop() {
  impl_->running.store(false);
  if (impl_->thread.joinable()) {
    impl_->thread.join();
  }
  if (impl_->client) {
    impl_->client->Stop();
  }
}

const AudioFormat& CaptureStream::format() const noexcept { return impl_->format; }

bool CaptureStream::running() const noexcept { return impl_->running.load(); }

CaptureStream::Stats CaptureStream::stats() const noexcept {
  Stats stats;
  stats.frames = impl_->frames.load(std::memory_order_relaxed);
  stats.buffers = impl_->buffers.load(std::memory_order_relaxed);
  stats.silentBuffers = impl_->silent.load(std::memory_order_relaxed);
  stats.discontinuities = impl_->discontinuities.load(std::memory_order_relaxed);
  stats.timeouts = impl_->timeouts.load(std::memory_order_relaxed);
  return stats;
}

}  // namespace ms::audio_io

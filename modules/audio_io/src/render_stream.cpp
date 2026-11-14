#include "audio_io/render_stream.h"

#include <windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <atomic>
#include <cstring>
#include <thread>
#include <utility>

#include "format_util.h"
#include "platform/rt_thread.h"

using Microsoft::WRL::ComPtr;

namespace ms::audio_io {

struct RenderStream::Impl {
  ComPtr<IAudioClient> client;
  ComPtr<IAudioRenderClient> render;
  HANDLE event = nullptr;
  AudioFormat format;
  RenderCallback callback;
  UINT32 buffer_frames = 0;

  std::thread thread;
  std::atomic<bool> running{false};

  std::atomic<std::uint64_t> frames{0};
  std::atomic<std::uint32_t> buffers{0};
  std::atomic<std::uint32_t> silent{0};
  std::atomic<std::uint32_t> discontinuities{0};
  std::atomic<std::uint32_t> timeouts{0};
};

RenderStream::RenderStream() : impl_(std::make_unique<Impl>()) {}

RenderStream::~RenderStream() {
  stop();
  if (impl_->event != nullptr) {
    ::CloseHandle(impl_->event);
    impl_->event = nullptr;
  }
}

platform::Error RenderStream::open(const std::wstring& endpoint_id) {
  if (impl_->client) {
    stop();
    impl_->client.Reset();
    impl_->render.Reset();
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
    return platform::Error::make(platform::ErrorCode::DeviceNotFound, hr, L"找不到该渲染端点");
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
      return platform::Error::make(platform::ErrorCode::StreamInitFailed,
                                   HRESULT_FROM_WIN32(::GetLastError()), L"创建事件失败");
    }
  }

  REFERENCE_TIME default_period = 0;
  REFERENCE_TIME min_period = 0;
  client->GetDevicePeriod(&default_period, &min_period);
  if (default_period <= 0) {
    default_period = 100'000;
  }

  hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, default_period, 0,
                          mix, nullptr);
  ::CoTaskMemFree(mix);
  if (FAILED(hr)) {
    return platform::Error::make(platform::ErrorCode::StreamInitFailed, hr);
  }

  hr = client->SetEventHandle(impl_->event);
  if (FAILED(hr)) {
    return platform::Error::make(platform::ErrorCode::StreamInitFailed, hr);
  }

  ComPtr<IAudioRenderClient> render;
  hr = client->GetService(IID_PPV_ARGS(&render));
  if (FAILED(hr) || !render) {
    return platform::Error::make(platform::ErrorCode::StreamInitFailed, hr);
  }

  UINT32 buffer_frames = 0;
  if (FAILED(client->GetBufferSize(&buffer_frames))) {
    return platform::Error::make(platform::ErrorCode::StreamInitFailed, E_FAIL);
  }

  impl_->client = client;
  impl_->render = render;
  impl_->buffer_frames = buffer_frames;
  return {};
}

platform::Error RenderStream::start(RenderCallback callback) {
  if (!impl_->client || !impl_->render) {
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

        UINT32 padding = 0;
        if (FAILED(impl->client->GetCurrentPadding(&padding))) {
          impl->running.store(false);
          break;
        }

        const UINT32 available = impl->buffer_frames - padding;
        if (available == 0) {
          continue;
        }

        BYTE* data = nullptr;
        if (FAILED(impl->render->GetBuffer(available, &data)) || data == nullptr) {
          impl->running.store(false);
          break;
        }

        if (impl->callback) {
          impl->callback(data, available, impl->format);
        } else {
          std::memset(data, 0, static_cast<std::size_t>(available) * impl->format.blockAlign);
          impl->silent.fetch_add(1, std::memory_order_relaxed);
        }

        impl->render->ReleaseBuffer(available, 0);
        impl->frames.fetch_add(available, std::memory_order_relaxed);
        impl->buffers.fetch_add(1, std::memory_order_relaxed);
      }
    }
    ::CoUninitialize();
  });

  return {};
}

void RenderStream::stop() {
  impl_->running.store(false);
  if (impl_->thread.joinable()) {
    impl_->thread.join();
  }
  if (impl_->client) {
    impl_->client->Stop();
  }
}

const AudioFormat& RenderStream::format() const noexcept { return impl_->format; }

std::uint32_t RenderStream::buffer_frames() const noexcept { return impl_->buffer_frames; }

bool RenderStream::running() const noexcept { return impl_->running.load(); }

RenderStream::Stats RenderStream::stats() const noexcept {
  Stats stats;
  stats.frames = impl_->frames.load(std::memory_order_relaxed);
  stats.buffers = impl_->buffers.load(std::memory_order_relaxed);
  stats.silentBuffers = impl_->silent.load(std::memory_order_relaxed);
  stats.discontinuities = impl_->discontinuities.load(std::memory_order_relaxed);
  stats.timeouts = impl_->timeouts.load(std::memory_order_relaxed);
  return stats;
}

}  // namespace ms::audio_io

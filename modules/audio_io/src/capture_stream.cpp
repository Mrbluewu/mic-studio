#include "audio_io/capture_stream.h"

#include <windows.h>

#include <audioclient.h>

#include <memory>
#include <utility>

#include "wasapi_session.h"

using Microsoft::WRL::ComPtr;

namespace ms::audio_io {

struct CaptureStream::Impl {
  detail::WasapiSession session;
  ComPtr<IAudioCaptureClient> capture;
  CaptureCallback callback;
};

CaptureStream::CaptureStream() : impl_(std::make_unique<Impl>()) {}

CaptureStream::~CaptureStream() = default;

platform::Error CaptureStream::open(const std::wstring& endpoint_id, bool loopback) {
  impl_->capture.Reset();

  detail::WasapiOptions options;
  options.loopback = loopback;
  options.request_raw = true;
  options.device_missing = L"找不到该端点";
  const auto error = impl_->session.open(endpoint_id, options);
  if (error.failed()) {
    return error;
  }

  ComPtr<IAudioCaptureClient> capture;
  const HRESULT hr = impl_->session.client->GetService(IID_PPV_ARGS(&capture));
  if (FAILED(hr) || !capture) {
    impl_->session.close();
    return platform::Error::make(platform::ErrorCode::StreamInitFailed, hr);
  }

  impl_->capture = capture;
  return {};
}

platform::Error CaptureStream::start(CaptureCallback callback) {
  if (!impl_->session.client || !impl_->capture) {
    return platform::Error::make(platform::ErrorCode::StreamStartFailed, E_UNEXPECTED, L"流尚未打开");
  }

  impl_->callback = std::move(callback);
  const auto error = impl_->session.start([impl = impl_.get()]() {
    while (impl->session.running.load(std::memory_order_relaxed)) {
      if (!impl->session.wait_ready()) {
        break;
      }

      UINT32 packet = 0;
      while (impl->session.running.load(std::memory_order_relaxed) &&
             SUCCEEDED(impl->capture->GetNextPacketSize(&packet)) && packet > 0) {
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;

        const HRESULT hr = impl->capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
        if (hr == AUDCLNT_S_BUFFER_EMPTY) {
          break;
        }
        if (FAILED(hr)) {
          impl->session.running.store(false);
          break;
        }

        if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr) {
          impl->session.silent.fetch_add(1, std::memory_order_relaxed);
        } else if (impl->callback) {
          impl->callback(data, frames, impl->session.format);
        }

        impl->session.frames.fetch_add(frames, std::memory_order_relaxed);
        impl->session.buffers.fetch_add(1, std::memory_order_relaxed);
        if ((flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0) {
          impl->session.discontinuities.fetch_add(1, std::memory_order_relaxed);
        }

        impl->capture->ReleaseBuffer(frames);
      }
    }
  });
  if (error.failed()) {
    impl_->callback = nullptr;
  }
  return error;
}

void CaptureStream::stop() { impl_->session.stop(); }

void CaptureStream::close() {
  impl_->session.close();
  impl_->capture.Reset();
  impl_->callback = nullptr;
}

const AudioFormat& CaptureStream::format() const noexcept { return impl_->session.format; }

std::uint32_t CaptureStream::period_frames() const noexcept {
  return impl_->session.period_frames;
}

bool CaptureStream::running() const noexcept { return impl_->session.running.load(); }

CaptureStream::Stats CaptureStream::stats() const noexcept {
  const auto& session = impl_->session;
  Stats stats;
  stats.frames = session.frames.load(std::memory_order_relaxed);
  stats.buffers = session.buffers.load(std::memory_order_relaxed);
  stats.silentBuffers = session.silent.load(std::memory_order_relaxed);
  stats.discontinuities = session.discontinuities.load(std::memory_order_relaxed);
  stats.timeouts = session.timeouts.load(std::memory_order_relaxed);
  return stats;
}

}  // namespace ms::audio_io

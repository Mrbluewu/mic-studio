#include "audio_io/render_stream.h"

#include <windows.h>

#include <audioclient.h>

#include <cstring>
#include <memory>
#include <utility>

#include "wasapi_session.h"

using Microsoft::WRL::ComPtr;

namespace ms::audio_io {

struct RenderStream::Impl {
  detail::WasapiSession session;
  ComPtr<IAudioRenderClient> render;
  RenderCallback callback;
  UINT32 buffer_frames = 0;
};

RenderStream::RenderStream() : impl_(std::make_unique<Impl>()) {}

RenderStream::~RenderStream() = default;

platform::Error RenderStream::open(const std::wstring& endpoint_id) {
  impl_->render.Reset();

  detail::WasapiOptions options;
  options.use_default_period = true;
  // 渲染端延迟直接压到设备最小时钟周期；接口/驱动不支持时回退默认周期。
  options.low_latency = true;
  options.device_missing = L"找不到该渲染端点";
  const auto error = impl_->session.open(endpoint_id, options);
  if (error.failed()) {
    return error;
  }

  ComPtr<IAudioRenderClient> render;
  HRESULT hr = impl_->session.client->GetService(IID_PPV_ARGS(&render));
  if (FAILED(hr) || !render) {
    impl_->session.close();
    return platform::Error::make(platform::ErrorCode::StreamInitFailed, hr);
  }

  UINT32 buffer_frames = 0;
  if (FAILED(impl_->session.client->GetBufferSize(&buffer_frames))) {
    impl_->session.close();
    return platform::Error::make(platform::ErrorCode::StreamInitFailed, E_FAIL);
  }

  impl_->render = render;
  impl_->buffer_frames = buffer_frames;
  return {};
}

platform::Error RenderStream::start(RenderCallback callback) {
  if (!impl_->session.client || !impl_->render) {
    return platform::Error::make(platform::ErrorCode::StreamStartFailed, E_UNEXPECTED, L"流尚未打开");
  }

  impl_->callback = std::move(callback);
  const auto error = impl_->session.start([impl = impl_.get()]() {
    auto& session = impl->session;
    while (session.running.load(std::memory_order_relaxed)) {
      if (!session.wait_ready()) {
        break;
      }

      UINT32 padding = 0;
      if (FAILED(session.client->GetCurrentPadding(&padding))) {
        session.running.store(false);
        break;
      }

      const UINT32 available = impl->buffer_frames - padding;
      if (available == 0) {
        continue;
      }

      BYTE* data = nullptr;
      if (FAILED(impl->render->GetBuffer(available, &data)) || data == nullptr) {
        session.running.store(false);
        break;
      }

      if (impl->callback) {
        impl->callback(data, available, session.format);
      } else {
        std::memset(data, 0, static_cast<std::size_t>(available) * session.format.blockAlign);
        session.silent.fetch_add(1, std::memory_order_relaxed);
      }

      impl->render->ReleaseBuffer(available, 0);
      session.frames.fetch_add(available, std::memory_order_relaxed);
      session.buffers.fetch_add(1, std::memory_order_relaxed);
    }
  });
  if (error.failed()) {
    impl_->callback = nullptr;
  }
  return error;
}

void RenderStream::stop() { impl_->session.stop(); }

void RenderStream::close() {
  impl_->session.close();
  impl_->render.Reset();
  impl_->callback = nullptr;
  impl_->buffer_frames = 0;
}

const AudioFormat& RenderStream::format() const noexcept { return impl_->session.format; }

std::uint32_t RenderStream::buffer_frames() const noexcept { return impl_->buffer_frames; }

bool RenderStream::running() const noexcept { return impl_->session.running.load(); }

RenderStream::Stats RenderStream::stats() const noexcept {
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

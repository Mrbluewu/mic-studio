#include "audio_io/device_watcher.h"

#include <windows.h>

#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <atomic>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace ms::audio_io {

struct DeviceWatcher::Impl final : public IMMNotificationClient {
  std::atomic<ULONG> refs{1};
  Callback callback;
  ComPtr<IMMDeviceEnumerator> enumerator;

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
    if (object == nullptr) {
      return E_POINTER;
    }
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
      *object = static_cast<IMMNotificationClient*>(this);
      AddRef();
      return S_OK;
    }
    *object = nullptr;
    return E_NOINTERFACE;
  }

  ULONG STDMETHODCALLTYPE AddRef() override { return refs.fetch_add(1) + 1; }

  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG remaining = refs.fetch_sub(1) - 1;
    if (remaining == 0) {
      delete this;
    }
    return remaining;
  }

  void notify(Event event, LPCWSTR id) {
    if (callback && id != nullptr) {
      callback(event, std::wstring(id));
    }
  }

  HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR device_id, DWORD) override {
    notify(Event::StateChanged, device_id);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR device_id) override {
    notify(Event::Removed, device_id);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow, ERole, LPCWSTR device_id) override {
    notify(Event::DefaultChanged, device_id);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override {
    return S_OK;
  }
};

DeviceWatcher::DeviceWatcher() : impl_(std::make_unique<Impl>()) {}

DeviceWatcher::~DeviceWatcher() { stop(); }

platform::Error DeviceWatcher::start(Callback callback) {
  stop();

  ComPtr<IMMDeviceEnumerator> enumerator;
  HRESULT hr = ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&enumerator));
  if (FAILED(hr)) {
    return platform::Error::make(platform::ErrorCode::DeviceEnumFailed, hr);
  }

  impl_->callback = std::move(callback);
  impl_->enumerator = enumerator;

  hr = impl_->enumerator->RegisterEndpointNotificationCallback(impl_.get());
  if (FAILED(hr)) {
    impl_->enumerator.Reset();
    return platform::Error::make(platform::ErrorCode::DeviceEnumFailed, hr);
  }
  return {};
}

void DeviceWatcher::stop() {
  if (impl_->enumerator) {
    impl_->enumerator->UnregisterEndpointNotificationCallback(impl_.get());
    impl_->enumerator.Reset();
  }
  impl_->callback = nullptr;
}

}  // namespace ms::audio_io

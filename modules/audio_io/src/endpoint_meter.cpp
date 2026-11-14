#include "audio_io/endpoint_meter.h"

#include <windows.h>

#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace ms::audio_io {

struct EndpointMeter::Impl {
  ComPtr<IAudioMeterInformation> meter;
};

EndpointMeter::EndpointMeter() : impl_(std::make_unique<Impl>()) {}

EndpointMeter::~EndpointMeter() = default;

platform::Error EndpointMeter::open(const std::wstring& endpoint_id) {
  ComPtr<IMMDeviceEnumerator> enumerator;
  HRESULT hr = ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&enumerator));
  if (FAILED(hr)) {
    return platform::Error::make(platform::ErrorCode::DeviceEnumFailed, hr);
  }

  ComPtr<IMMDevice> device;
  hr = enumerator->GetDevice(endpoint_id.c_str(), &device);
  if (FAILED(hr) || !device) {
    return platform::Error::make(platform::ErrorCode::DeviceNotFound, hr);
  }

  ComPtr<IAudioMeterInformation> meter;
  hr = device->Activate(__uuidof(IAudioMeterInformation), CLSCTX_ALL, nullptr, &meter);
  if (FAILED(hr) || !meter) {
    return platform::Error::make(platform::ErrorCode::DeviceActivateFailed, hr);
  }

  impl_->meter = meter;
  return {};
}

bool EndpointMeter::valid() const noexcept { return impl_->meter != nullptr; }

float EndpointMeter::peak() const noexcept {
  if (!impl_->meter) {
    return 0.0f;
  }
  float value = 0.0f;
  if (FAILED(impl_->meter->GetPeakValue(&value))) {
    return 0.0f;
  }
  return value;
}

}  // namespace ms::audio_io

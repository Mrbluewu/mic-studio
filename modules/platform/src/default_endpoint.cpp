#include "platform/default_endpoint.h"

#include <windows.h>

#include <mmdeviceapi.h>
#include <mmreg.h>
#include <wrl/client.h>

namespace ms::platform {
namespace {

// 未文档化但长期稳定的接口：Vista 起被系统设置面板与第三方工具广泛使用。
struct IPolicyConfig : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX**) = 0;
  virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, INT64*, INT64*) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, INT64*) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void*) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void*) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR, ERole) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};

constexpr CLSID kPolicyConfigClient = {
    0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};
constexpr IID kIidPolicyConfig = {
    0xf8679f50, 0x850a, 0x41cf, {0x9c, 0x72, 0x43, 0x0f, 0x29, 0x02, 0x90, 0xc8}};

Microsoft::WRL::ComPtr<IPolicyConfig> create_policy_config() {
  Microsoft::WRL::ComPtr<IPolicyConfig> policy;
  const HRESULT hr = ::CoCreateInstance(kPolicyConfigClient, nullptr, CLSCTX_ALL, kIidPolicyConfig,
                                        reinterpret_cast<void**>(policy.GetAddressOf()));
  if (FAILED(hr)) {
    return nullptr;
  }
  return policy;
}

std::wstring device_id(IMMDevice* device) {
  LPWSTR raw = nullptr;
  if (device == nullptr || FAILED(device->GetId(&raw)) || raw == nullptr) {
    return {};
  }
  std::wstring id(raw);
  ::CoTaskMemFree(raw);
  return id;
}

}  // namespace

std::wstring current_default_capture() {
  Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
  if (FAILED(::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&enumerator)))) {
    return {};
  }
  Microsoft::WRL::ComPtr<IMMDevice> device;
  if (FAILED(enumerator->GetDefaultAudioEndpoint(eCapture, eCommunications, &device))) {
    return {};
  }
  return device_id(device.Get());
}

std::wstring set_default_capture(const std::wstring& endpoint_id) {
  if (endpoint_id.empty()) {
    return {};
  }
  const std::wstring previous = current_default_capture();

  auto policy = create_policy_config();
  if (policy == nullptr) {
    return {};
  }

  // 通信与常规角色都切，避免不同应用取到不同的默认设备
  const HRESULT hr_comm = policy->SetDefaultEndpoint(endpoint_id.c_str(), eCommunications);
  policy->SetDefaultEndpoint(endpoint_id.c_str(), eConsole);
  if (FAILED(hr_comm)) {
    return {};
  }
  return previous;
}

bool restore_default_capture(const std::wstring& endpoint_id) {
  if (endpoint_id.empty()) {
    return false;
  }
  auto policy = create_policy_config();
  if (policy == nullptr) {
    return false;
  }
  const HRESULT hr_comm = policy->SetDefaultEndpoint(endpoint_id.c_str(), eCommunications);
  policy->SetDefaultEndpoint(endpoint_id.c_str(), eConsole);
  return SUCCEEDED(hr_comm);
}

}  // namespace ms::platform

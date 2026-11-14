#include "audio_io/device_enumerator.h"

#include <windows.h>

#include <audioclient.h>
#include <endpointvolume.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <utility>

#include "format_util.h"

using Microsoft::WRL::ComPtr;

namespace ms::audio_io {
namespace {

// PKEY_Device_FriendlyName：不引入 functiondiscoverykeys_devpkey.h，
// 避免它对 DEFINE_PROPERTYKEY 的包含顺序依赖。
constexpr PROPERTYKEY kFriendlyName = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

std::wstring device_id(IMMDevice* device) {
  LPWSTR raw = nullptr;
  if (FAILED(device->GetId(&raw)) || raw == nullptr) {
    return {};
  }
  std::wstring id(raw);
  ::CoTaskMemFree(raw);
  return id;
}

}  // namespace

DeviceList enumerate_devices(Flow flow) {
  DeviceList result;

  ComPtr<IMMDeviceEnumerator> enumerator;
  HRESULT hr = ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&enumerator));
  if (FAILED(hr)) {
    result.error = platform::Error::make(platform::ErrorCode::DeviceEnumFailed, hr);
    return result;
  }

  const EDataFlow data_flow = (flow == Flow::Capture) ? eCapture : eRender;

  // 默认端点（控制台 / 通信）用于给用户标注，不参与选择逻辑。
  std::wstring default_console;
  std::wstring default_comm;
  {
    ComPtr<IMMDevice> device;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(data_flow, eConsole, &device))) {
      default_console = device_id(device.Get());
    }
    device.Reset();
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(data_flow, eCommunications, &device))) {
      default_comm = device_id(device.Get());
    }
  }

  ComPtr<IMMDeviceCollection> collection;
  hr = enumerator->EnumAudioEndpoints(data_flow, DEVICE_STATE_ACTIVE, &collection);
  if (FAILED(hr)) {
    result.error = platform::Error::make(platform::ErrorCode::DeviceEnumFailed, hr);
    return result;
  }

  UINT32 count = 0;
  if (FAILED(collection->GetCount(&count))) {
    result.error = platform::Error::make(platform::ErrorCode::DeviceEnumFailed, E_FAIL);
    return result;
  }

  result.items.reserve(count);

  for (UINT32 i = 0; i < count; ++i) {
    ComPtr<IMMDevice> device;
    if (FAILED(collection->Item(i, &device))) {
      continue;
    }

    EndpointInfo info;
    info.index = i;
    info.id = device_id(device.Get());

    ComPtr<IPropertyStore> properties;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties))) {
      PROPVARIANT value;
      ::PropVariantInit(&value);
      if (SUCCEEDED(properties->GetValue(kFriendlyName, &value)) &&
          value.vt == VT_LPWSTR && value.pwszVal != nullptr) {
        info.friendlyName = value.pwszVal;
      }
      ::PropVariantClear(&value);
    }

    ComPtr<IAudioClient> client;
    if (SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client))) {
      WAVEFORMATEX* mix = nullptr;
      if (SUCCEEDED(client->GetMixFormat(&mix)) && mix != nullptr) {
        info.format = detail::parse_format(mix);

        ComPtr<IAudioClient3> client3;
        if (SUCCEEDED(client.As(&client3))) {
          UINT32 default_period = 0;
          UINT32 fundamental_period = 0;
          UINT32 min_period = 0;
          UINT32 max_period = 0;
          if (SUCCEEDED(client3->GetSharedModeEnginePeriod(
                  mix, &default_period, &fundamental_period, &min_period, &max_period))) {
            info.defaultPeriodFrames = default_period;
            info.minPeriodFrames = min_period;
          }
        }

        if (info.defaultPeriodFrames == 0 && info.format.sampleRate != 0) {
          REFERENCE_TIME default_ref = 0;
          REFERENCE_TIME min_ref = 0;
          if (SUCCEEDED(client->GetDevicePeriod(&default_ref, &min_ref)) && default_ref > 0) {
            info.defaultPeriodFrames = static_cast<std::uint32_t>(
                (default_ref * info.format.sampleRate) / 10'000'000LL);
          }
        }
        ::CoTaskMemFree(mix);
      }
    }

    info.isDefaultConsole = !default_console.empty() && info.id == default_console;
    info.isDefaultCommunications = !default_comm.empty() && info.id == default_comm;

    ComPtr<IAudioEndpointVolume> volume;
    if (SUCCEEDED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, &volume))) {
      BOOL mute = FALSE;
      if (SUCCEEDED(volume->GetMute(&mute))) {
        info.muted = (mute != FALSE);
      }
      float scalar = 0.0f;
      if (SUCCEEDED(volume->GetMasterVolumeLevelScalar(&scalar))) {
        info.volumeScalar = scalar;
      }
    }

    result.items.push_back(std::move(info));
  }

  return result;
}

}  // namespace ms::audio_io

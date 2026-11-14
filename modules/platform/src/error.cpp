#include "platform/error.h"

#include <windows.h>

#include <utility>

namespace ms::platform {

const wchar_t* to_string(ErrorCode code) {
  switch (code) {
    case ErrorCode::Ok: return L"Ok";
    case ErrorCode::ComInitFailed: return L"ComInitFailed";
    case ErrorCode::DeviceEnumFailed: return L"DeviceEnumFailed";
    case ErrorCode::DeviceNotFound: return L"DeviceNotFound";
    case ErrorCode::DeviceActivateFailed: return L"DeviceActivateFailed";
    case ErrorCode::FormatUnsupported: return L"FormatUnsupported";
    case ErrorCode::StreamInitFailed: return L"StreamInitFailed";
    case ErrorCode::StreamStartFailed: return L"StreamStartFailed";
    case ErrorCode::StreamLost: return L"StreamLost";
    case ErrorCode::Timeout: return L"Timeout";
    case ErrorCode::Unknown: return L"Unknown";
  }
  return L"Unknown";
}

std::wstring format_hresult(long hr) {
  wchar_t* buffer = nullptr;
  const DWORD chars = ::FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, static_cast<DWORD>(hr), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);

  std::wstring text;
  if (chars != 0 && buffer != nullptr) {
    text.assign(buffer, chars);
    ::LocalFree(buffer);
  }
  // 去掉系统文案尾部的换行，避免日志出现空行
  while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ')) {
    text.pop_back();
  }
  return text;
}

Error Error::make(ErrorCode code, long hr, std::wstring message) {
  if (message.empty() && hr != 0) {
    message = format_hresult(hr);
  }
  return Error{code, static_cast<std::int32_t>(hr), std::move(message)};
}

}  // namespace ms::platform

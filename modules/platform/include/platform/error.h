#pragma once

#include <cstdint>
#include <string>

namespace ms::platform {

// 稳定的错误码：对外呈现文案由 ux::ErrorMapper 负责，这里只保证可分类、可记录。
enum class ErrorCode : std::int32_t {
  Ok = 0,

  ComInitFailed = 100,
  DeviceEnumFailed,
  DeviceNotFound,
  DeviceActivateFailed,
  FormatUnsupported,
  StreamInitFailed,
  StreamStartFailed,
  StreamLost,
  Timeout,

  Unknown = 999,
};

struct Error {
  ErrorCode code = ErrorCode::Ok;
  std::int32_t hresult = 0;
  std::wstring message;

  bool ok() const noexcept { return code == ErrorCode::Ok; }
  bool failed() const noexcept { return !ok(); }

  static Error make(ErrorCode code, long hr, std::wstring message = {});
};

const wchar_t* to_string(ErrorCode code);
std::wstring format_hresult(long hr);

}  // namespace ms::platform

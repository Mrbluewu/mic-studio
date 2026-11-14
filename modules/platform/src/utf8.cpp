#include "platform/utf8.h"

#include <windows.h>

namespace ms::platform {

std::string to_utf8(std::wstring_view text) {
  if (text.empty()) {
    return {};
  }
  const int bytes = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                          nullptr, 0, nullptr, nullptr);
  if (bytes <= 0) {
    return {};
  }
  std::string out(static_cast<std::size_t>(bytes), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), bytes,
                        nullptr, nullptr);
  return out;
}

std::wstring from_utf8(std::string_view text) {
  if (text.empty()) {
    return {};
  }
  const int chars = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                          nullptr, 0);
  if (chars <= 0) {
    return {};
  }
  std::wstring out(static_cast<std::size_t>(chars), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), chars);
  return out;
}

}  // namespace ms::platform

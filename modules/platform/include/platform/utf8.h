#pragma once

#include <string>
#include <string_view>

namespace ms::platform {

std::string to_utf8(std::wstring_view text);
std::wstring from_utf8(std::string_view text);

}  // namespace ms::platform

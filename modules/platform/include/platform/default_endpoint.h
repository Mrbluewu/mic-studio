#pragma once

#include <string>

namespace ms::platform {

// 把指定采集端点设为系统默认麦克风（通信与常规角色同时生效）。
// 返回设置前的默认端点 id；失败返回空字符串。
// Windows 没有公开 API，这里使用广泛使用的 IPolicyConfig（未文档化接口）。
std::wstring set_default_capture(const std::wstring& endpoint_id);

// 还原到之前记录的默认端点。成功返回 true。
bool restore_default_capture(const std::wstring& endpoint_id);

// 读取当前默认通信采集端点的 id。
std::wstring current_default_capture();

}  // namespace ms::platform

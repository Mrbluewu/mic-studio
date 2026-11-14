#pragma once

#include <string_view>

namespace ms::platform {

enum class LogLevel { Info, Warn, Error };

void set_log_level(LogLevel level);

// 只允许在非实时线程调用：内部有互斥量与系统调用。
void log(LogLevel level, std::string_view utf8_text);

inline void log_info(std::string_view text) { log(LogLevel::Info, text); }
inline void log_warn(std::string_view text) { log(LogLevel::Warn, text); }
inline void log_error(std::string_view text) { log(LogLevel::Error, text); }

}  // namespace ms::platform

#include "platform/log.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <mutex>

namespace {

std::mutex g_mutex;
ms::platform::LogLevel g_level = ms::platform::LogLevel::Info;

const char* level_label(ms::platform::LogLevel level) {
  switch (level) {
    case ms::platform::LogLevel::Info: return "INFO ";
    case ms::platform::LogLevel::Warn: return "WARN ";
    case ms::platform::LogLevel::Error: return "ERROR";
  }
  return "INFO ";
}

}  // namespace

namespace ms::platform {

void set_log_level(LogLevel level) { g_level = level; }

void log(LogLevel level, std::string_view utf8_text) {
  if (static_cast<int>(level) < static_cast<int>(g_level)) {
    return;
  }

  const auto now = std::chrono::system_clock::now();
  const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

  std::tm local{};
  localtime_s(&local, &seconds);

  char stamp[32]{};
  std::snprintf(stamp, sizeof(stamp), "%02d:%02d:%02d.%03d", local.tm_hour, local.tm_min, local.tm_sec,
                static_cast<int>(ms.count()));

  std::lock_guard<std::mutex> lock(g_mutex);
  std::cerr << '[' << stamp << "] " << level_label(level) << ' ' << utf8_text << '\n';
}

}  // namespace ms::platform

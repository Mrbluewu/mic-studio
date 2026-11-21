#pragma once

// 测试断言共用工具：所有测试可执行文件共用同一套计数与输出格式，
// 便于 CI 解析最后一行的 "<n> checks, <m> failed"。

#include <cstdio>

inline int g_checks = 0;
inline int g_failures = 0;

inline void check_impl(bool ok, const char* expr, int line) {
  ++g_checks;
  if (!ok) {
    ++g_failures;
    std::printf("FAIL %s:%d: %s\n", __FILE__, line, expr);
  }
}

#define CHECK(expr) check_impl((expr), #expr, __LINE__)

inline void begin(const char* name) { std::printf("-- %s\n", name); }

inline int report(const char* name) {
  std::printf("\n%s: %d checks, %d failed\n", name, g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}

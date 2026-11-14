# UI 模块单元测试：直接复用主程序的 UI 源文件，保证测的是同一份实现。
add_executable(micstudio_ui_tests
  ui_tests.cpp
  ${CMAKE_SOURCE_DIR}/apps/micstudio/ui_theme.cpp
  ${CMAKE_SOURCE_DIR}/apps/micstudio/ui_controls.cpp
)

target_include_directories(micstudio_ui_tests PRIVATE ${CMAKE_SOURCE_DIR}/apps/micstudio)

# 仅在测试目标中启用底噪条内部状态与离屏绘制钩子。
target_compile_definitions(micstudio_ui_tests PRIVATE MICSTUDIO_UI_TESTING
  MICSTUDIO_VERSION="${PROJECT_VERSION}")

target_link_libraries(micstudio_ui_tests PRIVATE
  ms_flags
  user32 gdi32 gdiplus comctl32 dwmapi uxtheme
)

add_test(NAME micstudio_ui_tests COMMAND micstudio_ui_tests)

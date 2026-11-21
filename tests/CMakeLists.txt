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

# 实时组件单元测试：环形缓冲、重采样器、时钟漂移控制器。
# 全部为纯计算逻辑，不需要音频设备，可随时运行。
add_executable(micstudio_dsp_core_tests dsp_core_tests.cpp)

target_include_directories(micstudio_dsp_core_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/tests
  ${CMAKE_SOURCE_DIR}/modules/engine/include
)

target_link_libraries(micstudio_dsp_core_tests PRIVATE
  ms_flags
  ms_dsp_core
  ms_ring_buffer
)

add_test(NAME micstudio_dsp_core_tests COMMAND micstudio_dsp_core_tests)

# 并发安全单测：只依赖头文件（TapBuffer、有界参数队列），不链接引擎，
# 这样可以在 CI 里快速跑满高速读写的竞争场景。
find_package(Threads REQUIRED)
add_executable(ms_concurrency_tests concurrency_tests.cpp)
target_include_directories(ms_concurrency_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/modules/engine/include
  ${CMAKE_SOURCE_DIR}/modules/dsp_core/include
)
target_link_libraries(ms_concurrency_tests PRIVATE ms_flags Threads::Threads)
add_test(NAME ms_concurrency_tests COMMAND ms_concurrency_tests)

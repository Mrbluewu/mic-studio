configure_file("${CMAKE_SOURCE_DIR}/assets/icons/mic-studio.ico"
  "${CMAKE_CURRENT_BINARY_DIR}/app.ico" COPYONLY)
configure_file(micstudio.rc.in "${CMAKE_CURRENT_BINARY_DIR}/micstudio.rc" @ONLY)

add_executable(micstudio WIN32
  main.cpp app_ui.cpp app_plugins.cpp app_window.cpp ui_theme.cpp ui_controls.cpp
  "${CMAKE_CURRENT_BINARY_DIR}/micstudio.rc")
# 资源脚本生成的 .rc 在构建目录里，resource.h 还在源码目录，两者都要能#include 到。
target_include_directories(micstudio PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(micstudio PRIVATE
  ms_engine ms_config ms_presets
  shell32 user32 gdi32 comctl32 comdlg32 gdiplus dwmapi uxtheme
)
# 页脚版本号直接取顶层 project(VERSION)，避免两处手写版本出现偏差。
target_compile_definitions(micstudio PRIVATE MICSTUDIO_VERSION="${PROJECT_VERSION}")

# 内置降噪后端随程序分发：DeepFilterNet.dll 是唯一的默认降噪实现。
set(MS_DEEPFILTER_DLL "${CMAKE_SOURCE_DIR}/third_party/deepfilternet/DeepFilterNet.dll")
if(NOT EXISTS "${MS_DEEPFILTER_DLL}")
  message(FATAL_ERROR "缺少内置降噪 DLL：${MS_DEEPFILTER_DLL}")
endif()
add_custom_command(TARGET micstudio POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${MS_DEEPFILTER_DLL}" "$<TARGET_FILE_DIR:micstudio>/DeepFilterNet.dll"
  COMMENT "复制 DeepFilterNet.dll")

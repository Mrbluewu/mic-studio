cmake_minimum_required(VERSION 3.24)
project(mic_studio VERSION 0.1.14 LANGUAGES C CXX)

# VST3 SDK 体积大且需要手动放置到 third_party/vst3sdk，默认开启但允许关闭，
# 以便干净克隆的环境（如 CI 首次构建）能先跑通主链路。
option(MS_ENABLE_VST3 "Build VST3 plugin hosting support" ON)

# MSVC 运行库静态链接：产物不依赖 vc_redist，安装包可直接分发。
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "构建类型" FORCE)
endif()

# 全局编译约定：UTF-8 源码、Windows 宽字符、严格的宏定义卫生
add_library(ms_flags INTERFACE)
target_compile_definitions(ms_flags INTERFACE
  UNICODE
  _UNICODE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
)
# /WX：把警告当错误。新代码不许引入警告；确属设计意图的（如 alignas 造成的
# 缓存行填充 C4324）在代码里用 #pragma warning(disable:...) 定点抑制，不用全局关。
target_compile_options(ms_flags INTERFACE
  $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX /permissive- /utf-8 /Zc:__cplusplus /EHsc>
)

add_subdirectory(modules/platform)
add_subdirectory(modules/ring_buffer)
add_subdirectory(modules/audio_io)
add_subdirectory(modules/dsp_core)
add_subdirectory(modules/analysis)
add_subdirectory(modules/vst2_host)
if(MS_ENABLE_VST3)
  add_subdirectory(modules/vst3_host)
endif()
add_subdirectory(modules/denoise)
add_subdirectory(modules/effects)
add_subdirectory(modules/presets)
add_subdirectory(modules/config)
add_subdirectory(modules/engine)
add_subdirectory(apps/engine-cli)
add_subdirectory(apps/micstudio)

enable_testing()
add_subdirectory(tests)

# 安装包脚本由模板生成，版本号统一取 PROJECT_VERSION，避免第二处手写值。
configure_file(packaging/installer.iss.in "${CMAKE_BINARY_DIR}/packaging/installer.iss" @ONLY)

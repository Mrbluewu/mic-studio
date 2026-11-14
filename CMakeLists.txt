cmake_minimum_required(VERSION 3.24)
project(mic_studio VERSION 0.1.13 LANGUAGES C CXX)

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
target_compile_options(ms_flags INTERFACE
  $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc>
)

add_subdirectory(modules/platform)
add_subdirectory(modules/ring_buffer)
add_subdirectory(modules/audio_io)
add_subdirectory(modules/dsp_core)
add_subdirectory(modules/analysis)
add_subdirectory(modules/vst2_host)
add_subdirectory(modules/vst3_host)
add_subdirectory(modules/denoise)
add_subdirectory(modules/effects)
add_subdirectory(modules/presets)
add_subdirectory(modules/config)
add_subdirectory(modules/engine)
add_subdirectory(apps/engine-cli)
add_subdirectory(apps/micstudio)

enable_testing()
add_subdirectory(tests)

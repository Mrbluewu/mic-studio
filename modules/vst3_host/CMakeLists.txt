# VST3 SDK 的宿主侧最小构建：只编译 base / pluginterfaces / hosting 所需源码，
# 不引入 SDK 的示例与全局 CMake 设置（避免改变主工程的输出目录）。
set(VST3_SDK_ROOT "${CMAKE_SOURCE_DIR}/third_party/vst3sdk")

add_library(ms_vst3_sdk STATIC
  ${VST3_SDK_ROOT}/pluginterfaces/base/conststringtable.cpp
  ${VST3_SDK_ROOT}/pluginterfaces/base/coreiids.cpp
  ${VST3_SDK_ROOT}/pluginterfaces/base/funknown.cpp
  ${VST3_SDK_ROOT}/pluginterfaces/base/ustring.cpp
  ${VST3_SDK_ROOT}/base/source/baseiids.cpp
  ${VST3_SDK_ROOT}/base/source/fbuffer.cpp
  ${VST3_SDK_ROOT}/base/source/fdebug.cpp
  ${VST3_SDK_ROOT}/base/source/fdynlib.cpp
  ${VST3_SDK_ROOT}/base/source/fobject.cpp
  ${VST3_SDK_ROOT}/base/source/fstreamer.cpp
  ${VST3_SDK_ROOT}/base/source/fstring.cpp
  ${VST3_SDK_ROOT}/base/source/timer.cpp
  ${VST3_SDK_ROOT}/base/source/updatehandler.cpp
  ${VST3_SDK_ROOT}/base/thread/source/fcondition.cpp
  ${VST3_SDK_ROOT}/base/thread/source/flock.cpp
  ${VST3_SDK_ROOT}/public.sdk/source/common/commoniids.cpp
  ${VST3_SDK_ROOT}/public.sdk/source/common/threadchecker_win32.cpp
  ${VST3_SDK_ROOT}/public.sdk/source/vst/utility/stringconvert.cpp
  ${VST3_SDK_ROOT}/public.sdk/source/vst/vstinitiids.cpp
  ${VST3_SDK_ROOT}/public.sdk/source/vst/hosting/connectionproxy.cpp
  ${VST3_SDK_ROOT}/public.sdk/source/vst/hosting/eventlist.cpp
  ${VST3_SDK_ROOT}/public.sdk/source/vst/hosting/hostclasses.cpp
  ${VST3_SDK_ROOT}/public.sdk/source/vst/hosting/module.cpp
  ${VST3_SDK_ROOT}/public.sdk/source/vst/hosting/module_win32.cpp
  ${VST3_SDK_ROOT}/public.sdk/source/vst/hosting/parameterchanges.cpp
  ${VST3_SDK_ROOT}/public.sdk/source/vst/hosting/pluginterfacesupport.cpp
  ${VST3_SDK_ROOT}/public.sdk/source/vst/hosting/processdata.cpp
)

target_include_directories(ms_vst3_sdk PUBLIC ${VST3_SDK_ROOT})
# SDK 3.7 的 module_win32.cpp 使用 generic_u8string()，在 C++20 下会变成 char8_t，
# 因此 SDK 自身按 C++17 编译；接口 ABI 与主工程的 C++20 完全兼容。
set_target_properties(ms_vst3_sdk PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_compile_definitions(ms_vst3_sdk PUBLIC
  NOMINMAX
  WIN32_LEAN_AND_MEAN
  $<$<CONFIG:Debug>:DEVELOPMENT=1>
  $<$<CONFIG:Release>:RELEASE=1>
)
target_link_libraries(ms_vst3_sdk PUBLIC ole32 shell32)

add_library(ms_vst3_host STATIC
  src/vst3_slot.cpp
)

target_include_directories(ms_vst3_host PUBLIC include)
target_link_libraries(ms_vst3_host
  PUBLIC ms_dsp_core ms_platform
  PRIVATE ms_vst3_sdk ms_flags ole32 shell32
)

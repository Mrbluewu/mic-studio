add_library(ms_engine STATIC
  src/audio_engine.cpp
)

target_include_directories(ms_engine PUBLIC include)
target_link_libraries(ms_engine
  PUBLIC ms_audio_io ms_dsp_core ms_ring_buffer ms_analysis ms_denoise ms_vst2_host ms_effects
  PRIVATE ms_flags
)

# VST3 宿主为可选依赖：关闭时 engine 侧用 MS_ENABLE_VST3 宏走降级分支，
# 加载 VST3 槽会直接报错而不是崩溃。
if(MS_ENABLE_VST3)
  target_link_libraries(ms_engine PUBLIC ms_vst3_host)
  target_compile_definitions(ms_engine PUBLIC MS_ENABLE_VST3=1)
endif()

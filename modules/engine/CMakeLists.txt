add_library(ms_engine STATIC
  src/audio_engine.cpp
)

target_include_directories(ms_engine PUBLIC include)
target_link_libraries(ms_engine
  PUBLIC ms_audio_io ms_dsp_core ms_ring_buffer ms_analysis ms_denoise ms_vst2_host ms_vst3_host ms_effects
  PRIVATE ms_flags
)

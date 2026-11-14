add_library(ms_dsp_core STATIC
  src/format_bridge.cpp
  src/basic_processors.cpp
)

target_include_directories(ms_dsp_core PUBLIC include)
target_link_libraries(ms_dsp_core
  PUBLIC ms_audio_io
  PRIVATE ms_flags
)

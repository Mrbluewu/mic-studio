add_library(ms_vst2_host STATIC
  src/vst2_slot.cpp
)

target_include_directories(ms_vst2_host PUBLIC include)
target_link_libraries(ms_vst2_host
  PUBLIC ms_dsp_core ms_platform
  PRIVATE ms_flags
)

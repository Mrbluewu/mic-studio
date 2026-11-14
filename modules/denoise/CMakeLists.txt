add_library(ms_denoise STATIC
  src/deepfilter_denoiser.cpp
)

target_include_directories(ms_denoise PUBLIC include)
target_link_libraries(ms_denoise
  PUBLIC ms_dsp_core ms_platform ms_vst2_host
  PRIVATE ms_flags
)

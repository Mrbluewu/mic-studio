add_library(ms_presets STATIC
  src/preset_library.cpp
)

target_include_directories(ms_presets PUBLIC include)
target_link_libraries(ms_presets PRIVATE ms_flags)

add_library(ms_config STATIC
  src/settings.cpp
)

target_include_directories(ms_config PUBLIC include)
target_link_libraries(ms_config PRIVATE ms_flags)

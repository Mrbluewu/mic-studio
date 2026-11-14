add_library(ms_platform STATIC
  src/error.cpp
  src/log.cpp
  src/rt_thread.cpp
  src/utf8.cpp
  src/default_endpoint.cpp
)

target_include_directories(ms_platform PUBLIC include)
target_link_libraries(ms_platform
  PUBLIC ms_flags
  PRIVATE ole32 avrt
)

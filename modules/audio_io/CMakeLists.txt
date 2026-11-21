add_library(ms_audio_io STATIC
  src/device_enumerator.cpp
  src/wasapi_session.cpp
  src/capture_stream.cpp
  src/render_stream.cpp
  src/endpoint_meter.cpp
  src/device_watcher.cpp
)

target_include_directories(ms_audio_io PUBLIC include)
target_link_libraries(ms_audio_io
  PUBLIC ms_platform
  PRIVATE ole32 oleaut32 uuid
)

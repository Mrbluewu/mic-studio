add_executable(engine-cli main.cpp)
target_link_libraries(engine-cli PRIVATE ms_engine ms_presets ms_config winmm)

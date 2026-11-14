add_library(ms_effects STATIC
  src/effect_cards.cpp
)

target_include_directories(ms_effects PUBLIC include)
target_link_libraries(ms_effects
  PUBLIC ms_dsp_core
  PRIVATE ms_flags
)

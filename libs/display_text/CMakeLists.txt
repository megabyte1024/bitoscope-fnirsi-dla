# SPDX-License-Identifier: MIT

add_library(bitoscope_display_text STATIC
    display_text.cpp
    display_text.h
)

target_include_directories(bitoscope_display_text PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(bitoscope_display_text PUBLIC cxx_std_23)
# WHY: MSVC needs /utf-8: display_text.cpp asserts that a narrow \u00B2 is two UTF-8 bytes.
target_compile_options(bitoscope_display_text PUBLIC $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
bitoscope_enable_warnings(bitoscope_display_text)

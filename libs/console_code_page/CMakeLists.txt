# SPDX-License-Identifier: MIT

add_library(bitoscope_console_code_page STATIC
    console_code_page.cpp
    console_code_page.h
)

target_include_directories(bitoscope_console_code_page PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(bitoscope_console_code_page PUBLIC cxx_std_23)
bitoscope_enable_warnings(bitoscope_console_code_page)

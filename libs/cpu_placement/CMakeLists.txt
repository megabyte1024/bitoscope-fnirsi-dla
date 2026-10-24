# SPDX-License-Identifier: MIT

add_library(bitoscope_cpu_placement STATIC
    decode_threads.cpp
    decode_threads.h
    work_placement.cpp
    work_placement.h
)

target_include_directories(bitoscope_cpu_placement PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(bitoscope_cpu_placement PUBLIC cxx_std_23)
bitoscope_enable_warnings(bitoscope_cpu_placement)

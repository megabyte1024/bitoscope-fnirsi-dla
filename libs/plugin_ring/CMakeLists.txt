# SPDX-License-Identifier: MIT

add_library(bitoscope_plugin_ring STATIC
    ring_encoding.cpp
)

target_include_directories(bitoscope_plugin_ring PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(bitoscope_plugin_ring PUBLIC cxx_std_23)
bitoscope_enable_warnings(bitoscope_plugin_ring)

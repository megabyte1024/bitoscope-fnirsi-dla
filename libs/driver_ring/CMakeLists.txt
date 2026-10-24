# SPDX-License-Identifier: MIT

add_library(bitoscope_driver_ring STATIC
    ring_writer.cpp
)

target_include_directories(bitoscope_driver_ring PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(bitoscope_driver_ring PUBLIC
    bitoscope_plugin_ring
    bitoscope_bulk_ring
)

target_compile_features(bitoscope_driver_ring PUBLIC cxx_std_23)
bitoscope_enable_warnings(bitoscope_driver_ring)

# SPDX-License-Identifier: MIT

add_library(bitoscope_device_properties STATIC
    device_properties.cpp
    device_properties.h
)

target_include_directories(bitoscope_device_properties PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(bitoscope_device_properties PUBLIC bitoscope_plugin_ipc)
target_compile_features(bitoscope_device_properties PUBLIC cxx_std_23)
bitoscope_enable_warnings(bitoscope_device_properties)

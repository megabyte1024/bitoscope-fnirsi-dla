# SPDX-License-Identifier: MIT

add_library(bitoscope_cli_drivers STATIC
    cli_drivers.cpp
)

target_include_directories(bitoscope_cli_drivers PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(bitoscope_cli_drivers PRIVATE bitoscope_display_text)
target_link_libraries(bitoscope_cli_drivers PUBLIC
    bitoscope_plugin_host
    bitoscope_pipe_clients
    bitoscope_device_properties
)
target_compile_features(bitoscope_cli_drivers PUBLIC cxx_std_23)
bitoscope_enable_warnings(bitoscope_cli_drivers)

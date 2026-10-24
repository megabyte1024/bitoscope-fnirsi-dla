# SPDX-License-Identifier: MIT

add_library(bitoscope_cli_plugins STATIC
    cli_plugins.cpp
)

target_include_directories(bitoscope_cli_plugins PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(bitoscope_cli_plugins PRIVATE bitoscope_display_text)
target_link_libraries(bitoscope_cli_plugins PUBLIC
    bitoscope_plugin_host
    bitoscope_pipe_clients
    bitoscope_device_properties
)
target_compile_features(bitoscope_cli_plugins PUBLIC cxx_std_23)
bitoscope_enable_warnings(bitoscope_cli_plugins)

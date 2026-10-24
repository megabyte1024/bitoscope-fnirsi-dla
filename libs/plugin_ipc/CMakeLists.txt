# SPDX-License-Identifier: MIT

add_library(bitoscope_plugin_ipc STATIC
    ipc_capnp_boundary.cpp
    ipc_frame.cpp
    ipc_memory_stream.cpp
    ipc_messages.cpp
    ipc_properties.cpp
    plugin_version.cpp
)

target_include_directories(bitoscope_plugin_ipc PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(bitoscope_plugin_ipc PRIVATE bitoscope_capnp)

target_compile_features(bitoscope_plugin_ipc PUBLIC cxx_std_23)
bitoscope_enable_warnings(bitoscope_plugin_ipc)

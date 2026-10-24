# SPDX-License-Identifier: MIT

add_library(bitoscope_driver_ipc STATIC
    ipc_la_acquisition.cpp
    ipc_signal_generator.cpp
)

target_include_directories(bitoscope_driver_ipc PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(bitoscope_driver_ipc
    PUBLIC bitoscope_plugin_ipc
    PRIVATE bitoscope_driver_capnp
)

target_compile_features(bitoscope_driver_ipc PUBLIC cxx_std_23)
bitoscope_enable_warnings(bitoscope_driver_ipc)

# SPDX-License-Identifier: MIT

add_library(bitoscope_plugin_c_sdk STATIC
    src/common_api.cpp
    src/declared_abbreviations.cpp
    src/facet_values.cpp
    src/frame_output.cpp
    src/plugin_session.cpp
    src/private_stdio.cpp
)

target_include_directories(bitoscope_plugin_c_sdk
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(bitoscope_plugin_c_sdk PRIVATE
    bitoscope_plugin_ipc
    bitoscope_device_properties
    bitoscope_display_text
)

find_package(Threads REQUIRED)
target_link_libraries(bitoscope_plugin_c_sdk PRIVATE Threads::Threads)

target_compile_features(bitoscope_plugin_c_sdk PUBLIC cxx_std_23)
bitoscope_enable_warnings(bitoscope_plugin_c_sdk)

add_library(bitoscope_plugin_c_sdk_internal INTERFACE)
target_include_directories(bitoscope_plugin_c_sdk_internal INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(bitoscope_plugin_c_sdk_internal INTERFACE bitoscope_plugin_c_sdk bitoscope_plugin_ipc)

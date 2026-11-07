# SPDX-License-Identifier: MIT

add_library(bitoscope_driver_c_sdk STATIC
    src/c_api.cpp
    src/cpp_api.cpp
    src/cpp_twins.cpp
    src/device_bytes.cpp
    src/device_finder.cpp
    src/device_location.cpp
    src/device_source.cpp
    src/device_watch.cpp
    src/driver_calls.cpp
    src/driver_description.cpp
    src/generic_facet.cpp
    src/driver_source.cpp
    src/facet_properties.cpp
    src/identity_cache.cpp
    src/la_runtime.cpp
    src/signal_generator_facet.cpp
)

target_include_directories(bitoscope_driver_c_sdk
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(bitoscope_driver_c_sdk
    PUBLIC
        bitoscope_plugin_c_sdk
    PRIVATE
        bitoscope_plugin_c_sdk_internal
        bitoscope_driver_ipc
        bitoscope_driver_ring
        bitoscope_device_properties
        bitoscope_display_text
)

find_package(Threads REQUIRED)
target_link_libraries(bitoscope_driver_c_sdk PRIVATE Threads::Threads)

target_compile_features(bitoscope_driver_c_sdk PUBLIC cxx_std_23)
bitoscope_enable_warnings(bitoscope_driver_c_sdk)

add_library(bitoscope_driver_c_sdk_internal INTERFACE)
target_include_directories(bitoscope_driver_c_sdk_internal INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(bitoscope_driver_c_sdk_internal INTERFACE
    bitoscope_driver_c_sdk
    bitoscope_plugin_c_sdk_internal
    bitoscope_driver_ipc
    bitoscope_driver_ring
)

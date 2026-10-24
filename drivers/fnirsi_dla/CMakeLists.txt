# SPDX-License-Identifier: MIT

find_package(Threads REQUIRED)

add_library(fnirsi_protocol STATIC fnirsi_protocol.cpp)
target_include_directories(fnirsi_protocol PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(fnirsi_protocol PUBLIC cxx_std_23)
bitoscope_enable_warnings(fnirsi_protocol)

add_library(fnirsi_usb_transfer_config STATIC usb_transfer_config.cpp)
target_include_directories(fnirsi_usb_transfer_config PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(fnirsi_usb_transfer_config PUBLIC cxx_std_23)
bitoscope_enable_warnings(fnirsi_usb_transfer_config)

add_library(fnirsi_channel_padding STATIC channel_padding.cpp)
target_include_directories(fnirsi_channel_padding PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(fnirsi_channel_padding PUBLIC cxx_std_23)
bitoscope_enable_warnings(fnirsi_channel_padding)

add_library(fnirsi_data_rate_limit STATIC data_rate_limit.cpp)
target_include_directories(fnirsi_data_rate_limit PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fnirsi_data_rate_limit PRIVATE fnirsi_protocol fnirsi_channel_padding)
target_compile_features(fnirsi_data_rate_limit PUBLIC cxx_std_23)
bitoscope_enable_warnings(fnirsi_data_rate_limit)

add_library(fnirsi_connect_sequence STATIC connect_sequence.cpp)
target_include_directories(fnirsi_connect_sequence PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fnirsi_connect_sequence PUBLIC fnirsi_protocol)
target_compile_features(fnirsi_connect_sequence PUBLIC cxx_std_23)
bitoscope_enable_warnings(fnirsi_connect_sequence)

add_library(fnirsi_usb STATIC fnirsi_device.cpp)
target_include_directories(fnirsi_usb PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fnirsi_usb PUBLIC fnirsi_protocol fnirsi_connect_sequence bitoscope_driver_c_sdk)
target_compile_features(fnirsi_usb PUBLIC cxx_std_23)
bitoscope_enable_warnings(fnirsi_usb)

add_library(fnirsi_dla_facets STATIC dla_analyzer.cpp dla_signal_generator.cpp dla_plugin.cpp)
target_include_directories(fnirsi_dla_facets PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fnirsi_dla_facets
    PUBLIC bitoscope_driver_c_sdk fnirsi_protocol fnirsi_usb_transfer_config
    PRIVATE fnirsi_channel_padding fnirsi_data_rate_limit Threads::Threads)
target_compile_features(fnirsi_dla_facets PUBLIC cxx_std_23)
bitoscope_enable_warnings(fnirsi_dla_facets)

add_library(fnirsi_dla_usb STATIC dla_usb.cpp)
target_link_libraries(fnirsi_dla_usb PUBLIC fnirsi_dla_facets PRIVATE fnirsi_usb)
target_compile_features(fnirsi_dla_usb PUBLIC cxx_std_23)
bitoscope_enable_warnings(fnirsi_dla_usb)

add_executable(fnirsi_dla main.cpp)
target_link_libraries(fnirsi_dla PRIVATE fnirsi_dla_usb)
bitoscope_plugin_executable(fnirsi_dla)
target_compile_features(fnirsi_dla PRIVATE cxx_std_23)
bitoscope_enable_warnings(fnirsi_dla)

add_library(fnirsi_pwm_check STATIC tools/pwm_check.cpp tools/pwm_synth.cpp tools/soak_plan.cpp tools/run_segments.cpp)
target_include_directories(fnirsi_pwm_check PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/tools)
target_link_libraries(fnirsi_pwm_check PUBLIC bitoscope_bulk_ring PRIVATE fnirsi_protocol bitoscope_display_text Threads::Threads)
target_compile_features(fnirsi_pwm_check PUBLIC cxx_std_23)
bitoscope_enable_warnings(fnirsi_pwm_check)

add_library(fnirsi_dla_tool_session STATIC tools/dla_session.cpp tools/ring_reader.cpp)
target_include_directories(fnirsi_dla_tool_session PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/tools)
target_link_libraries(fnirsi_dla_tool_session PUBLIC
    bitoscope_cli_plugins
    bitoscope_bulk_ring
    fnirsi_pwm_check
)
target_compile_features(fnirsi_dla_tool_session PUBLIC cxx_std_23)
bitoscope_enable_warnings(fnirsi_dla_tool_session)

add_executable(fnirsi_dla_loopback_check tools/loopback_check.cpp)
target_link_libraries(fnirsi_dla_loopback_check PRIVATE
    fnirsi_dla_tool_session
    fnirsi_protocol
)
add_dependencies(fnirsi_dla_loopback_check fnirsi_dla)
target_compile_definitions(fnirsi_dla_loopback_check PRIVATE
    BITOSCOPE_DEFAULT_PLUGIN_DIR="$<TARGET_FILE_DIR:fnirsi_dla>"
)
target_compile_features(fnirsi_dla_loopback_check PRIVATE cxx_std_23)
bitoscope_enable_warnings(fnirsi_dla_loopback_check)

add_executable(fnirsi_dla_max_throughput_soak_test tools/max_throughput_soak_test.cpp)
target_link_libraries(fnirsi_dla_max_throughput_soak_test PRIVATE
    fnirsi_dla_tool_session
    fnirsi_protocol
    fnirsi_pwm_check
    bitoscope_display_text
    bitoscope_console_code_page
)
add_dependencies(fnirsi_dla_max_throughput_soak_test fnirsi_dla)
target_compile_definitions(fnirsi_dla_max_throughput_soak_test PRIVATE
    BITOSCOPE_DEFAULT_PLUGIN_DIR="$<TARGET_FILE_DIR:fnirsi_dla>"
)
target_compile_features(fnirsi_dla_max_throughput_soak_test PRIVATE cxx_std_23)
bitoscope_enable_warnings(fnirsi_dla_max_throughput_soak_test)

add_library(fnirsi_raw_lanes STATIC tools/raw_lanes.cpp)
target_include_directories(fnirsi_raw_lanes PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/tools)
target_compile_features(fnirsi_raw_lanes PUBLIC cxx_std_23)
bitoscope_enable_warnings(fnirsi_raw_lanes)

add_executable(fnirsi_dla_raw_probe tools/raw_probe.cpp)
target_link_libraries(fnirsi_dla_raw_probe PRIVATE fnirsi_usb fnirsi_raw_lanes fnirsi_pwm_check
                                                     fnirsi_protocol bitoscope_console_code_page bitoscope_driver_usb)
target_compile_features(fnirsi_dla_raw_probe PRIVATE cxx_std_23)
bitoscope_enable_warnings(fnirsi_dla_raw_probe)

add_executable(fnirsi_dla_pwm_check_throughput tools/pwm_check_throughput.cpp)
target_link_libraries(fnirsi_dla_pwm_check_throughput PRIVATE fnirsi_pwm_check)
target_compile_features(fnirsi_dla_pwm_check_throughput PRIVATE cxx_std_23)
bitoscope_enable_warnings(fnirsi_dla_pwm_check_throughput)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(fnirsi_dla_usb_held_check tools/usb_held_check.cpp)
  target_link_libraries(fnirsi_dla_usb_held_check PRIVATE fnirsi_protocol bitoscope_libusb)
  target_compile_features(fnirsi_dla_usb_held_check PRIVATE cxx_std_23)
  bitoscope_enable_warnings(fnirsi_dla_usb_held_check)
endif()

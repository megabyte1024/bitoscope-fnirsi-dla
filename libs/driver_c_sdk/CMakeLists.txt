# SPDX-License-Identifier: MIT

add_library(bitoscope_driver_c_sdk STATIC
    src/c_api.cpp
    src/cpp_api.cpp
    src/cpp_twins.cpp
    src/device_change_source.cpp
    src/device_bytes.cpp
    src/device_location.cpp
    src/device_source.cpp
    src/device_watch.cpp
    src/driver_calls.cpp
    src/driver_description.cpp
    src/driver_plugin.cpp
    src/driver_source.cpp
    src/facet_properties.cpp
    src/identity_cache.cpp
    src/la_runtime.cpp
    src/signal_generator_facet.cpp
    src/usb_device_api.cpp
    src/usb_source.cpp
    src/usb_stream.cpp
    src/usb_stream_properties.cpp
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_sources(bitoscope_driver_c_sdk PRIVATE src/linux_uevent_reader.cpp)
endif()

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
        bitoscope_driver_usb
        bitoscope_device_properties
        bitoscope_display_text
)

find_package(Threads REQUIRED)
target_link_libraries(bitoscope_driver_c_sdk PRIVATE Threads::Threads)
if(WIN32)
  target_link_libraries(bitoscope_driver_c_sdk PRIVATE cfgmgr32)
endif()

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

if(NOT TARGET bitoscope_libusb)
  add_library(bitoscope_libusb INTERFACE)
  # WHY: vcpkg's libusb port ships no CMake config and MSVC has no pkg-config, so find_path/find_library.
  if(MSVC)
    find_path(LIBUSB_INCLUDE_DIR NAMES libusb.h PATH_SUFFIXES libusb-1.0)
    find_library(LIBUSB_LIBRARY NAMES libusb-1.0)
    if(NOT LIBUSB_INCLUDE_DIR OR NOT LIBUSB_LIBRARY)
      message(FATAL_ERROR
        "libusb-1.0 not found for MSVC: install it with vcpkg (vcpkg install libusb:x64-windows) and configure "
        "with -DCMAKE_TOOLCHAIN_FILE=<vcpkg-root>/scripts/buildsystems/vcpkg.cmake, or set LIBUSB_INCLUDE_DIR "
        "and LIBUSB_LIBRARY")
    endif()
    target_include_directories(bitoscope_libusb SYSTEM INTERFACE ${LIBUSB_INCLUDE_DIR})
    target_link_libraries(bitoscope_libusb INTERFACE ${LIBUSB_LIBRARY})
  else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(BITOSCOPE_LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)
    target_link_libraries(bitoscope_libusb INTERFACE PkgConfig::BITOSCOPE_LIBUSB)
  endif()
endif()

add_library(bitoscope_driver_usb STATIC
    src/usb_inventory.cpp
    src/usb_checks.cpp
    src/usb_device.cpp
    src/libusb_bus.cpp
    src/libusb_stream_transport.cpp
)
target_include_directories(bitoscope_driver_usb PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(bitoscope_driver_usb PRIVATE bitoscope_libusb)
target_compile_features(bitoscope_driver_usb PUBLIC cxx_std_23)
bitoscope_enable_warnings(bitoscope_driver_usb)

# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.24)

project(bitoscope_fnirsi_dla VERSION 0.1.0 LANGUAGES CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(EXISTS "${CMAKE_SOURCE_DIR}/private/CMakeLists.txt")
  set(BITOSCOPE_PRIVATE_TREE ON)
else()
  set(BITOSCOPE_PRIVATE_TREE OFF)
endif()

include(cmake/BitoscopeDeps.cmake)

include(cmake/BitoscopeOwnTargetWarnings.cmake)

include(cmake/BitoscopeBuildJobs.cmake)

find_package(Threads REQUIRED)

function(bitoscope_enable_warnings target)
  # WHY: C4324 only reports padding an alignas asks for, GCC and Clang have no such warning (owner, 2026-10-21).
  target_compile_options(${target} PRIVATE
      $<$<CXX_COMPILER_ID:MSVC>:/W4;/wd4324>
      $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall;-Wextra;-Wpedantic>
  )
endfunction()

get_property(BITOSCOPE_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(BITOSCOPE_MULTI_CONFIG)
  set(BITOSCOPE_PLUGIN_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
else()
  set(BITOSCOPE_PLUGIN_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin")
endif()

function(bitoscope_plugin_executable target)
  set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${BITOSCOPE_PLUGIN_OUTPUT_DIR}")
endfunction()

if(BITOSCOPE_PRIVATE_TREE)
  # WHY: Here, not in private dir: ctest reads test files from the root down, and dirs added before write none.
  enable_testing()
endif()

add_subdirectory(proto/capnp)
add_subdirectory(proto/driver_capnp)
add_subdirectory(libs/display_text)
add_subdirectory(libs/console_code_page)
add_subdirectory(libs/plugin_ipc)
add_subdirectory(libs/driver_ipc)
add_subdirectory(libs/device_properties)
add_subdirectory(libs/bulk_ring)
add_subdirectory(libs/plugin_ring)
add_subdirectory(libs/driver_ring)
add_subdirectory(libs/plugin_c_sdk)
add_subdirectory(libs/driver_c_sdk)
add_subdirectory(libs/cpu_placement)
add_subdirectory(libs/core)
add_subdirectory(cli)
add_subdirectory(drivers/fnirsi_dla)

install(TARGETS fnirsi_dla RUNTIME DESTINATION drivers)

if(BITOSCOPE_PRIVATE_TREE)
  add_subdirectory(private)
endif()

# WHY: Last, after every add_subdirectory: a target added below this line would get no warnings.
bitoscope_enable_warnings_on_own_targets("${CMAKE_SOURCE_DIR}")

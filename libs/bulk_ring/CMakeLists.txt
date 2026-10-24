# SPDX-License-Identifier: MIT

add_library(bitoscope_bulk_ring STATIC
    bulk_ring.cpp
)

target_include_directories(bitoscope_bulk_ring PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(bitoscope_bulk_ring PUBLIC cxx_std_23)
bitoscope_enable_warnings(bitoscope_bulk_ring)

# WHY: Link librt: glibc before 2.34 keeps shm_open and shm_unlink there.
if(UNIX AND NOT APPLE)
  target_link_libraries(bitoscope_bulk_ring PRIVATE rt)
endif()

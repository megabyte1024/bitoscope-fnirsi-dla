# SPDX-License-Identifier: MIT

set(CAPNPC_SRC_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}")
set(CAPNPC_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${CAPNPC_OUTPUT_DIR}")

capnp_generate_cpp(_bitoscope_capnp_srcs _bitoscope_capnp_hdrs
    envelope.capnp
    properties.capnp
)

add_library(bitoscope_capnp STATIC ${_bitoscope_capnp_srcs} ${_bitoscope_capnp_hdrs})
target_include_directories(bitoscope_capnp PUBLIC "${CAPNPC_OUTPUT_DIR}")
target_link_libraries(bitoscope_capnp PUBLIC CapnProto::capnp)
target_compile_features(bitoscope_capnp PUBLIC cxx_std_23)

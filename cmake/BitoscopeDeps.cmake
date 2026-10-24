# SPDX-License-Identifier: MIT

# WHY: Each configuration of a build folder needs its own deps: MSVC cannot link Debug against Release.

set(_bitoscope_deps_dir_help
    "Install prefix of the third-party libraries built by third_party/CMakeLists.txt (the deps build)")
set(BITOSCOPE_DEPS_DIR "${CMAKE_SOURCE_DIR}/deps" CACHE PATH "${_bitoscope_deps_dir_help}")
# WHY: An empty value is reset: a build dir configured before deps/ existed cached it empty, and CACHE never overwrites.
if(NOT BITOSCOPE_DEPS_DIR)
  set(BITOSCOPE_DEPS_DIR "${CMAKE_SOURCE_DIR}/deps" CACHE PATH "${_bitoscope_deps_dir_help}" FORCE)
endif()

set(_bitoscope_deps_howto
  "Build third-party libraries once, then configure again:\n"
  "  cmake -S third_party -B build-deps -DCMAKE_INSTALL_PREFIX=${BITOSCOPE_DEPS_DIR} -DCMAKE_BUILD_TYPE=Release\n"
  "  cmake --build build-deps --target install\n"
  "Debug build of project needs Debug deps too (own build directory, -DCMAKE_BUILD_TYPE=Debug).\n"
  "Visual Studio: one build directory, built with --config Debug and --config Release;\n"
  "a Release-only build directory (-DCMAKE_CONFIGURATION_TYPES=Release) needs --config Release only.")

set(BITOSCOPE_DEPS_PREFIX "")
foreach(_candidate IN LISTS CMAKE_PREFIX_PATH BITOSCOPE_DEPS_DIR)
  if(EXISTS "${_candidate}/bitoscope_deps.cmake")
    get_filename_component(BITOSCOPE_DEPS_PREFIX "${_candidate}" ABSOLUTE)
    break()
  endif()
endforeach()
if(NOT BITOSCOPE_DEPS_PREFIX)
  string(JOIN "" _howto ${_bitoscope_deps_howto})
  message(FATAL_ERROR
    "The third-party libraries (Cap'n Proto) are not built: no "
    "bitoscope_deps.cmake in ${BITOSCOPE_DEPS_DIR} or in CMAKE_PREFIX_PATH.\n${_howto}")
endif()
message(STATUS "Third-party libraries (deps build): ${BITOSCOPE_DEPS_PREFIX}")
list(PREPEND CMAKE_PREFIX_PATH "${BITOSCOPE_DEPS_PREFIX}")

include("${BITOSCOPE_DEPS_PREFIX}/bitoscope_deps.cmake")

find_package(Git QUIET)
function(bitoscope_check_deps_commits source_root)
  if(NOT GIT_EXECUTABLE)
    return()
  endif()
  file(RELATIVE_PATH _shown_root "${CMAKE_SOURCE_DIR}" "${source_root}")
  set(_stale "")
  foreach(_sub IN LISTS ARGN)
    set(_built "${BITOSCOPE_DEPS_COMMIT_${_sub}}")
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" -C "${source_root}/${_sub}" rev-parse HEAD
      OUTPUT_VARIABLE _now OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE _result)
    if(_result EQUAL 0 AND _built AND NOT _built STREQUAL _now)
      string(APPEND _stale "  ${_shown_root}/${_sub}: deps built from ${_built}, checkout is at ${_now}\n")
    endif()
  endforeach()
  if(_stale)
    string(JOIN "" _howto ${_bitoscope_deps_howto})
    message(FATAL_ERROR
      "${BITOSCOPE_DEPS_PREFIX} was built from other submodule commits than this checkout has:\n"
      "${_stale}Rebuild it. ${_howto}")
  endif()
endfunction()
bitoscope_check_deps_commits("${CMAKE_SOURCE_DIR}/third_party" capnproto)

# WHY: Map RelWithDebInfo and MinSizeRel to Release: otherwise CMake may pick Debug libraries.
set(CMAKE_MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release)
set(CMAKE_MAP_IMPORTED_CONFIG_MINSIZEREL Release)

get_property(_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(_multi_config)
  set(_needed "")
  foreach(_type IN LISTS CMAKE_CONFIGURATION_TYPES)
    if(_type STREQUAL "Debug")
      list(APPEND _needed DEBUG)
    else()
      list(APPEND _needed RELEASE)
    endif()
  endforeach()
  list(REMOVE_DUPLICATES _needed)
elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(_needed DEBUG)
else()
  set(_needed RELEASE)
endif()

# WHY: A macro, not a function: a package's config file sets variables (CMAKE_MODULE_PATH) its caller needs.
macro(bitoscope_require_deps)
  cmake_parse_arguments(_bitoscope_require "" "" "PACKAGES;SUBMODULES;TARGETS" ${ARGN})

  set(_missing "")
  foreach(_sub IN LISTS _bitoscope_require_SUBMODULES)
    if(NOT _sub IN_LIST BITOSCOPE_DEPS_SUBMODULES)
      string(APPEND _missing " ${_sub}")
    endif()
  endforeach()
  if(_missing)
    string(JOIN "" _howto ${_bitoscope_deps_howto})
    message(FATAL_ERROR
      "${BITOSCOPE_DEPS_PREFIX} was installed before these libraries were added to the deps build:"
      "${_missing}. Rebuild it. ${_howto}")
  endif()

  foreach(_pkg IN LISTS _bitoscope_require_PACKAGES)
    find_package(${_pkg} CONFIG REQUIRED)
    get_filename_component(_found "${${_pkg}_DIR}" ABSOLUTE)
    string(FIND "${_found}/" "${BITOSCOPE_DEPS_PREFIX}/" _at)
    if(NOT _at EQUAL 0)
      message(FATAL_ERROR
        "${_pkg} was found at ${_found}, outside the deps build (${BITOSCOPE_DEPS_PREFIX}). The project "
        "builds only against its pinned submodules; remove that install from the search path.")
    endif()
  endforeach()

  foreach(_target IN LISTS _bitoscope_require_TARGETS)
    if(NOT TARGET ${_target})
      message(FATAL_ERROR "The deps build in ${BITOSCOPE_DEPS_PREFIX} has no target ${_target}.")
    endif()
    get_target_property(_configs ${_target} IMPORTED_CONFIGURATIONS)
    foreach(_config IN LISTS _needed)
      if(NOT _config IN_LIST _configs)
        string(JOIN "" _howto ${_bitoscope_deps_howto})
        message(FATAL_ERROR
          "${_target} in ${BITOSCOPE_DEPS_PREFIX} has no ${_config} build (it has: ${_configs}). "
          "This build directory needs: ${_needed}. ${_howto}")
      endif()
    endforeach()
  endforeach()
endmacro()

bitoscope_require_deps(PACKAGES CapnProto SUBMODULES capnproto TARGETS CapnProto::kj CapnProto::capnp)

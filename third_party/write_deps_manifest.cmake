# SPDX-License-Identifier: MIT

# Run at install time by third_party/CMakeLists.txt (install(CODE)): writes deps/bitoscope_deps.cmake,
# the marker the project build looks for and the record of which submodule commit each installed
# library came from. Inputs: BITOSCOPE_DEPS_SOURCE_DIRS (the directories holding the submodules,
# third_party/ first; the first one that has a submodule's directory is read),
# BITOSCOPE_DEPS_BUILT_SUBMODULES (the submodules this deps build built) and BITOSCOPE_DEPS_MANIFEST
# (the file to write). The project build reads it (cmake/BitoscopeDeps.cmake).
#
# A commit that cannot be read (no git, or a source tree that is not a git checkout) is written as
# an empty string, and the project build then skips the comparison for that library.

set(_submodules ${BITOSCOPE_DEPS_BUILT_SUBMODULES})

find_program(_git git)
set(_text "# Written by third_party/CMakeLists.txt when the deps build was installed. Do not edit.\n")
string(APPEND _text "# Read by the project build (cmake/BitoscopeDeps.cmake).\n")
string(APPEND _text "set(BITOSCOPE_DEPS_SUBMODULES \"${_submodules}\")\n")
foreach(_sub IN LISTS _submodules)
  set(_commit "")
  set(_sub_dir "")
  foreach(_root IN LISTS BITOSCOPE_DEPS_SOURCE_DIRS)
    if(NOT _sub_dir AND EXISTS "${_root}/${_sub}")
      set(_sub_dir "${_root}/${_sub}")
    endif()
  endforeach()
  if(_git AND _sub_dir)
    execute_process(
      COMMAND "${_git}" -C "${_sub_dir}" rev-parse HEAD
      OUTPUT_VARIABLE _commit
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET
      RESULT_VARIABLE _result)
    if(NOT _result EQUAL 0)
      set(_commit "")
    endif()
  endif()
  string(APPEND _text "set(BITOSCOPE_DEPS_COMMIT_${_sub} \"${_commit}\")\n")
endforeach()

message(STATUS "Writing: ${BITOSCOPE_DEPS_MANIFEST}")
file(WRITE "${BITOSCOPE_DEPS_MANIFEST}" "${_text}")

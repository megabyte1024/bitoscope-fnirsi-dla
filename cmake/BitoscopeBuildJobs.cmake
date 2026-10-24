# SPDX-License-Identifier: MIT

cmake_host_system_information(RESULT BITOSCOPE_PHYSICAL_CORES QUERY NUMBER_OF_PHYSICAL_CORES)
if(NOT BITOSCOPE_PHYSICAL_CORES GREATER 0)
  set(BITOSCOPE_PHYSICAL_CORES 1)
endif()

if(CMAKE_SCRIPT_MODE_FILE)
  execute_process(COMMAND "${CMAKE_COMMAND}" -E echo "${BITOSCOPE_PHYSICAL_CORES}")
  return()
endif()

set(BITOSCOPE_BUILD_JOBS "${BITOSCOPE_PHYSICAL_CORES}" CACHE STRING
    "Most compile and link jobs at once (Ninja); at most the number of physical CPU cores")
if(NOT BITOSCOPE_BUILD_JOBS MATCHES "^[0-9]+$" OR BITOSCOPE_BUILD_JOBS LESS 1
   OR BITOSCOPE_BUILD_JOBS GREATER BITOSCOPE_PHYSICAL_CORES)
  message(WARNING "BITOSCOPE_BUILD_JOBS=${BITOSCOPE_BUILD_JOBS} is not between 1 and "
                  "${BITOSCOPE_PHYSICAL_CORES} physical CPU cores; ${BITOSCOPE_PHYSICAL_CORES} used")
  set(BITOSCOPE_BUILD_JOBS "${BITOSCOPE_PHYSICAL_CORES}" CACHE STRING
      "Most compile and link jobs at once (Ninja); at most the number of physical CPU cores" FORCE)
endif()

set_property(GLOBAL APPEND PROPERTY JOB_POOLS bitoscope_build_jobs=${BITOSCOPE_BUILD_JOBS})
set(CMAKE_JOB_POOL_COMPILE bitoscope_build_jobs)
set(CMAKE_JOB_POOL_LINK bitoscope_build_jobs)

# WHY: MSBuild compiles one file at a time; /MP<jobs> with one project at a time keeps to core limit.
if(CMAKE_GENERATOR MATCHES "^Visual Studio")
  add_compile_options("$<$<OR:$<COMPILE_LANG_AND_ID:C,MSVC>,$<COMPILE_LANG_AND_ID:CXX,MSVC>>:/MP${BITOSCOPE_BUILD_JOBS}>")
endif()

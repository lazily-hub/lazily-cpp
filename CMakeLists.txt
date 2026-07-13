cmake_minimum_required(VERSION 3.16)

project(lazily-cpp
VERSION 0.15.0
  LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default to Release build for the whole project unless overridden.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(LAZILY_BUILD_TESTS "Build lazily-cpp tests" ON)
option(LAZILY_BUILD_BENCHMARKS "Build lazily-cpp benchmarks" OFF)
option(LAZILY_BUILD_FFI "Build the C-ABI FFI shared library" ON)

add_library(lazily INTERFACE)
target_include_directories(lazily
  INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(lazily INTERFACE cxx_std_17)
# transport.hpp's ShmBackend (POSIX shm_open/mmap, Linux) needs _GNU_SOURCE for
# the shm_* declarations. Propagate it on Linux so consumers using the umbrella
# header compile cleanly (additive macro, standard for Linux C++ libs).
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_compile_definitions(lazily INTERFACE _GNU_SOURCE)
endif()

if(LAZILY_BUILD_FFI)
  add_library(lazily_ffi SHARED
    src/ffi.cpp
  )
  target_link_libraries(lazily_ffi PUBLIC lazily)
  set_target_properties(lazily_ffi PROPERTIES
    POSITION_INDEPENDENT_CODE ON
  )
endif()

if(LAZILY_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(LAZILY_BUILD_BENCHMARKS)
  add_subdirectory(benches)
endif()

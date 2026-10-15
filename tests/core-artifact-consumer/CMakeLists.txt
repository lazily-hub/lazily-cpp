cmake_minimum_required(VERSION 3.16)
project(lazily_core_consumer LANGUAGES CXX)

if(NOT IS_DIRECTORY "${LAZILY_ARTIFACT}/include")
  message(FATAL_ERROR "LAZILY_ARTIFACT must name an extracted lazily-cpp artifact")
endif()

add_executable(lazily_core_consumer main.cpp)
set(LAZILY_CXX_STANDARD 17 CACHE STRING "C++ standard for the consumer")
set_property(TARGET lazily_core_consumer PROPERTY CXX_STANDARD ${LAZILY_CXX_STANDARD})
set_property(TARGET lazily_core_consumer PROPERTY CXX_STANDARD_REQUIRED ON)
set_property(TARGET lazily_core_consumer PROPERTY CXX_EXTENSIONS OFF)
target_include_directories(lazily_core_consumer PRIVATE "${LAZILY_ARTIFACT}/include")

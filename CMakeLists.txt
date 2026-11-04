# RemesherDesk — top-level build.
# Scaffold only: a stub core library and tests. Nothing here assumes anything
# about the Quadremesh native code (see docs/PLAN.md, intake gate B0).
cmake_minimum_required(VERSION 3.25)
project(RemesherDesk VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(RDESK_BUILD_TESTS "Build tests" ON)
option(RDESK_WERROR "Treat warnings as errors" OFF)
option(RDESK_SANITIZE "Enable ASan+UBSan (gcc/clang only)" OFF)

include(cmake/RdeskWarnings.cmake)

add_subdirectory(src/core)

if(RDESK_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

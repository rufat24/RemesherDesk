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
option(RDESK_BUILD_APP "Build the GUI (fetches Polyscope; needs OpenGL and, on Linux, X11 development packages)" OFF)
option(RDESK_SANITIZE "Enable ASan+UBSan (gcc/clang only)" OFF)

# enable_testing() must come before the first add_subdirectory(): tests registered in a subdirectory (src/app) are
# otherwise dropped silently.
if(RDESK_BUILD_TESTS)
  enable_testing()
endif()

include(cmake/RdeskWarnings.cmake)
include(cmake/Deps.cmake)

add_subdirectory(src/core)
add_subdirectory(src/io)
add_subdirectory(src/platform)
add_subdirectory(src/backends/fake)
add_subdirectory(src/cli)
if(RDESK_BUILD_APP)
  add_subdirectory(src/app)
endif()

if(RDESK_BUILD_TESTS)
  add_subdirectory(tests)
endif()

# Pinned third-party dependencies. Rules (docs/work-breakdown.md): pin by commit SHA or by
# URL + SHA256, and add a row to the licence table in docs/packaging-licensing.md in the same commit.
#
# Both dependencies below are header-only. We fetch the sources but deliberately do NOT run their
# CMake projects (SOURCE_SUBDIR points at a directory that does not exist): the pinned doctest
# 2.4.11 declares cmake_minimum_required(VERSION < 3.5), which current CMake releases reject (seen on
# the GitHub Windows runner), and we only need an include path anyway.
include(FetchContent)

# For offline work: cmake -DFETCHCONTENT_SOURCE_DIR_DOCTEST=/path/to/doctest ...  (same for NLOHMANN_JSON).

# doctest 2.4.11 (MIT), tests only. Pinned to the commit the v2.4.11 tag points at.
function(rdesk_fetch_doctest)
  if(TARGET doctest::doctest)
    return()
  endif()
  FetchContent_Declare(doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG        ae7a13539fb71f270b87eb2e874fbac80bc8dda2  # v2.4.11
    SOURCE_SUBDIR  rdesk-header-only-no-cmake)
  FetchContent_MakeAvailable(doctest)
  add_library(doctest::doctest INTERFACE IMPORTED GLOBAL)
  # The header is included as "doctest/doctest.h", which lives in <src>/doctest/.
  target_include_directories(doctest::doctest SYSTEM INTERFACE "${doctest_SOURCE_DIR}")
endfunction()

# nlohmann/json 3.11.3 (MIT), used by src/io for the RDM META chunk. Pinned to the commit of tag v3.11.3.
function(rdesk_fetch_json)
  if(TARGET nlohmann_json::nlohmann_json)
    return()
  endif()
  FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03  # v3.11.3
    SOURCE_SUBDIR  rdesk-header-only-no-cmake)
  FetchContent_MakeAvailable(nlohmann_json)
  add_library(nlohmann_json::nlohmann_json INTERFACE IMPORTED GLOBAL)
  target_include_directories(nlohmann_json::nlohmann_json SYSTEM INTERFACE "${nlohmann_json_SOURCE_DIR}/single_include")
endfunction()

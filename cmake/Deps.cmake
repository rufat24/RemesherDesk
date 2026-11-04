# Pinned third-party dependencies. Rules (docs/work-breakdown.md): pin by commit SHA or by
# URL + SHA256, and add a row to the licence table in docs/packaging-licensing.md in the same commit.
include(FetchContent)

# doctest 2.4.11 (MIT), tests only. Pinned to the commit the v2.4.11 tag points at.
# For offline work: cmake -DFETCHCONTENT_SOURCE_DIR_DOCTEST=/path/to/doctest ...
function(rdesk_fetch_doctest)
  if(TARGET doctest::doctest)
    return()
  endif()
  set(DOCTEST_WITH_TESTS OFF CACHE BOOL "" FORCE)
  set(DOCTEST_WITH_MAIN_IN_STATIC_LIB OFF CACHE BOOL "" FORCE)
  set(DOCTEST_NO_INSTALL ON CACHE BOOL "" FORCE)
  FetchContent_Declare(doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG        ae7a13539fb71f270b87eb2e874fbac80bc8dda2  # v2.4.11
    SYSTEM)
  FetchContent_MakeAvailable(doctest)
endfunction()

# core.version is a plain executable; unit tests (A2+) use doctest.
add_executable(test_version test_version.cpp)
target_link_libraries(test_version PRIVATE rdesk::core)
rdesk_target_defaults(test_version)
add_test(NAME core.version COMMAND test_version)
set_tests_properties(core.version PROPERTIES LABELS "unit")

# Reference RDM reader/writer self-test (pure Python, stdlib only).
find_package(Python3 3.8 COMPONENTS Interpreter)
if(Python3_FOUND)
  add_test(NAME tools.rdm_py_selftest
           COMMAND Python3::Interpreter -I ${PROJECT_SOURCE_DIR}/tools/rdm/rdm.py selftest)
  set_tests_properties(tools.rdm_py_selftest PROPERTIES LABELS "tools")
endif()

# Unit tests (doctest). One executable; ctest label "unit".
include(${PROJECT_SOURCE_DIR}/cmake/Deps.cmake)
rdesk_fetch_doctest()
add_executable(rdesk_unit
  unit/unit_main.cpp
  unit/test_mesh.cpp
  unit/test_validate.cpp
)
target_link_libraries(rdesk_unit PRIVATE rdesk::core doctest::doctest)
rdesk_target_defaults(rdesk_unit)
add_test(NAME unit.all COMMAND rdesk_unit)
set_tests_properties(unit.all PROPERTIES LABELS "unit")

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
rdesk_fetch_doctest()
add_executable(rdesk_unit
  unit/unit_main.cpp
  unit/test_mesh.cpp
  unit/test_validate.cpp
  unit/test_rdm.cpp
)
target_link_libraries(rdesk_unit PRIVATE rdesk::core rdesk::io doctest::doctest)
target_compile_definitions(rdesk_unit PRIVATE RDESK_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
rdesk_target_defaults(rdesk_unit)
add_test(NAME unit.all COMMAND rdesk_unit)
set_tests_properties(unit.all PROPERTIES LABELS "unit")

# C++ RDM writer -> Python reference reader agreement (A3 exit check).
add_executable(rdm_write_cube tools/rdm_write_cube.cpp)
target_link_libraries(rdm_write_cube PRIVATE rdesk::core rdesk::io)
rdesk_target_defaults(rdm_write_cube)
if(Python3_FOUND)
  add_test(NAME io.rdm_cpp_writes_what_python_reads
           COMMAND ${CMAKE_COMMAND}
             -DWRITER=$<TARGET_FILE:rdm_write_cube>
             -DPYTHON=${Python3_EXECUTABLE}
             -DRDM_PY=${PROJECT_SOURCE_DIR}/tools/rdm/rdm.py
             -DOUT=${CMAKE_CURRENT_BINARY_DIR}/cpp_cube.rdm
             -P ${CMAKE_CURRENT_SOURCE_DIR}/run_rdm_crosscheck.cmake)
  set_tests_properties(io.rdm_cpp_writes_what_python_reads PROPERTIES LABELS "unit;tools")
endif()

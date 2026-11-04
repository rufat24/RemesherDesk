# Tests are plain executables (no framework yet). Task A2 introduces doctest.
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

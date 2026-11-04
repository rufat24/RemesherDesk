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

# Helper program started by the process-layer tests.
add_executable(rdesk_test_child helpers/child.cpp)
if(WIN32)
  target_link_libraries(rdesk_test_child PRIVATE rdesk::platform)
endif()
rdesk_target_defaults(rdesk_test_child)

# Unit tests (doctest). One executable; ctest label "unit".
rdesk_fetch_doctest()
add_executable(rdesk_unit
  unit/unit_main.cpp
  unit/test_mesh.cpp
  unit/test_validate.cpp
  unit/test_rdm.cpp
  unit/test_obj.cpp
  unit/test_metrics.cpp
  unit/test_facade.cpp
  unit/test_process.cpp
)
target_link_libraries(rdesk_unit PRIVATE rdesk::core rdesk::io rdesk::platform rdesk::backend_fake doctest::doctest)
target_include_directories(rdesk_unit PRIVATE ${PROJECT_SOURCE_DIR}/src/core/src)
target_compile_definitions(rdesk_unit PRIVATE RDESK_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
                                              RDESK_TEST_CHILD="$<TARGET_FILE:rdesk_test_child>")
add_dependencies(rdesk_unit rdesk_test_child)
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

# Property tests: random meshes, seeds in the failure messages. ctest label "property".
add_executable(rdesk_property property/test_properties.cpp)
target_link_libraries(rdesk_property PRIVATE rdesk::core doctest::doctest)
target_include_directories(rdesk_property PRIVATE ${PROJECT_SOURCE_DIR}/src/core/src ${CMAKE_CURRENT_SOURCE_DIR}/unit)
rdesk_target_defaults(rdesk_property)
add_test(NAME property.all COMMAND rdesk_property)
set_tests_properties(property.all PROPERTIES LABELS "property")

# Worker protocol conformance: drives the real rdesk executable through rdesk::platform::Process.
add_executable(rdesk_protocol protocol/test_protocol.cpp)
target_link_libraries(rdesk_protocol PRIVATE rdesk::core rdesk::io rdesk::platform doctest::doctest nlohmann_json::nlohmann_json)
target_include_directories(rdesk_protocol PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/unit)
target_compile_definitions(rdesk_protocol PRIVATE RDESK_EXE="$<TARGET_FILE:rdesk>")
add_dependencies(rdesk_protocol rdesk)
rdesk_target_defaults(rdesk_protocol)
add_test(NAME protocol.all COMMAND rdesk_protocol)
set_tests_properties(protocol.all PROPERTIES LABELS "protocol" TIMEOUT 300)

# Fuzz targets (docs/testing.md §6). With RDESK_FUZZ_ENGINE=ON (preset clang-fuzz) they link libFuzzer; otherwise they
# get a replay main() and ctest feeds them the seed corpus and every file in tests/data/fuzz-regressions, so crashes
# found once stay fixed on every compiler.
option(RDESK_FUZZ_ENGINE "Link the fuzz targets with libFuzzer (clang only)" OFF)
foreach(target IN ITEMS fuzz_rdm:rdm fuzz_obj:obj fuzz_params_json:params)
  string(REPLACE ":" ";" parts "${target}")
  list(GET parts 0 name)
  list(GET parts 1 kind)
  add_executable(${name} fuzz/${name}.cpp)
  target_link_libraries(${name} PRIVATE rdesk::core rdesk::io rdesk::backend_fake)
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/fuzz)
  rdesk_target_defaults(${name})
  if(RDESK_FUZZ_ENGINE)
    target_link_options(${name} PRIVATE -fsanitize=fuzzer)
  else()
    target_sources(${name} PRIVATE fuzz/replay_main.cpp)
    add_test(NAME fuzz.replay.${kind}
             COMMAND ${name} ${CMAKE_CURRENT_SOURCE_DIR}/fuzz/corpus/${kind} ${CMAKE_CURRENT_SOURCE_DIR}/data/fuzz-regressions/${kind})
    set_tests_properties(fuzz.replay.${kind} PROPERTIES LABELS "unit;fuzz")
  endif()
endforeach()

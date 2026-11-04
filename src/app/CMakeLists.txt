rdesk_fetch_polyscope()
add_executable(RemesherDesk spike_main.cpp)
target_link_libraries(RemesherDesk PRIVATE rdesk::core rdesk::io rdesk::platform polyscope)
rdesk_target_defaults(RemesherDesk)

# Screenshot smoke test: Linux only, needs xvfb-run (Mesa software rendering).
find_program(RDESK_XVFB_RUN xvfb-run)
if(RDESK_XVFB_RUN AND UNIX)
  add_test(NAME gui.screenshot_smoke
           COMMAND ${PROJECT_SOURCE_DIR}/tests/gui/screenshot_smoke.sh $<TARGET_FILE:RemesherDesk>
                   ${PROJECT_SOURCE_DIR}/tests/data/mixed.obj ${PROJECT_SOURCE_DIR}/tools/png_stats.py
                   ${CMAKE_CURRENT_BINARY_DIR}/screenshot_smoke.png)
  set_tests_properties(gui.screenshot_smoke PROPERTIES LABELS "gui" TIMEOUT 120)
endif()

rdesk_fetch_json()
add_library(rdesk_io
  src/rdm.cpp
)
add_library(rdesk::io ALIAS rdesk_io)
target_include_directories(rdesk_io PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(rdesk_io PUBLIC rdesk::core PRIVATE nlohmann_json::nlohmann_json)
rdesk_target_defaults(rdesk_io)

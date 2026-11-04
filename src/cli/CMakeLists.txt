add_executable(rdesk main.cpp)
target_link_libraries(rdesk PRIVATE rdesk::core rdesk::io rdesk::platform rdesk::backend_fake nlohmann_json::nlohmann_json)
target_include_directories(rdesk PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
rdesk_target_defaults(rdesk)

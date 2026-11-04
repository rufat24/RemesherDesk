add_library(rdesk_backend_fake
  fake_backend.cpp
)
add_library(rdesk::backend_fake ALIAS rdesk_backend_fake)
target_include_directories(rdesk_backend_fake PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
target_link_libraries(rdesk_backend_fake PUBLIC rdesk::core)
rdesk_target_defaults(rdesk_backend_fake)

add_library(rdesk_core
  src/version.cpp
  src/mesh.cpp
  src/convention.cpp
  src/validate.cpp
  src/metrics.cpp
  src/params.cpp
  src/facade.cpp
  src/sampling.cpp
  src/aabb_tree.cpp
)
add_library(rdesk::core ALIAS rdesk_core)
target_include_directories(rdesk_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_definitions(rdesk_core PRIVATE
  RDESK_VERSION_STRING="${PROJECT_VERSION}")
rdesk_target_defaults(rdesk_core)

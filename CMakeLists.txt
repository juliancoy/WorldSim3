cmake_minimum_required(VERSION 3.20)
project(WorldSim3 VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

find_package(Vulkan REQUIRED)
find_package(CURL REQUIRED)

set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/glfw/CMakeLists.txt")
  add_subdirectory(third_party/glfw)
else()
  find_package(glfw3 QUIET)
endif()

if(NOT TARGET glfw)
  FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.4
  )
  FetchContent_MakeAvailable(glfw)
endif()

FetchContent_Declare(
  imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.91.9b
)
FetchContent_MakeAvailable(imgui)

FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

set(NO_TESTS ON CACHE BOOL "" FORCE)
set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
set(NO_MEDIA ON CACHE BOOL "" FORCE)
FetchContent_Declare(
  libdatachannel
  GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
  GIT_TAG v0.22.6
)
FetchContent_MakeAvailable(libdatachannel)

add_executable(worldsim3
  main.cpp
  worldsim_app.cpp
  worldsim_app_run.cpp
  worldsim_cli.cpp
  worldsim_dataset_bootstrap.cpp
  worldsim_bootstrap.cpp
  aggregate_visualization_strategies.cpp
  app_lifecycle.cpp
  app_settings.cpp
  app_utils.cpp
  memory_utils.cpp
  zoning.cpp
  parcel_matched_layers.cpp
  vacancy_overlay.cpp
  dataset_library.cpp
  net_http_utils.cpp
  status_api.cpp
  dataset_lan_api.cpp
  layer_runtime.cpp
  layer_workers.cpp
  layer_geometry.cpp
  map_render_heatmap_pass.cpp
  map_render_hover.cpp
  map_render_layers.cpp
  map_render_overlays.cpp
  map_render_projection.cpp
  map_render_utils.cpp
  parcel_timeline.cpp
  heatmap_render.cpp
  heatmap_gpu_aggregate.cpp
  heatmap_strategy_common.cpp
  heatmap_strategy_grid.cpp
  heatmap_strategy_kde.cpp
  heatmap_strategy_gpu_splat.cpp
  heatmap_strategy_hex.cpp
  heatmap_strategy_multires.cpp
  time_cube_panel.cpp
  policy_panel.cpp
  model_tabs_panel.cpp
  layer_settings.cpp
  download_queue.cpp
  graph_model_tab.cpp
  star_schema_tab.cpp
  spatial_index_tab.cpp
  uncertainty_tab.cpp
  change_log_tab.cpp
  risk_scorecards_tab.cpp
  causal_panel_tab.cpp
  scenarios_tab.cpp
  arkavo_realtime_client.cpp
  arkavo_signaling_transport_curl.cpp
  arkavo_rtc_session_manager.cpp
  time_cube.cpp
  layer_state_io.cpp
  feature_props.cpp
  geo.cpp
  cache_io.cpp
  cpu_affinity.cpp
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
)

target_include_directories(worldsim3 PRIVATE
  ${imgui_SOURCE_DIR}
  ${imgui_SOURCE_DIR}/backends
  ${CMAKE_CURRENT_SOURCE_DIR}
)

find_program(GLSLANG_VALIDATOR_EXECUTABLE glslangValidator)
if(GLSLANG_VALIDATOR_EXECUTABLE)
  set(WS3_SHADER_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated_shaders")
  file(MAKE_DIRECTORY "${WS3_SHADER_OUT_DIR}")
  set(WS3_GPU_SPLAT_SHADER_SPV "${WS3_SHADER_OUT_DIR}/heatmap_splat.comp.spv")
  add_custom_command(
    OUTPUT "${WS3_GPU_SPLAT_SHADER_SPV}"
    COMMAND "${GLSLANG_VALIDATOR_EXECUTABLE}" -V "${CMAKE_CURRENT_SOURCE_DIR}/shaders/heatmap_splat.comp" -o "${WS3_GPU_SPLAT_SHADER_SPV}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/heatmap_splat.comp"
    COMMENT "Compiling heatmap_splat.comp -> SPIR-V"
    VERBATIM
  )
  add_custom_target(worldsim3_shaders DEPENDS "${WS3_GPU_SPLAT_SHADER_SPV}")
  add_dependencies(worldsim3 worldsim3_shaders)
  target_compile_definitions(worldsim3 PRIVATE WS3_GPU_SPLAT_SHADER_SPV="${WS3_GPU_SPLAT_SHADER_SPV}")
endif()

target_link_libraries(worldsim3 PRIVATE Vulkan::Vulkan nlohmann_json::nlohmann_json glfw CURL::libcurl datachannel)

if(UNIX AND NOT APPLE)
  target_link_libraries(worldsim3 PRIVATE dl pthread)
endif()

if(WIN32)
  target_compile_definitions(worldsim3 PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_link_libraries(worldsim3 PRIVATE ws2_32)
endif()

add_executable(arkavo_connectivity_test
  arkavo_connectivity_test.cpp
  arkavo_realtime_client.cpp
  arkavo_signaling_transport_curl.cpp
  arkavo_rtc_session_manager.cpp
)

target_include_directories(arkavo_connectivity_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(arkavo_connectivity_test PRIVATE nlohmann_json::nlohmann_json CURL::libcurl datachannel)

if(UNIX AND NOT APPLE)
  target_link_libraries(arkavo_connectivity_test PRIVATE pthread)
endif()

if(WIN32)
  target_compile_definitions(arkavo_connectivity_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_link_libraries(arkavo_connectivity_test PRIVATE ws2_32)
endif()

add_executable(worldsim3_gpu_aggregate_harness
  gpu_aggregate_harness.cpp
  heatmap_gpu_aggregate.cpp
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
)

target_include_directories(worldsim3_gpu_aggregate_harness PRIVATE
  ${imgui_SOURCE_DIR}
  ${imgui_SOURCE_DIR}/backends
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(worldsim3_gpu_aggregate_harness PRIVATE Vulkan::Vulkan glfw)

if(UNIX AND NOT APPLE)
  target_link_libraries(worldsim3_gpu_aggregate_harness PRIVATE dl pthread)
endif()

if(WIN32)
  target_compile_definitions(worldsim3_gpu_aggregate_harness PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_link_libraries(worldsim3_gpu_aggregate_harness PRIVATE ws2_32)
endif()

if(GLSLANG_VALIDATOR_EXECUTABLE)
  add_dependencies(worldsim3_gpu_aggregate_harness worldsim3_shaders)
  target_compile_definitions(worldsim3_gpu_aggregate_harness PRIVATE WS3_GPU_SPLAT_SHADER_SPV="${WS3_GPU_SPLAT_SHADER_SPV}")
endif()

install(TARGETS worldsim3 RUNTIME DESTINATION bin)

install(
  FILES packaging/linux/worldsim3.desktop
  DESTINATION share/applications
)

install(
  FILES packaging/linux/worldsim3.svg
  DESTINATION share/icons/hicolor/scalable/apps
)

set(CPACK_PACKAGE_NAME "worldsim3")
set(CPACK_PACKAGE_VENDOR "WorldSim3")
set(CPACK_PACKAGE_CONTACT "maintainers@worldsim3.local")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "World simulation and visualization tool")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")

if(UNIX AND NOT APPLE)
  set(CPACK_GENERATOR "DEB")
  set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
  set(CPACK_DEBIAN_PACKAGE_SECTION "science")
  set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
  set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
endif()

include(CPack)

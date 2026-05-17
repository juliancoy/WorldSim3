cmake_minimum_required(VERSION 3.20)
project(WorldSim3 VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

find_package(Vulkan REQUIRED)
find_package(CURL REQUIRED)
find_package(ZLIB REQUIRED)

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

# The status screenshot API reads back from the main swapchain image. Dear ImGui's
# Vulkan helper defaults swapchain images to COLOR_ATTACHMENT only, so request
# TRANSFER_SRC when the surface supports it.
set(WS3_IMGUI_VULKAN_BACKEND "${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp")
file(READ "${WS3_IMGUI_VULKAN_BACKEND}" WS3_IMGUI_VULKAN_BACKEND_TEXT)
if(NOT WS3_IMGUI_VULKAN_BACKEND_TEXT MATCHES "info\\.imageUsage \\|= VK_IMAGE_USAGE_TRANSFER_SRC_BIT")
  string(REPLACE
    "info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;"
    "info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;\n        if (cap.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)\n            info.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;"
    WS3_IMGUI_VULKAN_BACKEND_TEXT
    "${WS3_IMGUI_VULKAN_BACKEND_TEXT}")
  file(WRITE "${WS3_IMGUI_VULKAN_BACKEND}" "${WS3_IMGUI_VULKAN_BACKEND_TEXT}")
endif()

FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

set(BUILD_SHELL OFF CACHE BOOL "" FORCE)
set(BUILD_UNITTESTS OFF CACHE BOOL "" FORCE)
set(BUILD_EXTENSIONS "" CACHE STRING "" FORCE)
set(DISABLE_BUILTIN_EXTENSIONS TRUE CACHE BOOL "" FORCE)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/duckdb/CMakeLists.txt")
  add_subdirectory(third_party/duckdb EXCLUDE_FROM_ALL)
else()
  FetchContent_Declare(
    duckdb
    GIT_REPOSITORY https://github.com/duckdb/duckdb.git
    GIT_TAG 19864453f7d0ed095256d848b46e7b8630989bac
  )
  FetchContent_MakeAvailable(duckdb)
endif()

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
  app_main_loop.cpp
  worldsim_cli.cpp
  worldsim_dataset_bootstrap.cpp
  worldsim_bootstrap.cpp
  aggregate_visualization_strategies.cpp
  app_lifecycle.cpp
  app_settings.cpp
  app_utils.cpp
  app_frame_support.cpp
  frame_prelude.cpp
  memory_utils.cpp
  zoning.cpp
  parcel_matched_layers.cpp
  vacancy_overlay.cpp
  dataset_library.cpp
  duckdb_analytics.cpp
  sql_tab.cpp
  net_http_utils.cpp
  status_api.cpp
  dataset_lan_api.cpp
  layer_runtime.cpp
  layer_pipeline_drain.cpp
  derived_layer_caches.cpp
  layer_workers.cpp
  layer_geometry.cpp
  basemap_sources.cpp
  map_render_heatmap_pass.cpp
  map_render_hover.cpp
  map_render_basemap.cpp
  map_render_hud.cpp
  map_inspection.cpp
  map_overlay_panels.cpp
  map_render_layers.cpp
  map_render_overlays.cpp
  map_render_projection.cpp
  map_render_selection.cpp
  map_render_utils.cpp
  map_canvas_session.cpp
  map_frame_render.cpp
  map_frame_session.cpp
  map_tab.cpp
  left_panel.cpp
  map_viewport.cpp
  parcel_timeline.cpp
  parcel_unified.cpp
  parcel_consolidation.cpp
  heatmap_render.cpp
  heatmap_gpu_aggregate.cpp
  heatmap_strategy_common.cpp
  heatmap_strategy_grid.cpp
  heatmap_strategy_kde.cpp
  heatmap_strategy_gpu_splat.cpp
  heatmap_strategy_hex.cpp
  heatmap_strategy_multires.cpp
  gear_panel.cpp
  time_cube_panel.cpp
  policy_panel.cpp
  model_tabs_panel.cpp
  layers_panel_ui.cpp
  ui_primitives.cpp
  ui_fonts.cpp
  ui_theme.cpp
  layer_registry.cpp
  layer_ui_actions.cpp
  layer_ui_context_builders.cpp
  layer_runtime_coordinator.cpp
  layer_ui_state_sync.cpp
  data_library_coordinator.cpp
  data_library_panel.cpp
  performance_stats_panel.cpp
  performance_runtime_support.cpp
  filter_context_builder.cpp
  heat_normalization.cpp
  heatmap_key_builder.cpp
  heatmap_runtime.cpp
  render_layer_pass.cpp
  render_tail_pass.cpp
  render_policy.cpp
  profiling_layer_snapshot.cpp
  basemap_coverage.cpp
  render_plan_builder.cpp
  status_api_context_builder.cpp
  api_control_commands.cpp
  app_shutdown_context_builder.cpp
  layer_settings.cpp
  download_queue.cpp
  basemap_panel.cpp
  layer_download_queue.cpp
  lan_discovery.cpp
  tiles.cpp
  owner_info.cpp
  owner_aggregates.cpp
  active_queries_tab.cpp
  owners_tab.cpp
  gradient_tab.cpp
  vacancy_parcel_tab.cpp
  parcel_info_tab.cpp
  filters_tab.cpp
  right_panel.cpp
  zoning_filters_panel.cpp
  graph_model_tab.cpp
  star_schema_tab.cpp
  spatial_index_tab.cpp
  uncertainty_tab.cpp
  change_log_tab.cpp
  risk_scorecards_tab.cpp
  causal_panel_tab.cpp
  scenarios_tab.cpp
  selection.cpp
  arkavo_realtime_client.cpp
  arkavo_signaling_transport_curl.cpp
  arkavo_rtc_session_manager.cpp
  time_cube.cpp
  layer_state_io.cpp
  layer_import.cpp
  filters.cpp
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

target_compile_definitions(worldsim3 PRIVATE
  WS3_GPU_SPLAT_SHADER_SRC="${CMAKE_CURRENT_SOURCE_DIR}/shaders/heatmap_splat.comp"
  WS3_PARCEL_GPU_VERT_SRC="${CMAKE_CURRENT_SOURCE_DIR}/shaders/parcel_fill.vert"
  WS3_PARCEL_GPU_FRAG_SRC="${CMAKE_CURRENT_SOURCE_DIR}/shaders/parcel_fill.frag"
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
  set(WS3_PARCEL_GPU_VERT_SPV "${WS3_SHADER_OUT_DIR}/parcel_fill.vert.spv")
  set(WS3_PARCEL_GPU_FRAG_SPV "${WS3_SHADER_OUT_DIR}/parcel_fill.frag.spv")
  add_custom_command(
    OUTPUT "${WS3_GPU_SPLAT_SHADER_SPV}"
    COMMAND "${GLSLANG_VALIDATOR_EXECUTABLE}" -V "${CMAKE_CURRENT_SOURCE_DIR}/shaders/heatmap_splat.comp" -o "${WS3_GPU_SPLAT_SHADER_SPV}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/heatmap_splat.comp"
    COMMENT "Compiling heatmap_splat.comp -> SPIR-V"
    VERBATIM
  )
  add_custom_command(
    OUTPUT "${WS3_PARCEL_GPU_VERT_SPV}"
    COMMAND "${GLSLANG_VALIDATOR_EXECUTABLE}" -V "${CMAKE_CURRENT_SOURCE_DIR}/shaders/parcel_fill.vert" -o "${WS3_PARCEL_GPU_VERT_SPV}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/parcel_fill.vert"
    COMMENT "Compiling parcel_fill.vert -> SPIR-V"
    VERBATIM
  )
  add_custom_command(
    OUTPUT "${WS3_PARCEL_GPU_FRAG_SPV}"
    COMMAND "${GLSLANG_VALIDATOR_EXECUTABLE}" -V "${CMAKE_CURRENT_SOURCE_DIR}/shaders/parcel_fill.frag" -o "${WS3_PARCEL_GPU_FRAG_SPV}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/parcel_fill.frag"
    COMMENT "Compiling parcel_fill.frag -> SPIR-V"
    VERBATIM
  )
  add_custom_target(worldsim3_shaders DEPENDS
    "${WS3_GPU_SPLAT_SHADER_SPV}"
    "${WS3_PARCEL_GPU_VERT_SPV}"
    "${WS3_PARCEL_GPU_FRAG_SPV}"
  )
  add_dependencies(worldsim3 worldsim3_shaders)
  target_compile_definitions(worldsim3 PRIVATE
    WS3_GPU_SPLAT_SHADER_SPV="${WS3_GPU_SPLAT_SHADER_SPV}"
    WS3_PARCEL_GPU_VERT_SPV="${WS3_PARCEL_GPU_VERT_SPV}"
    WS3_PARCEL_GPU_FRAG_SPV="${WS3_PARCEL_GPU_FRAG_SPV}"
  )
endif()

target_link_libraries(worldsim3 PRIVATE Vulkan::Vulkan nlohmann_json::nlohmann_json glfw CURL::libcurl ZLIB::ZLIB datachannel)

if(TARGET duckdb_static)
  target_link_libraries(worldsim3 PRIVATE duckdb_static)
elseif(TARGET duckdb)
  target_link_libraries(worldsim3 PRIVATE duckdb)
endif()

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
target_compile_definitions(worldsim3_gpu_aggregate_harness PRIVATE WS3_GPU_SPLAT_SHADER_SRC="${CMAKE_CURRENT_SOURCE_DIR}/shaders/heatmap_splat.comp")

target_include_directories(worldsim3_gpu_aggregate_harness PRIVATE
  ${imgui_SOURCE_DIR}
  ${imgui_SOURCE_DIR}/backends
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(worldsim3_gpu_aggregate_harness PRIVATE Vulkan::Vulkan nlohmann_json::nlohmann_json glfw)

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

add_executable(worldsim_hud_pit_builder
  tools/hud_pit_layer_builder.cpp
)
target_include_directories(worldsim_hud_pit_builder PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(worldsim_hud_pit_builder PRIVATE nlohmann_json::nlohmann_json)

add_executable(worldsim_regional_parcel_builder
  tools/regional_parcel_builder.cpp
)
target_include_directories(worldsim_regional_parcel_builder PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(worldsim_regional_parcel_builder PRIVATE nlohmann_json::nlohmann_json)

add_executable(worldsim3_duckdb_owner_dump
  tools/duckdb_owner_dump.cpp
  aggregate_visualization_strategies.cpp
  app_utils.cpp
  cache_io.cpp
  cpu_affinity.cpp
  duckdb_analytics.cpp
  feature_props.cpp
  geo.cpp
  headless_layer_hydration.cpp
  layer_geometry.cpp
  layer_runtime.cpp
  layer_state_io.cpp
  layer_workers.cpp
  memory_utils.cpp
  parcel_consolidation.cpp
  parcel_unified.cpp
)
target_include_directories(worldsim3_duckdb_owner_dump PRIVATE
  ${imgui_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(worldsim3_duckdb_owner_dump PRIVATE nlohmann_json::nlohmann_json)
if(TARGET duckdb_static)
  target_link_libraries(worldsim3_duckdb_owner_dump PRIVATE duckdb_static)
elseif(TARGET duckdb)
  target_link_libraries(worldsim3_duckdb_owner_dump PRIVATE duckdb)
endif()
if(UNIX AND NOT APPLE)
  target_link_libraries(worldsim3_duckdb_owner_dump PRIVATE dl pthread)
endif()
if(WIN32)
  target_compile_definitions(worldsim3_duckdb_owner_dump PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_link_libraries(worldsim3_duckdb_owner_dump PRIVATE ws2_32)
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

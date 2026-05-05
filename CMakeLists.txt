cmake_minimum_required(VERSION 3.20)
project(BaltimoreVulkanMap LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

find_package(Vulkan REQUIRED)
find_package(CURL REQUIRED)
find_package(glfw3 QUIET)
if(NOT glfw3_FOUND)
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
  arkavo_realtime_client.cpp
  arkavo_signaling_transport_curl.cpp
  arkavo_rtc_session_manager.cpp
  time_cube.cpp
  layer_state_io.cpp
  feature_props.cpp
  geo.cpp
  cache_io.cpp
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

target_link_libraries(worldsim3 PRIVATE Vulkan::Vulkan nlohmann_json::nlohmann_json glfw CURL::libcurl datachannel)

if(UNIX AND NOT APPLE)
  target_link_libraries(worldsim3 PRIVATE dl pthread)
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

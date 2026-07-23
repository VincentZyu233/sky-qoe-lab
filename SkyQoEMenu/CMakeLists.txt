cmake_minimum_required(VERSION 3.24)

project(SkyQoEMenu VERSION 0.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(MINGW)
  get_filename_component(SKYQOE_MINGW_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
  find_program(
    SKYQOE_WINDRES_EXECUTABLE
    NAMES windres.exe windres
    HINTS "${SKYQOE_MINGW_BIN_DIR}"
    REQUIRED
  )
else()
  enable_language(RC)
endif()

include(FetchContent)

FetchContent_Declare(
  minhook
  GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
  GIT_TAG v1.3.4
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(minhook)

set(SKYQOE_IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.tools/imgui" CACHE PATH "Dear ImGui source directory")
if(NOT EXISTS "${SKYQOE_IMGUI_DIR}/imgui.cpp")
  FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG 8936b58fe26e8c3da834b8f60b06511d537b4c63
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(imgui)
  set(SKYQOE_IMGUI_DIR "${imgui_SOURCE_DIR}")
endif()

add_library(SkyQoEMenu SHARED
  src/dllmain.cpp
  src/game_state.cpp
  src/http_server.cpp
  src/level_assets.cpp
  src/local_effects.cpp
  src/overlay.cpp
  src/snapshot_json.cpp
  "${SKYQOE_IMGUI_DIR}/imgui.cpp"
  "${SKYQOE_IMGUI_DIR}/imgui_draw.cpp"
  "${SKYQOE_IMGUI_DIR}/imgui_tables.cpp"
  "${SKYQOE_IMGUI_DIR}/imgui_widgets.cpp"
  "${SKYQOE_IMGUI_DIR}/backends/imgui_impl_win32.cpp"
  "${SKYQOE_IMGUI_DIR}/backends/imgui_impl_dx11.cpp"
)

target_include_directories(SkyQoEMenu PRIVATE
  src
  "${SKYQOE_IMGUI_DIR}"
  "${SKYQOE_IMGUI_DIR}/backends"
)

target_compile_definitions(SkyQoEMenu PRIVATE
  UNICODE
  _UNICODE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  IMGUI_IMPL_WIN32_DISABLE_GAMEPAD
)

if(MINGW)
  target_compile_options(SkyQoEMenu PRIVATE -Wall -Wextra -Wpedantic -finput-charset=UTF-8 -fexec-charset=UTF-8)
  target_link_options(SkyQoEMenu PRIVATE -static-libgcc -static-libstdc++)
endif()

target_link_libraries(SkyQoEMenu PRIVATE
  d3d11
  dxgi
  d3dcompiler
  dwmapi
  user32
  gdi32
  shell32
  imm32
  ws2_32
  minhook
)

set_target_properties(SkyQoEMenu PROPERTIES
  PREFIX ""
  OUTPUT_NAME "SkyQoEMenu"
)

add_executable(SkyQoEMenuHarness WIN32 tools/harness.cpp)
target_compile_definitions(SkyQoEMenuHarness PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
if(MINGW)
  target_link_options(SkyQoEMenuHarness PRIVATE -municode -static-libgcc -static-libstdc++)
endif()
target_link_libraries(SkyQoEMenuHarness PRIVATE user32 gdi32)

add_executable(SkyQoEMenuApiTest tools/api_test.cpp)
target_compile_definitions(SkyQoEMenuApiTest PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
if(MINGW)
  target_link_options(SkyQoEMenuApiTest PRIVATE -static-libgcc -static-libstdc++)
endif()

add_executable(SkyProcessDump tools/process_dump.cpp)
target_compile_definitions(SkyProcessDump PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
if(MINGW)
  target_link_options(SkyProcessDump PRIVATE -municode -static-libgcc -static-libstdc++)
endif()
target_link_libraries(SkyProcessDump PRIVATE dbghelp)

add_executable(SkyLevelAssetInspect
  tools/level_asset_inspect.cpp
  src/level_assets.cpp
)
target_include_directories(SkyLevelAssetInspect PRIVATE src)
target_compile_definitions(SkyLevelAssetInspect PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
if(MINGW)
  target_link_options(SkyLevelAssetInspect PRIVATE -municode -static-libgcc -static-libstdc++)
endif()
target_link_libraries(SkyLevelAssetInspect PRIVATE shell32)

set(SKYQOE_LOADER_PAYLOAD "${CMAKE_CURRENT_BINARY_DIR}/loader-payload/SkyQoEMenu.dll")
add_custom_command(
  OUTPUT "${SKYQOE_LOADER_PAYLOAD}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/loader-payload"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "$<TARGET_FILE:SkyQoEMenu>"
          "${SKYQOE_LOADER_PAYLOAD}"
  DEPENDS SkyQoEMenu
  VERBATIM
)
add_custom_target(SkyQoELoaderPayload DEPENDS "${SKYQOE_LOADER_PAYLOAD}")

file(TO_CMAKE_PATH "${SKYQOE_LOADER_PAYLOAD}" SKYQOE_LOADER_PAYLOAD_RC_PATH)
file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/tools/loader.manifest"
     SKYQOE_LOADER_MANIFEST_RC_PATH)
configure_file(
  tools/loader.rc.in
  "${CMAKE_CURRENT_BINARY_DIR}/skyqoe_loader.rc"
  @ONLY
)

if(MINGW)
  set(SKYQOE_LOADER_RESOURCE_OBJECT
      "${CMAKE_CURRENT_BINARY_DIR}/skyqoe_loader.rc.obj")
  add_custom_command(
    OUTPUT "${SKYQOE_LOADER_RESOURCE_OBJECT}"
    COMMAND "${SKYQOE_WINDRES_EXECUTABLE}" -O coff
            "${CMAKE_CURRENT_BINARY_DIR}/skyqoe_loader.rc"
            "${SKYQOE_LOADER_RESOURCE_OBJECT}"
    DEPENDS
      "${CMAKE_CURRENT_BINARY_DIR}/skyqoe_loader.rc"
      "${SKYQOE_LOADER_PAYLOAD}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tools/loader.manifest"
    VERBATIM
  )
  set_source_files_properties(
    "${SKYQOE_LOADER_RESOURCE_OBJECT}"
    PROPERTIES GENERATED TRUE EXTERNAL_OBJECT TRUE
  )
  set(SKYQOE_LOADER_RESOURCE_SOURCE "${SKYQOE_LOADER_RESOURCE_OBJECT}")
else()
  set_property(
    SOURCE "${CMAKE_CURRENT_BINARY_DIR}/skyqoe_loader.rc"
    APPEND PROPERTY OBJECT_DEPENDS "${SKYQOE_LOADER_PAYLOAD}"
  )
  set(SKYQOE_LOADER_RESOURCE_SOURCE
      "${CMAKE_CURRENT_BINARY_DIR}/skyqoe_loader.rc")
endif()

add_executable(SkyQoELoader WIN32
  tools/loader.cpp
  "${SKYQOE_LOADER_RESOURCE_SOURCE}"
)
add_dependencies(SkyQoELoader SkyQoELoaderPayload)
target_include_directories(SkyQoELoader PRIVATE src)
target_compile_definitions(SkyQoELoader PRIVATE
  UNICODE
  _UNICODE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  SKYQOE_PAYLOAD_VERSION=L"${PROJECT_VERSION}"
)
if(MINGW)
  target_compile_options(SkyQoELoader PRIVATE
    -Wall -Wextra -Wpedantic -finput-charset=UTF-8 -fexec-charset=UTF-8
  )
  target_link_options(SkyQoELoader PRIVATE -municode -static-libgcc -static-libstdc++)
endif()
target_link_libraries(SkyQoELoader PRIVATE shell32 ole32 user32)

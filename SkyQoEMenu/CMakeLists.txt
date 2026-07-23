cmake_minimum_required(VERSION 3.24)

project(SkyQoEMenu VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(SKYQOE_IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.tools/imgui" CACHE PATH "Dear ImGui source directory")
if(NOT EXISTS "${SKYQOE_IMGUI_DIR}/imgui.cpp")
  include(FetchContent)
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
  src/overlay.cpp
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

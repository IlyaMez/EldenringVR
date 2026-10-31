cmake_minimum_required(VERSION 3.21)
project(EldenRingVR VERSION 0.4.0 LANGUAGES C CXX RC)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "ELDEN RING is 64-bit; configure with a 64-bit toolchain (e.g. -A x64).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)

include(FetchContent)

# ---- MinHook (inline hooking) -------------------------------------------------
FetchContent_Declare(minhook
  GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
  GIT_TAG        v1.3.4
  GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(minhook)

# ---- ReShade headers (add-on API; ERVR.dll registers itself as a ReShade add-on) ----
# Header-only: reshade.hpp finds ReShade at runtime by scanning loaded modules, so we
# link nothing. BSD 3-clause - see THIRD_PARTY.md.
FetchContent_Declare(reshade
  GIT_REPOSITORY https://github.com/crosire/reshade.git
  GIT_TAG        v6.8.0
  GIT_SHALLOW    TRUE
  GIT_SUBMODULES ""                            # skip deps/, we only want include/
  SOURCE_SUBDIR  headers_only_do_not_build)    # non-existent subdir => headers only, no targets
FetchContent_MakeAvailable(reshade)
set(RESHADE_INCLUDE_DIR "${reshade_SOURCE_DIR}/include")

# ---- OpenXR loader (our own XrSession; static so the dist stays a single DLL) ----
set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
set(JSONCPP_STATIC_WINDOWS_RUNTIME ON CACHE BOOL "" FORCE)   # loader vendors jsoncpp
FetchContent_Declare(openxr
  GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
  GIT_TAG        release-1.1.62
  GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(openxr)
# The loader sets its own MSVC runtime, which would collide with ERVR's /MT (LNK2038).
foreach(_xr openxr_loader jsoncpp_object jsoncpp_static jsoncpp_lib)
  if(TARGET ${_xr})
    set_property(TARGET ${_xr} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  endif()
endforeach()

# ---- Version resources (file properties show the ERVR version; the packager checks it) ----
set(ERVR_RC_NAME "ERVR")
set(ERVR_RC_DESCRIPTION "ERVR - native VR for ELDEN RING (ReShade add-on, OpenXR)")
configure_file(src/version.rc.in ${CMAKE_BINARY_DIR}/ERVR_version.rc @ONLY)
set(ERVR_RC_NAME "dinput8")
set(ERVR_RC_DESCRIPTION "ERVR proxy loader (forwards DirectInput8Create, loads ERVR/ERVR.dll)")
configure_file(src/version.rc.in ${CMAKE_BINARY_DIR}/dinput8_version.rc @ONLY)

# ---- ERVR.dll: the mod --------------------------------------------------------
add_library(ERVR SHARED
  ${CMAKE_BINARY_DIR}/ERVR_version.rc
  src/ervr/main.cpp
  src/ervr/log.cpp
  src/ervr/config.cpp
  src/ervr/vr_openxr.cpp
  src/ervr/vr_backend.cpp
  src/ervr/reshade_addon.cpp
  src/ervr/dx12_blit.cpp
  src/ervr/stereo.cpp
  src/ervr/hud.cpp
  src/ervr/xinput_hook.cpp
  src/ervr/virtual_pad.cpp
  src/ervr/gestures.cpp
  src/ervr/game_memory.cpp
  src/ervr/midhook.cpp
  src/ervr/rtti.cpp
  src/ervr/frame_probe.cpp
  src/ervr/cam_watch.cpp
  src/ervr/autopilot.cpp
  src/ervr/debug_keys.cpp
  src/ervr/game_er.cpp
  src/ervr/game_version.cpp
  src/ervr/patches_er.cpp
  src/ervr/combat.cpp
  src/ervr/params_er.cpp
  src/ervr/crashlog.cpp
  ${minhook_SOURCE_DIR}/src/hde/hde64.c)          # length disassembler bundled with MinHook, reused for mid-function hooks
target_include_directories(ERVR PRIVATE src/ervr ${RESHADE_INCLUDE_DIR} ${minhook_SOURCE_DIR}/src/hde)
target_link_libraries(ERVR PRIVATE minhook openxr_loader d3d12 dxgi)
target_compile_definitions(ERVR PRIVATE ERVR_VERSION="${PROJECT_VERSION}")
set_target_properties(ERVR PROPERTIES OUTPUT_NAME "ERVR" PREFIX "")

# ---- xrprobe.exe: standalone OpenXR/D3D12 diagnostic -------------------------
add_executable(xrprobe tools/xrprobe.cpp)
target_link_libraries(xrprobe PRIVATE openxr_loader d3d12 dxgi)
set_target_properties(xrprobe PROPERTIES OUTPUT_NAME "xrprobe")

# ---- dinput8.dll: proxy loader ----------------------------------------------
add_library(dinput8_proxy SHARED
  src/proxy/dinput8.cpp
  src/proxy/dinput8.def
  ${CMAKE_BINARY_DIR}/dinput8_version.rc)
set_target_properties(dinput8_proxy PROPERTIES OUTPUT_NAME "dinput8" PREFIX "")

# ---- ERVR Launcher.exe: finds the game, installs ERVR into it, starts it with EAC off ----
# A Dear ImGui window (MIT, fetched); the OpenXR loader is linked for its headset check (`--probe`).
# No network code, like ERVR itself: links open in the browser, updates are a new zip.
option(ERVR_BUILD_LAUNCHER "Build ERVR Launcher.exe" ON)
if(ERVR_BUILD_LAUNCHER)
  FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.92.9b
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  headers_only_do_not_build)    # sources listed below, no targets of its own
  FetchContent_MakeAvailable(imgui)
  set(ERVR_LAUNCHER_SOURCES src/launcher/main.cpp src/launcher/ui.cpp src/launcher/system.cpp src/launcher/probe.cpp)
  file(WRITE ${CMAKE_BINARY_DIR}/launcher_version.h
    "#define ERVR_RC_VERSION \"${PROJECT_VERSION}\"\n"
    "#define ERVR_RC_VERSION_NUMBERS ${PROJECT_VERSION_MAJOR},${PROJECT_VERSION_MINOR},${PROJECT_VERSION_PATCH},0\n")
  add_executable(ervr_launcher WIN32
    ${ERVR_LAUNCHER_SOURCES}
    src/launcher/launcher.rc
    src/launcher/launcher.manifest
    ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)
  target_include_directories(ervr_launcher PRIVATE src/launcher src/ervr ${CMAKE_BINARY_DIR}
                             ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
  target_compile_definitions(ervr_launcher PRIVATE ERVR_VERSION="${PROJECT_VERSION}")
  set_source_files_properties(${ERVR_LAUNCHER_SOURCES} PROPERTIES COMPILE_OPTIONS "/W4;/permissive-")
  # The resource script compiles the fonts and the icon in; a changed one builds it again.
  file(GLOB ERVR_LAUNCHER_FONTS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/launcher/fonts/*.ttf")
  set_source_files_properties(src/launcher/launcher.rc PROPERTIES OBJECT_DEPENDS
                              "${ERVR_LAUNCHER_FONTS};${CMAKE_SOURCE_DIR}/src/launcher/ervr.ico")
  target_link_libraries(ervr_launcher PRIVATE openxr_loader d3d11 dxgi dwmapi version shell32 ole32 uuid advapi32)
  set_target_properties(ervr_launcher PROPERTIES OUTPUT_NAME "ERVR Launcher")
endif()

# Pure regression checks; no game process or headset required.
option(ERVR_BUILD_REGRESSION_TESTS "Build camera, pose, input, and action-queue regression tests" OFF)
if(ERVR_BUILD_REGRESSION_TESTS)
  enable_testing()
  foreach(_test head_anchor action_queue pose_frame two_hand_aim two_hand_grip weapon_motion weapon_contact body_pose virtual_pad movement_facing ride_state mounted_rider game_version game_res)
    add_executable(test_${_test} tools/test_${_test}.cpp)
    target_include_directories(test_${_test} PRIVATE src/ervr)
    add_test(NAME ${_test} COMMAND test_${_test})
  endforeach()
  target_sources(test_virtual_pad PRIVATE src/ervr/virtual_pad.cpp src/ervr/gestures.cpp src/ervr/config.cpp)
  # The launcher's ERVR.ini edits, log reading and Steam parsing.
  add_executable(test_launcher_text tools/test_launcher_text.cpp)
  target_include_directories(test_launcher_text PRIVATE src/launcher src/ervr)
  add_test(NAME launcher_text COMMAND test_launcher_text)
  # The launcher's install / park / flat / uninstall on a throwaway game folder, with this build's DLLs.
  add_executable(test_launcher_folder tools/test_launcher_folder.cpp src/launcher/system.cpp)
  target_include_directories(test_launcher_folder PRIVATE src/launcher src/ervr)
  file(TO_CMAKE_PATH "$ENV{SystemRoot}/System32/dxgi.dll" _foreign_dll)
  target_compile_definitions(test_launcher_folder PRIVATE
    ERVR_VERSION="${PROJECT_VERSION}"
    ERVR_TEST_DLL="$<TARGET_FILE:ERVR>"
    ERVR_TEST_PROXY="$<TARGET_FILE:dinput8_proxy>"
    ERVR_TEST_RESHADE="${CMAKE_SOURCE_DIR}/third_party/reshade/ReShade64.dll"
    ERVR_TEST_RESHADE_INI="${CMAKE_SOURCE_DIR}/third_party/reshade/ReShade.ini"
    ERVR_TEST_INI="${CMAKE_SOURCE_DIR}/config/ERVR.ini"
    ERVR_TEST_FOREIGN_DLL="${_foreign_dll}")
  target_link_libraries(test_launcher_folder PRIVATE dxgi version shell32 ole32 uuid advapi32)
  add_dependencies(test_launcher_folder ERVR dinput8_proxy)
  add_test(NAME launcher_folder COMMAND test_launcher_folder)
endif()

# ---- packaging ------------------------------------------------------------
set(DIST_DIR ${CMAKE_BINARY_DIR}/dist)
add_custom_target(dist ALL
  COMMAND ${CMAKE_COMMAND} -E make_directory ${DIST_DIR}/ERVR
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:dinput8_proxy> ${DIST_DIR}/dinput8.dll
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:ERVR>         ${DIST_DIR}/ERVR/ERVR.dll
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/config/ERVR.ini ${DIST_DIR}/ERVR/ERVR.ini
  # Bundled ReShade (add-on build, unmodified) - see third_party/reshade/README.md and THIRD_PARTY.md.
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/third_party/reshade/ReShade64.dll ${DIST_DIR}/dxgi.dll
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/third_party/reshade/ReShade.ini   ${DIST_DIR}/ReShade.ini
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/third_party/reshade/LICENSE.md    ${DIST_DIR}/LICENSE-ReShade.txt
  DEPENDS ERVR dinput8_proxy
  COMMENT "Assembling redistributable in ${DIST_DIR}")
if(ERVR_BUILD_LAUNCHER)
  add_custom_command(TARGET dist POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:ervr_launcher> "${DIST_DIR}/ERVR Launcher.exe"
    VERBATIM)
  add_dependencies(dist ervr_launcher)
endif()

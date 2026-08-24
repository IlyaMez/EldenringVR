cmake_minimum_required(VERSION 3.21)
project(EldenRingVR VERSION 0.3.0 LANGUAGES C CXX)

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

# ---- ERVR.dll: the mod --------------------------------------------------------
add_library(ERVR SHARED
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
  src/proxy/dinput8.def)
set_target_properties(dinput8_proxy PROPERTIES OUTPUT_NAME "dinput8" PREFIX "")

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
